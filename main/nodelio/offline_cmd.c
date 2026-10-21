#include "offline_cmd.h"

#include "offline_cmd_map.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "sdkconfig.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <model_path.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_NODELIO_PANEL

void nodelio_offline_cmd_set_done_cb(nodelio_offline_cmd_done_cb_t cb)
{
    (void)cb;
}
esp_err_t nodelio_offline_cmd_prewarm(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t nodelio_offline_cmd_begin_listen(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
void nodelio_offline_cmd_end_listen(void) {}
bool nodelio_offline_cmd_listening(void)
{
    return false;
}
void nodelio_offline_cmd_feed(const int16_t *data, int samples)
{
    (void)data;
    (void)samples;
}
void nodelio_offline_cmd_stop(void) {}
int nodelio_offline_cmd_dram_epoch(void) { return 0; }
bool nodelio_offline_cmd_yield_slot(void) { return false; }
void nodelio_offline_cmd_yield_for_cloud_audio(void) {}
void nodelio_offline_cmd_shield_up(void) {}
void nodelio_offline_cmd_finish_playback_dram(int epoch) { (void)epoch; }
void nodelio_offline_cmd_restore_after_media(int epoch) { (void)epoch; }
void nodelio_offline_cmd_reset_dynamic(void) {}
bool nodelio_offline_cmd_add_dynamic(const char *device_id, int channel_or_pin, bool is_pin,
                                    const char *action, const char *name, const char *pinyin)
{
    (void)device_id;
    (void)channel_or_pin;
    (void)is_pin;
    (void)action;
    (void)name;
    (void)pinyin;
    return false;
}
void nodelio_offline_cmd_commit_dynamic(void) {}
void nodelio_offline_cmd_rebuild_from_layout(const nodelio_panel_layout_t *layout) { (void)layout; }
int nodelio_offline_cmd_dynamic_count(void) { return 0; }
bool nodelio_offline_cmd_dynamic_at(int i, char *name, size_t name_len, char *action,
                                   size_t action_len)
{
    (void)i;
    (void)name;
    (void)name_len;
    (void)action;
    (void)action_len;
    return false;
}

#else

static const char *TAG = "nl_ocmd";

static nodelio_offline_cmd_done_cb_t s_done_cb;
static volatile bool s_session;   /* 口令窗进行中（含排队执行） */
static volatile bool s_listening; /* 仍接受 AFE feed */
static srmodel_list_t *s_models;
static bool s_owns_models;

/* 当前正在 detect 的实例。L1 可在播歌前拆掉；L2 开机建好后不拆。 */
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static esp_mn_iface_t *s_l1_mn;
static model_iface_data_t *s_l1_data;
static esp_mn_iface_t *s_l2_mn;
static model_iface_data_t *s_l2_data;

static QueueHandle_t s_cmd_q;
static TaskHandle_t s_cmd_task;
static StackType_t *s_cmd_stack;
static StaticTask_t s_cmd_tcb;
static int64_t s_deadline_us;
static int s_mn_div;
/** detect 与 destroy 跨任务：播歌 stop 不能拆正在跑的 MultiNet。 */
static portMUX_TYPE s_mn_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_in_detect;

#define CMD_STACK_WORDS 6144
/** 口令窗：须长于「唤醒后等网失败再进本地」的体感；5s 来不及说完 */
#define LISTEN_MS       12000

#define NL_DYN_CMD_MAX 32

typedef struct {
    int cmd_id;
    char device_id[32];
    int channel_or_pin;
    bool is_pin;
    char action[8]; // "ON" / "OFF"
    char name[32];
    char pinyin[48];
} nl_dyn_cmd_t;

static nl_dyn_cmd_t s_dyn_cmds[NL_DYN_CMD_MAX];
static int s_dyn_cmd_count = 0;

static const nl_dyn_cmd_t *find_dynamic_cmd(int cid)
{
    for (int i = 0; i < s_dyn_cmd_count; i++) {
        if (s_dyn_cmds[i].cmd_id == cid) {
            return &s_dyn_cmds[i];
        }
    }
    return NULL;
}

void nodelio_offline_cmd_reset_dynamic(void)
{
    s_dyn_cmd_count = 0;
}

bool nodelio_offline_cmd_add_dynamic(const char *device_id, int channel_or_pin, bool is_pin,
                                    const char *action, const char *name, const char *pinyin)
{
    if (!device_id || !device_id[0] || !action || !action[0] || !pinyin || !pinyin[0]) {
        return false;
    }
    /* 裸「打开/关闭」无辨识度，且会撞车；本机系统口令保留 */
    if (strcmp(pinyin, "da kai") == 0 || strcmp(pinyin, "guan bi") == 0 ||
        strcmp(pinyin, "chong qi") == 0 || strcmp(pinyin, "chong xin qi dong") == 0 ||
        strcmp(pinyin, "tui xia") == 0 || strcmp(pinyin, "zai jian") == 0 ||
        strcmp(pinyin, "bai bai") == 0 || strcmp(pinyin, "xi ping") == 0 ||
        strcmp(pinyin, "jin ru pei wang") == 0 || strcmp(pinyin, "da kai re dian") == 0 ||
        strcmp(pinyin, "pei wang mo shi") == 0 || strcmp(pinyin, "pei wang") == 0 ||
        strcmp(pinyin, "da kai shou ye") == 0 || strcmp(pinyin, "hui dao shou ye") == 0 ||
        strcmp(pinyin, "da kai ying yong") == 0 || strcmp(pinyin, "da kai she zhi") == 0 ||
        strcmp(pinyin, "da kai lian jie") == 0 || strcmp(pinyin, "wang luo she zhi") == 0 ||
        strcmp(pinyin, "xian shi yu sheng yin") == 0 || strcmp(pinyin, "da kai sheng dian") == 0 ||
        strcmp(pinyin, "kai sheng dian") == 0 || strcmp(pinyin, "guan sheng dian") == 0 ||
        strcmp(pinyin, "kai qi sheng dian") == 0 || strcmp(pinyin, "guan bi sheng dian") == 0 ||
        strcmp(pinyin, "da kai kong jia") == 0 || strcmp(pinyin, "guan bi kong jia") == 0 ||
        strcmp(pinyin, "da kai xiao zhi") == 0 || strcmp(pinyin, "guan bi xiao zhi") == 0 ||
        strcmp(pinyin, "kai qi xiao zhi") == 0 || strcmp(pinyin, "shua xin bu ju") == 0 ||
        strcmp(pinyin, "jian cha geng xin") == 0 || strcmp(pinyin, "hui fu chu chang") == 0 ||
        strcmp(pinyin, "yu yin zhu shou") == 0 || strcmp(pinyin, "bo fang qi she zhi") == 0 ||
        strcmp(pinyin, "kong jia she zhi") == 0 || strcmp(pinyin, "she bei xin xi") == 0 ||
        strcmp(pinyin, "shang yi shou") == 0 || strcmp(pinyin, "xia yi shou") == 0 ||
        strcmp(pinyin, "shang yi ping") == 0 || strcmp(pinyin, "xia yi ping") == 0 ||
        strcmp(pinyin, "wang qian fan") == 0 || strcmp(pinyin, "wang hou fan") == 0 ||
        strcmp(pinyin, "wang shang fan") == 0 || strcmp(pinyin, "wang xia fan") == 0 ||
        strcmp(pinyin, "xiang shang hua") == 0 || strcmp(pinyin, "xiang xia hua") == 0 ||
        strcmp(pinyin, "zan ting") == 0 || strcmp(pinyin, "ji xu bo fang") == 0 ||
        strcmp(pinyin, "guan bi bo fang qi") == 0 || strcmp(pinyin, "guan bi qu ku") == 0 ||
        strcmp(pinyin, "guan bi yin yue") == 0 || strcmp(pinyin, "guan diao yin yue") == 0 ||
        strcmp(pinyin, "tui chu bo fang qi") == 0 || strcmp(pinyin, "guan diao bo fang qi") == 0 ||
        strcmp(pinyin, "shou qi qu ku") == 0 ||
        strcmp(pinyin, "da kai qu ku") == 0 || strcmp(pinyin, "da kai yin yue") == 0 ||
        strcmp(pinyin, "da kai xin wen") == 0 || strcmp(pinyin, "da kai tian qi") == 0 ||
        strcmp(pinyin, "da kai dian tai") == 0 || strcmp(pinyin, "da kai bo ke") == 0 ||
        strcmp(pinyin, "da kai kong qi") == 0 || strcmp(pinyin, "da kai mei ri yi ju") == 0) {
        return false;
    }
    for (int i = 0; i < s_dyn_cmd_count; i++) {
        if (strcmp(s_dyn_cmds[i].pinyin, pinyin) == 0) {
            return false; /* 同音保留先注册的磁贴 */
        }
    }
    if (s_dyn_cmd_count >= NL_DYN_CMD_MAX) {
        return false;
    }
    nl_dyn_cmd_t *cmd = &s_dyn_cmds[s_dyn_cmd_count];
    cmd->cmd_id = NL_DYN_CMD_BASE + s_dyn_cmd_count;
    snprintf(cmd->device_id, sizeof(cmd->device_id), "%s", device_id);
    cmd->channel_or_pin = channel_or_pin;
    cmd->is_pin = is_pin;
    snprintf(cmd->action, sizeof(cmd->action), "%s", action);
    snprintf(cmd->name, sizeof(cmd->name), "%s", name ? name : device_id);
    snprintf(cmd->pinyin, sizeof(cmd->pinyin), "%s", pinyin);
    s_dyn_cmd_count++;
    return true;
}

int nodelio_offline_cmd_dynamic_count(void)
{
    return s_dyn_cmd_count;
}

bool nodelio_offline_cmd_dynamic_at(int i, char *name, size_t name_len, char *action,
                                   size_t action_len)
{
    if (i < 0 || i >= s_dyn_cmd_count || !name || name_len == 0 || !action || action_len == 0) {
        return false;
    }
    snprintf(name, name_len, "%s", s_dyn_cmds[i].name);
    snprintf(action, action_len, "%s", s_dyn_cmds[i].action);
    return true;
}

void nodelio_offline_cmd_rebuild_from_layout(const nodelio_panel_layout_t *layout)
{
    nodelio_offline_cmd_reset_dynamic();
    if (!nodelio_controls_enabled()) {
        nodelio_offline_cmd_commit_dynamic();
        ESP_LOGI(TAG, "voice cmds skipped: controls module off");
        return;
    }
    if (!layout) {
        nodelio_offline_cmd_commit_dynamic();
        return;
    }
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *page = &layout->pages[pi];
        for (int si = 0; si < page->section_count; si++) {
            const nl_section_t *sec = &page->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->type != NL_WIDGET_SWITCH_TILE && w->type != NL_WIDGET_PIN_TOGGLE) {
                    continue;
                }
                if (w->bind.kind != NL_BIND_DEVICE_CHANNEL && w->bind.kind != NL_BIND_DEVICE_PIN) {
                    continue;
                }
                if (w->bind.device_id[0] == '\0') {
                    continue;
                }
                bool is_pin = (w->bind.kind == NL_BIND_DEVICE_PIN);
                int idx = is_pin ? w->bind.pin : w->bind.channel;
                if (idx < 0) {
                    idx = 0;
                }
                const char *name = w->title[0] ? w->title : w->bind.device_id;
                if (w->pinyin_on[0]) {
                    nodelio_offline_cmd_add_dynamic(w->bind.device_id, idx, is_pin, "ON", name, w->pinyin_on);
                }
                if (w->pinyin_off[0]) {
                    nodelio_offline_cmd_add_dynamic(w->bind.device_id, idx, is_pin, "OFF", name, w->pinyin_off);
                }
            }
        }
    }
    nodelio_offline_cmd_commit_dynamic();
    ESP_LOGI(TAG, "voice cmds from layout: %d", s_dyn_cmd_count);
}

static const nl_dyn_cmd_t *find_dynamic_cmd(int cid);
static void install_commands_level1(void);  /* L1: 全功能命令集 */
static void install_commands_level2(void);  /* L2: 应急模式命令集（极简） */
static esp_err_t ensure_multinet_level(int level);

void nodelio_offline_cmd_commit_dynamic(void)
{
    /* 动态词只进 L1。命令表是全局的，更新前必须重新绑到 L1，避免写成 L2。 */
    if (s_l1_mn && s_l1_data) {
        (void)esp_mn_commands_alloc(s_l1_mn, s_l1_data);
        install_commands_level1();
    }
}

void nodelio_offline_cmd_set_done_cb(nodelio_offline_cmd_done_cb_t cb)
{
    s_done_cb = cb;
}

static void install_commands_level1(void)
{
    esp_mn_commands_clear();
    /* 本机系统口令：断网也可执行 */
    static const struct {
        int id;
        const char *py;
    } k_local[] = {
        {NL_OC_REBOOT, "chong qi"},
        {NL_OC_REBOOT, "chong xin qi dong"},
        {NL_OC_WIFI_CONFIG, "jin ru pei wang"},
        {NL_OC_WIFI_CONFIG, "da kai re dian"},
        {NL_OC_WIFI_CONFIG, "pei wang mo shi"},
        {NL_OC_WIFI_CONFIG, "pei wang"},
        {NL_OC_GOTO_HOME, "da kai shou ye"},
        {NL_OC_GOTO_HOME, "hui dao shou ye"},
        {NL_OC_GOTO_APPS, "da kai ying yong"},
        {NL_OC_GOTO_SETTINGS, "da kai she zhi"},
        {NL_OC_GOTO_CONNECT, "da kai lian jie"},
        {NL_OC_GOTO_CONNECT, "wang luo she zhi"},
        {NL_OC_GOTO_DISP, "xian shi yu sheng yin"},
        {NL_OC_GOTO_POWER, "da kai sheng dian"},
        {NL_OC_POWER_SAVE_ON, "kai sheng dian"},
        {NL_OC_POWER_SAVE_ON, "kai qi sheng dian"},
        {NL_OC_POWER_SAVE_OFF, "guan sheng dian"},
        {NL_OC_POWER_SAVE_OFF, "guan bi sheng dian"},
        {NL_OC_GOTO_VOICE, "yu yin zhu shou"},
        {NL_OC_GOTO_MEDIA, "bo fang qi she zhi"},
        {NL_OC_GOTO_CTRL, "kong jia she zhi"},
        {NL_OC_CONTROLS_ON, "da kai kong jia"},
        {NL_OC_CONTROLS_OFF, "guan bi kong jia"},
        {NL_OC_XIAOZHI_ON, "da kai xiao zhi"},
        {NL_OC_XIAOZHI_ON, "kai qi xiao zhi"},
        {NL_OC_XIAOZHI_OFF, "guan bi xiao zhi"},
        {NL_OC_LAYOUT_SYNC, "shua xin bu ju"},
        {NL_OC_FW_CHECK, "jian cha geng xin"},
        {NL_OC_GOTO_RESET, "hui fu chu chang"},
        {NL_OC_GOTO_SYS, "she bei xin xi"},
        {NL_OC_GOODBYE, "tui xia"},
        {NL_OC_GOODBYE, "zai jian"},
        {NL_OC_GOODBYE, "bai bai"},
        {NL_OC_GOODBYE, "xi ping"},
        {NL_OC_MEDIA_PREV, "shang yi shou"},
        {NL_OC_MEDIA_NEXT, "xia yi shou"},
        {NL_OC_PAGE_PREV, "shang yi ping"},
        {NL_OC_PAGE_PREV, "wang qian fan"},
        {NL_OC_PAGE_NEXT, "xia yi ping"},
        {NL_OC_PAGE_NEXT, "wang hou fan"},
        {NL_OC_SCROLL_UP, "wang shang fan"},
        {NL_OC_SCROLL_UP, "xiang shang hua"},
        {NL_OC_SCROLL_DOWN, "wang xia fan"},
        {NL_OC_SCROLL_DOWN, "xiang xia hua"},
        {NL_OC_MEDIA_PAUSE, "zan ting"},
        {NL_OC_MEDIA_RESUME, "ji xu bo fang"},
        {NL_OC_SKILL_CLOSE, "guan bi bo fang qi"},
        {NL_OC_SKILL_CLOSE, "guan bi qu ku"},
        {NL_OC_SKILL_CLOSE, "guan bi yin yue"},
        {NL_OC_SKILL_CLOSE, "guan diao yin yue"},
        {NL_OC_SKILL_CLOSE, "tui chu bo fang qi"},
        {NL_OC_SKILL_CLOSE, "guan diao bo fang qi"},
        {NL_OC_SKILL_CLOSE, "shou qi qu ku"},
        {NL_OC_APP_MUSIC, "da kai qu ku"},
        {NL_OC_APP_MUSIC, "da kai yin yue"},
        {NL_OC_APP_NEWS, "da kai xin wen"},
        {NL_OC_APP_WEATHER, "da kai tian qi"},
        {NL_OC_APP_RADIO, "da kai dian tai"},
        {NL_OC_APP_PODCAST, "da kai bo ke"},
        {NL_OC_APP_AIR, "da kai kong qi"},
        {NL_OC_APP_QUOTE, "da kai mei ri yi ju"},
    };
    static const struct {
        int id;
        const char *py;
    } k_ctrl_off[] = {
        {NL_OC_CTRL_OFF, "kai deng"},
        {NL_OC_CTRL_OFF, "guan deng"},
        {NL_OC_CTRL_OFF, "hui jia"},
        {NL_OC_CTRL_OFF, "chu men"},
    };
    const bool ctrl_off = !nodelio_controls_enabled();
    for (int i = 0; i < (int)(sizeof(k_local) / sizeof(k_local[0])); i++) {
        esp_err_t ret = esp_mn_commands_add(k_local[i].id, k_local[i].py);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "local voice cmd: id=%d pinyin='%s'", k_local[i].id, k_local[i].py);
        } else {
            ESP_LOGW(TAG, "Fail to add local voice cmd: '%s'", k_local[i].py);
        }
    }
    if (ctrl_off) {
        for (int i = 0; i < (int)(sizeof(k_ctrl_off) / sizeof(k_ctrl_off[0])); i++) {
            esp_err_t ret = esp_mn_commands_add(k_ctrl_off[i].id, k_ctrl_off[i].py);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Fail to add ctrl-off voice cmd: '%s'", k_ctrl_off[i].py);
            }
        }
    }
    /* 布局下发的动态拼音（控家仍走 panel/command，需联网） */
    for (int i = 0; i < s_dyn_cmd_count; i++) {
        esp_err_t ret = esp_mn_commands_add(s_dyn_cmds[i].cmd_id, s_dyn_cmds[i].pinyin);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Dynamic voice cmd registered: id=%d pinyin='%s' -> dev=%s idx=%d pin=%d act=%s",
                     s_dyn_cmds[i].cmd_id, s_dyn_cmds[i].pinyin, s_dyn_cmds[i].device_id,
                     s_dyn_cmds[i].channel_or_pin, s_dyn_cmds[i].is_pin ? 1 : 0, s_dyn_cmds[i].action);
        } else {
            ESP_LOGW(TAG, "Fail to add dynamic voice cmd: '%s'", s_dyn_cmds[i].pinyin);
        }
    }

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        ESP_LOGW(TAG, "mn commands update has %d error phrases", err->num);
    }
    if (s_l1_mn && s_l1_data) {
        s_l1_mn->print_active_speech_commands(s_l1_data);
    }
}

/* L2 应急模式：极简命令集（内存需求最低） */
static void install_commands_level2(void)
{
    esp_mn_commands_clear();
    /* 核心离线命令：重启 / 配网 / 退下 / 首页 / 设置 */
    static const struct {
        int id;
        const char *py;
    } k_l2[] = {
        {NL_OC_REBOOT, "chong qi"},
        {NL_OC_REBOOT, "chong xin qi dong"},
        {NL_OC_WIFI_CONFIG, "pei wang"},
        {NL_OC_WIFI_CONFIG, "jin ru pei wang"},
        {NL_OC_GOODBYE, "tui xia"},
        {NL_OC_GOODBYE, "zai jian"},
        {NL_OC_GOODBYE, "bai bai"},
        {NL_OC_GOTO_HOME, "da kai shou ye"},
        {NL_OC_GOTO_HOME, "hui dao shou ye"},
        {NL_OC_GOTO_SETTINGS, "da kai she zhi"},
        {NL_OC_GOTO_SYS, "she bei xin xi"},
    };
    
    for (int i = 0; i < (int)(sizeof(k_l2) / sizeof(k_l2[0])); i++) {
        esp_err_t ret = esp_mn_commands_add(k_l2[i].id, k_l2[i].py);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "L2 cmd: id=%d pinyin='%s'", k_l2[i].id, k_l2[i].py);
        } else {
            ESP_LOGW(TAG, "Fail to add L2 cmd: '%s'", k_l2[i].py);
        }
    }

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        ESP_LOGW(TAG, "L2 mn commands update has %d error phrases", err->num);
    }
}

/* 由 application.cc 提供：复用 assets 已 load 的 srmodels，勿再找已删除的 model 分区 */
extern srmodel_list_t *nodelio_get_srmodels(void);

/* 开机最大连续内部块约 60KB，只够一份 MultiNet。播歌后堆会被切到约 8KB，建不回来。
 * 模型从 prewarm 一直留到重启，stop() 只结束口令窗，不 destroy。 */

static void *s_slot;
static size_t s_slot_size;
static void *s_shield;
static int s_shield_epoch;
static int s_dram_epoch;
static int s_yield_epoch = -1;

static void reserve_slot(void)
{
    if (s_slot || (s_mn && s_mn_data)) {
        return;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < 48 * 1024) {
        ESP_LOGW(TAG, "dram slot miss largest=%u", (unsigned)largest);
        return;
    }
    size_t n = largest;
    if (n > 49 * 1024) {
        n -= 1024;
    }
    s_slot = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_slot_size = s_slot ? n : 0;
    if (!s_slot) {
        ESP_LOGW(TAG, "dram slot malloc %u failed", (unsigned)n);
        return;
    }
    ESP_LOGI(TAG, "dram slot held %u", (unsigned)s_slot_size);
}

static void free_slot(void)
{
    if (!s_slot) {
        return;
    }
    heap_caps_free(s_slot);
    s_slot = NULL;
    s_slot_size = 0;
}

static void free_shield_if(int epoch)
{
    if (!s_shield || s_shield_epoch != epoch) {
        return;
    }
    heap_caps_free(s_shield);
    s_shield = NULL;
}

int nodelio_offline_cmd_dram_epoch(void)
{
    return s_dram_epoch;
}

bool nodelio_offline_cmd_yield_slot(void)
{
    if (!s_slot) {
        return false;
    }
    ESP_LOGI(TAG, "dram slot yield %u", (unsigned)s_slot_size);
    s_yield_epoch = s_dram_epoch;
    free_slot();
    return true;
}

void nodelio_offline_cmd_yield_for_cloud_audio(void)
{
    free_shield_if(s_shield_epoch);
    const bool y = nodelio_offline_cmd_yield_slot();
    const size_t free_i = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "yield for cloud audio slot=%d free=%u largest=%u", y ? 1 : 0, (unsigned)free_i,
             (unsigned)largest);
}

void nodelio_offline_cmd_shield_up(void)
{
    if (s_yield_epoch != s_dram_epoch || s_shield || s_slot || (s_mn && s_mn_data)) {
        return;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < 20 * 1024) {
        return;
    }
    const size_t n = largest - 8 * 1024;
    s_shield = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_shield) {
        return;
    }
    s_shield_epoch = s_dram_epoch;
    ESP_LOGI(TAG, "dram shield %u leave=8192", (unsigned)n);
}

void nodelio_offline_cmd_finish_playback_dram(int epoch)
{
    free_shield_if(epoch);
    /* 换曲时 stop() 已经把 epoch 加一，这里仍要把刚释放的连续块占住，
     * 否则下一首的 yield 拿到的是一块已经被切碎的堆。 */
    if (s_mn && s_mn_data) {
        return;
    }
    reserve_slot();
}

void nodelio_offline_cmd_restore_after_media(int epoch)
{
    if (epoch != s_dram_epoch) {
        return;
    }
    free_shield_if(epoch);
    if (epoch != s_dram_epoch) {
        return;
    }
    if (s_mn && s_mn_data) {
        return;
    }
    if (!nodelio_local_cmd_enabled() || nodelio_xiaozhi_enabled()) {
        return;
    }
    
    /* L2 占着唯一的大块。播歌后堆已碎，拆掉 L2 也拼不出 48KB，不要在这里升级。 */
    if (s_l2_mn && s_l2_data) {
        ESP_LOGI(TAG, "after media keep L2");
        return;
    }
    esp_err_t err = ensure_multinet_level(1);
    if (err != ESP_OK) {
        err = ensure_multinet_level(2);
    }
    ESP_LOGI(TAG, "after media rebuild %s", esp_err_to_name(err));
}

/* L1 门槛 48KB；L2 词少，门槛 20KB。已建好的实例直接复用，不再看空闲块。 */
static esp_err_t ensure_multinet_level(int level)
{
    if (level == 1 && s_l1_mn && s_l1_data) {
        return ESP_OK;
    }
    if (level == 2 && s_l2_mn && s_l2_data) {
        return ESP_OK;
    }
    if (level != 1 && level != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    /* L1 的占位块要先还回堆，create 才能用到那块连续内存。失败再占回去。 */
    if (level == 1) {
        free_slot();
    }

    const size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const size_t need_dram = (level == 1) ? 48 * 1024 : 20 * 1024;
    if (dram_largest < need_dram || psram_largest < 160 * 1024) {
        ESP_LOGW(TAG, "multinet L%d skipped: dram largest=%u need=%u; psram largest=%u",
                 level, (unsigned)dram_largest, (unsigned)need_dram, (unsigned)psram_largest);
        if (level == 1) {
            reserve_slot();
        }
        return ESP_ERR_NO_MEM;
    }

    if (!s_models) {
        s_models = nodelio_get_srmodels();
        s_owns_models = false;
        if (!s_models) {
            s_models = esp_srmodel_init("model");
            s_owns_models = s_models != NULL;
        }
        if (!s_models) {
            ESP_LOGE(TAG, "no srmodels (assets 未加载 / 无 model 分区)");
            if (level == 1) {
                reserve_slot();
            }
            return ESP_ERR_NOT_FOUND;
        }
    }

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, NULL);
    }
    if (!mn_name) {
        ESP_LOGW(TAG, "no MultiNet in srmodels — 需 CONFIG_SR_MN_CN_MULTINET6_QUANT 并重烧 assets");
        if (level == 1) {
            reserve_slot();
        }
        return ESP_ERR_NOT_FOUND;
    }

    esp_mn_iface_t *mn = esp_mn_handle_from_name(mn_name);
    if (!mn) {
        if (level == 1) {
            reserve_slot();
        }
        return ESP_FAIL;
    }

    const size_t before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "creating multinet L%d dram free=%u largest=%u", level,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)before);

    model_iface_data_t *mn_data = mn->create(mn_name, LISTEN_MS);
    if (!mn_data) {
        ESP_LOGE(TAG, "multinet L%d create failed (DRAM?)", level);
        if (level == 1) {
            reserve_slot();
        }
        return ESP_ERR_NO_MEM;
    }

    if (mn->switch_loader_mode) {
        model_iface_data_t *sw = mn->switch_loader_mode(mn_data, ESP_MN_LOAD_FROM_FLASH);
        if (sw) {
            mn_data = sw;
        }
    }

    /* alloc 会丢掉上一次的短语链表，但已经 update 进模型里的 FST 还在。先绑再装词。 */
    (void)esp_mn_commands_alloc(mn, mn_data);
    if (level == 1) {
        s_l1_mn = mn;
        s_l1_data = mn_data;
        install_commands_level1();
    } else {
        s_l2_mn = mn;
        s_l2_data = mn_data;
        install_commands_level2();
    }
    const size_t after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "MultiNet L%d ready %s largest %u -> %u", level, mn_name, (unsigned)before,
             (unsigned)after);
    return ESP_OK;
}

/* 激活指定级别的模型进行聆听 */
static esp_err_t activate_listening_mode(int level)
{
    if (level == 1) {
        s_mn = s_l1_mn;
        s_mn_data = s_l1_data;
    } else if (level == 2) {
        s_mn = s_l2_mn;
        s_mn_data = s_l2_data;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mn && s_mn_data) {
        s_mn->clean(s_mn_data);
        s_mn_div = 0;
        s_deadline_us = esp_timer_get_time() + (int64_t)LISTEN_MS * 1000;
        s_session = true;
        s_listening = true;
        nodelio_panel_ui_note_activity();
        ESP_LOGI(TAG, "activated listening mode L%d dyn=%d", level, s_dyn_cmd_count);
    }
    
    return s_mn && s_mn_data ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static void finish_listen(const char *hint, bool ok)
{
    if (!s_session) {
        return;
    }
    s_session = false;
    s_listening = false;
    if (hint && hint[0]) {
        nodelio_panel_ui_set_voice_hint(hint);
    }
    nodelio_panel_ui_play_cue(ok ? 1 : 2);
    if (s_done_cb) {
        s_done_cb();
    }
}

static bool cmd_ends_session(int cid)
{
    /* 切歌会马上拆 MultiNet 给解码器。窗口若续开，AFE 会在 destroy 途中再进 detect。 */
    return cid == NL_OC_GOODBYE || cid == NL_OC_REBOOT || cid == NL_OC_WIFI_CONFIG ||
           cid == NL_OC_XIAOZHI_ON || cid == NL_OC_XIAOZHI_OFF || cid == NL_OC_MEDIA_PREV ||
           cid == NL_OC_MEDIA_NEXT || cid == NL_OC_MEDIA_PAUSE || cid == NL_OC_MEDIA_RESUME ||
           cid == NL_OC_SKILL_CLOSE;
}

/** 一条口令执行完后继续听，直到超时或退下/重启/配网。 */
static void continue_listen(const char *hint, bool ok)
{
    if (!s_session) {
        return;
    }
    if (hint && hint[0]) {
        nodelio_panel_ui_set_voice_hint(hint);
    }
    nodelio_panel_ui_play_cue(ok ? 1 : 2);
    nodelio_panel_ui_note_activity();
    if (s_mn && s_mn_data) {
        s_mn->clean(s_mn_data);
    }
    s_mn_div = 0;
    s_deadline_us = esp_timer_get_time() + (int64_t)LISTEN_MS * 1000;
    s_session = true;
    s_listening = true;
    ESP_LOGI(TAG, "local cmd stay open +%dms", LISTEN_MS);
}

static void after_cmd(int cid, const char *hint, bool ok)
{
    if (cmd_ends_session(cid)) {
        finish_listen(hint, ok);
        return;
    }
    continue_listen(hint, ok);
}

static void cmd_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int cid = 0;
        if (xQueueReceive(s_cmd_q, &cid, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cid <= 0) {
            continue;
        }
        /* 本机口令：不查服务器 / 不走 HTTP */
        if (nodelio_offline_cmd_is_local(cid)) {
            esp_err_t err = nodelio_offline_cmd_dispatch(NULL, cid, NULL);
            const char *zh = nodelio_offline_cmd_zh(cid);
            if (cid == NL_OC_PAGE_NEXT || cid == NL_OC_PAGE_PREV ||
                cid == NL_OC_SCROLL_DOWN || cid == NL_OC_SCROLL_UP) {
                /* dispatch 已写入边界文案 */
                after_cmd(cid, NULL, err == ESP_OK);
            } else if (err == ESP_OK) {
                after_cmd(cid, zh ? zh : "已执行", true);
            } else {
                after_cmd(cid, "执行失败", false);
            }
            continue;
        }
        /* PSRAM 栈任务禁止 nvs_load（flash 关 cache 会 assert）；用 RAM 镜像 */
        nodelio_nvs_config_t cfg;
        nodelio_nvs_cache_get(&cfg);
        if (cfg.server_url[0] == '\0') {
            nodelio_panel_ui_set_voice_hint("未绑定服务器");
            finish_listen(NULL, false);
            continue;
        }
        if (cid >= NL_DYN_CMD_BASE) {
            const nl_dyn_cmd_t *dyn = find_dynamic_cmd(cid);
            if (dyn) {
                nl_bind_t bind;
                memset(&bind, 0, sizeof(bind));
                bind.kind = dyn->is_pin ? NL_BIND_DEVICE_PIN : NL_BIND_DEVICE_CHANNEL;
                snprintf(bind.device_id, sizeof(bind.device_id), "%s", dyn->device_id);
                if (dyn->is_pin) {
                    bind.pin = dyn->channel_or_pin;
                    bind.channel = -1;
                } else {
                    bind.channel = dyn->channel_or_pin;
                    bind.pin = -1;
                }
                int http = 0;
                esp_err_t err = nodelio_panel_command(&cfg, "dyn_voice", "set", &bind, dyn->action, &http);
                if (err == ESP_OK) {
                    char t[64];
                    snprintf(t, sizeof(t), "已%s：%s", strcmp(dyn->action, "ON") == 0 ? "开启" : "关闭", dyn->name);
                    nodelio_panel_ui_toast(t);
                    after_cmd(cid, t, true);
                } else if (err == ESP_ERR_INVALID_STATE || http == 503) {
                    after_cmd(cid, "设备离线", false);
                } else {
                    after_cmd(cid, "执行失败", false);
                }
                continue;
            }
        }
        const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
        esp_err_t err = nodelio_offline_cmd_dispatch(&cfg, cid, layout);
        const char *zh = nodelio_offline_cmd_zh(cid);
        if (err == ESP_OK) {
            char t[48];
            snprintf(t, sizeof(t), "已%s", zh ? zh : "执行");
            nodelio_panel_ui_toast(t);
            after_cmd(cid, t, true);
        } else if (err == ESP_ERR_INVALID_STATE) {
            after_cmd(cid, "设备离线", false);
        } else {
            after_cmd(cid, "执行失败", false);
        }
    }
}

static esp_err_t ensure_cmd_worker(void)
{
    if (s_cmd_q && s_cmd_task) {
        return ESP_OK;
    }
    if (!s_cmd_q) {
        s_cmd_q = xQueueCreate(4, sizeof(int));
        if (!s_cmd_q) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_cmd_stack) {
        s_cmd_stack = heap_caps_malloc(CMD_STACK_WORDS * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        if (!s_cmd_stack) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_cmd_task) {
        s_cmd_task = xTaskCreateStaticPinnedToCore(cmd_worker, "nl_oc_cmd", CMD_STACK_WORDS, NULL, 3,
                                                   s_cmd_stack, &s_cmd_tcb, 1);
        if (!s_cmd_task) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t nodelio_offline_cmd_prewarm(void)
{
    if (!nodelio_local_cmd_enabled()) {
        ESP_LOGI(TAG, "prewarm skipped (local_cmd off)");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "prewarm begin");
    /* 60KB 连续块只够一份模型。先建全量 L1；失败再退到 L2。 */
    const esp_err_t e1 = ensure_multinet_level(1);
    if (e1 == ESP_OK) {
        ESP_LOGI(TAG, "prewarm L1");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "L1 prewarm failed: %s", esp_err_to_name(e1));
    const esp_err_t e2 = ensure_multinet_level(2);
    if (e2 != ESP_OK) {
        ESP_LOGW(TAG, "L2 prewarm failed: %s", esp_err_to_name(e2));
    }
    return e2 == ESP_OK ? ESP_OK : e1;
}

esp_err_t nodelio_offline_cmd_begin_listen(void)
{
    if (s_session) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nodelio_local_cmd_enabled()) {
        nodelio_panel_ui_toast("本地口令未启用（设置 → 语音助手）");
        ESP_LOGW(TAG, "begin_listen blocked: local_cmd off");
        return ESP_ERR_INVALID_STATE;
    }

    /* 未绑定/未注册仍可听本机系统口令（重启 / 进入配网）；控家口令需绑定+布局 */
    nodelio_nvs_config_t cfg;
    nodelio_nvs_cache_get(&cfg);
    const bool provisioned = cfg.server_url[0] != '\0' && cfg.owner[0] != '\0';
    const bool registered = cfg.device_id[0] != '\0' && cfg.device_key[0] != '\0';
    const bool have_layout = nodelio_panel_ui_get_layout() != NULL;
    if (!provisioned) {
        ESP_LOGW(TAG, "begin_listen: not provisioned — system cmds only");
    } else if (!registered) {
        ESP_LOGW(TAG, "begin_listen: not registered — system cmds only");
    }

    /* 已有的实例直接用。L2 占着唯一大块时不要再尝试建 L1。 */
    int selected_level = 0;
    if (s_l1_mn && s_l1_data) {
        selected_level = 1;
    } else if (s_l2_mn && s_l2_data) {
        selected_level = 2;
    } else if (ensure_multinet_level(1) == ESP_OK) {
        selected_level = 1;
    } else if (ensure_multinet_level(2) == ESP_OK) {
        selected_level = 2;
    }

    if (!selected_level) {
        nodelio_panel_ui_set_voice_hint("口令模型不可用");
        nodelio_panel_ui_toast("离线语音不可用");
        ESP_LOGW(TAG, "begin_listen failed: no multinet (dram largest=%u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = activate_listening_mode(selected_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "activate_listening_mode failed for L%d", selected_level);
        return err;
    }
    
    err = ensure_cmd_worker();
    if (err != ESP_OK) {
        nodelio_panel_ui_toast("本地口令启动失败");
        return err;
    }
    
    /* 显示提示信息 */
    if (!provisioned || !registered) {
        nodelio_panel_ui_set_voice_hint("可说重启、配网、打开首页/设置");
        nodelio_panel_ui_toast(provisioned ? "未注册：重启/配网/切页" : "未绑定：重启/配网/切页");
    } else if (selected_level == 2) {
        /* 应急模式提示 */
        nodelio_panel_ui_set_voice_hint("应急模式：重启/配网/退下");
        nodelio_panel_ui_toast("触控失效时可用这些命令");
    } else if (s_dyn_cmd_count > 0) {
        nodelio_panel_ui_set_voice_hint("可连续说，退下或停顿后结束");
        nodelio_panel_ui_toast("本地口令");
    } else if (have_layout) {
        nodelio_panel_ui_set_voice_hint("可说打开首页/设置/应用");
        nodelio_panel_ui_toast("本地口令（切页/重启/配网）");
    } else {
        nodelio_panel_ui_set_voice_hint("可说打开首页/设置/应用");
        nodelio_panel_ui_toast("无布局：切页/重启/配网");
    }
    
    nodelio_panel_ui_play_cue(0); /* 与小智进听同款 POPUP */
    ESP_LOGI(TAG, "local command window %dms level=%d dyn=%d layout=%d prov=%d reg=%d", 
             LISTEN_MS, selected_level, s_dyn_cmd_count, have_layout ? 1 : 0, 
             provisioned ? 1 : 0, registered ? 1 : 0);
    return ESP_OK;
}

void nodelio_offline_cmd_end_listen(void)
{
    finish_listen(NULL, false);
}

bool nodelio_offline_cmd_listening(void)
{
    return s_listening;
}

void nodelio_offline_cmd_feed(const int16_t *data, int samples)
{
    if (!data || samples <= 0) {
        return;
    }
    if (esp_timer_get_time() > s_deadline_us) {
        if (!s_listening) {
            return;
        }
        ESP_LOGI(TAG, "mn timeout");
        finish_listen("没听清，再说唤醒词", false);
        return;
    }
    /* 隔帧跑 MultiNet，减轻 FEED/CPU 压力 */
    if ((++s_mn_div & 1) != 0) {
        return;
    }

    esp_mn_iface_t *mn = NULL;
    model_iface_data_t *mn_data = NULL;
    portENTER_CRITICAL(&s_mn_mux);
    if (s_listening && s_mn && s_mn_data) {
        s_in_detect = 1;
        mn = s_mn;
        mn_data = s_mn_data;
    }
    portEXIT_CRITICAL(&s_mn_mux);
    if (!mn || !mn_data) {
        return;
    }

    int chunk = mn->get_samp_chunksize(mn_data);
    if (chunk <= 0 || samples < chunk) {
        portENTER_CRITICAL(&s_mn_mux);
        s_in_detect = 0;
        portEXIT_CRITICAL(&s_mn_mux);
        return;
    }
    /* 取末尾对齐 chunk（AFE fetch 通常正好一块） */
    const int16_t *p = data + (samples - chunk);
    esp_mn_state_t st = mn->detect(mn_data, (int16_t *)p);
    esp_mn_results_t *r = NULL;
    if (st == ESP_MN_STATE_DETECTED) {
        r = mn->get_results(mn_data);
    }
    portENTER_CRITICAL(&s_mn_mux);
    s_in_detect = 0;
    const bool dropped = !s_listening || s_mn_data != mn_data;
    if (!dropped && st == ESP_MN_STATE_DETECTED) {
        s_listening = false;
    }
    portEXIT_CRITICAL(&s_mn_mux);
    if (dropped) {
        return;
    }
    if (st == ESP_MN_STATE_DETECTING) {
        return;
    }
    if (st == ESP_MN_STATE_TIMEOUT) {
        ESP_LOGI(TAG, "mn state timeout");
        finish_listen("没听清，再说唤醒词", false);
        return;
    }
    if (st != ESP_MN_STATE_DETECTED) {
        return;
    }
    if (!r || r->num <= 0) {
        finish_listen("没听清", false);
        return;
    }
    int cid = r->command_id[0];
    const char *zh = nodelio_offline_cmd_zh(cid);
    ESP_LOGI(TAG, "mn cmd=%d str=%s zh=%s", cid, r->string ? r->string : "", zh ? zh : "");
    if (s_cmd_q && xQueueSend(s_cmd_q, &cid, 0) == pdTRUE) {
        char t[48];
        snprintf(t, sizeof(t), "%s…", zh ? zh : "执行");
        nodelio_panel_ui_set_voice_hint(t);
        /* finish_listen 在 cmd_worker 完成后调用 */
    } else {
        finish_listen("忙，请重试", false);
    }
}

void nodelio_offline_cmd_stop(void)
{
    portENTER_CRITICAL(&s_mn_mux);
    const bool had_session = s_session || s_listening;
    s_session = false;
    s_listening = false;
    s_mn = NULL;
    s_mn_data = NULL;
    portEXIT_CRITICAL(&s_mn_mux);

    /* 等 AFE 退出 detect。模型实例留着，不能 destroy，否则播歌会把那块切碎。 */
    for (int i = 0; i < 40; i++) {
        portENTER_CRITICAL(&s_mn_mux);
        const int busy = s_in_detect;
        portEXIT_CRITICAL(&s_mn_mux);
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (had_session && s_done_cb) {
        s_done_cb();
    }

    ESP_LOGI(TAG, "offline cmd stop keep L1=%d L2=%d dram largest=%u",
             (s_l1_mn && s_l1_data) ? 1 : 0, (s_l2_mn && s_l2_data) ? 1 : 0,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

#endif /* CONFIG_NODELIO_PANEL */
