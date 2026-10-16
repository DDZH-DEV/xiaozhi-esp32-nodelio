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

#else

static const char *TAG = "nl_ocmd";

static nodelio_offline_cmd_done_cb_t s_done_cb;
static volatile bool s_session;   /* 口令窗进行中（含排队执行） */
static volatile bool s_listening; /* 仍接受 AFE feed */
static srmodel_list_t *s_models;
static bool s_owns_models;
static esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static QueueHandle_t s_cmd_q;
static TaskHandle_t s_cmd_task;
static StackType_t *s_cmd_stack;
static StaticTask_t s_cmd_tcb;
static int64_t s_deadline_us;
static int s_mn_div;

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
        strcmp(pinyin, "chong qi") == 0 || strcmp(pinyin, "tui xia") == 0 ||
        strcmp(pinyin, "zai jian") == 0 || strcmp(pinyin, "bai bai") == 0 ||
        strcmp(pinyin, "xi ping") == 0 || strcmp(pinyin, "shang yi shou") == 0 ||
        strcmp(pinyin, "xia yi shou") == 0 || strcmp(pinyin, "zan ting") == 0 ||
        strcmp(pinyin, "ji xu bo fang") == 0) {
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

void nodelio_offline_cmd_rebuild_from_layout(const nodelio_panel_layout_t *layout)
{
    nodelio_offline_cmd_reset_dynamic();
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

static void install_commands(void);

void nodelio_offline_cmd_commit_dynamic(void)
{
    if (s_mn && s_mn_data) {
        install_commands();
    }
}

void nodelio_offline_cmd_set_done_cb(nodelio_offline_cmd_done_cb_t cb)
{
    s_done_cb = cb;
}

static void install_commands(void)
{
    esp_mn_commands_clear();
    /* 本机系统口令：断网也可执行 */
    static const struct {
        int id;
        const char *py;
    } k_local[] = {
        {NL_OC_REBOOT, "chong qi"},
        {NL_OC_GOODBYE, "tui xia"},
        {NL_OC_GOODBYE, "zai jian"},
        {NL_OC_GOODBYE, "bai bai"},
        {NL_OC_GOODBYE, "xi ping"},
        {NL_OC_MEDIA_PREV, "shang yi shou"},
        {NL_OC_MEDIA_NEXT, "xia yi shou"},
        {NL_OC_MEDIA_PAUSE, "zan ting"},
        {NL_OC_MEDIA_RESUME, "ji xu bo fang"},
    };
    for (int i = 0; i < (int)(sizeof(k_local) / sizeof(k_local[0])); i++) {
        esp_err_t ret = esp_mn_commands_add(k_local[i].id, k_local[i].py);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "local voice cmd: id=%d pinyin='%s'", k_local[i].id, k_local[i].py);
        } else {
            ESP_LOGW(TAG, "Fail to add local voice cmd: '%s'", k_local[i].py);
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
    if (s_mn && s_mn_data) {
        s_mn->print_active_speech_commands(s_mn_data);
    }
}

/* 由 application.cc 提供：复用 assets 已 load 的 srmodels，勿再找已删除的 model 分区 */
extern srmodel_list_t *nodelio_get_srmodels(void);

static esp_err_t ensure_multinet(void)
{
    if (s_mn && s_mn_data) {
        return ESP_OK;
    }
    if (!s_models) {
        s_models = nodelio_get_srmodels();
        s_owns_models = false;
        if (!s_models) {
            /* 兼容旧分区表：仅当 assets 未注入时再试 partition "model" */
            s_models = esp_srmodel_init("model");
            s_owns_models = s_models != NULL;
        }
        if (!s_models) {
            ESP_LOGE(TAG, "no srmodels (assets 未加载 / 无 model 分区)");
            return ESP_ERR_NOT_FOUND;
        }
    }
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, NULL);
    }
    if (!mn_name) {
        ESP_LOGW(TAG, "no MultiNet in srmodels — 需 CONFIG_SR_MN_CN_MULTINET6_QUANT 并重烧 assets");
        return ESP_ERR_NOT_FOUND;
    }
    s_mn = esp_mn_handle_from_name(mn_name);
    if (!s_mn) {
        return ESP_FAIL;
    }
    s_mn_data = s_mn->create(mn_name, LISTEN_MS);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "multinet create failed (DRAM?)");
        s_mn = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (s_mn->switch_loader_mode) {
        model_iface_data_t *sw = s_mn->switch_loader_mode(s_mn_data, ESP_MN_LOAD_FROM_FLASH);
        if (sw) {
            s_mn_data = sw;
        }
    }
    (void)esp_mn_commands_alloc(s_mn, s_mn_data);
    install_commands();
    ESP_LOGI(TAG, "MultiNet ready %s", mn_name);
    return ESP_OK;
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
            if (err == ESP_OK) {
                finish_listen(zh ? zh : "已执行", true);
            } else {
                finish_listen("执行失败", false);
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
                    finish_listen(t, true);
                } else if (err == ESP_ERR_INVALID_STATE || http == 503) {
                    finish_listen("设备离线", false);
                } else {
                    finish_listen("执行失败", false);
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
            finish_listen(t, true);
        } else if (err == ESP_ERR_INVALID_STATE) {
            finish_listen("设备离线", false);
        } else {
            finish_listen("执行失败", false);
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

esp_err_t nodelio_offline_cmd_begin_listen(void)
{
    if (s_session) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 未注册/未初始化：明确提示，勿进入空口令窗（读 RAM 镜像，避免误在非内部栈调 flash） */
    nodelio_nvs_config_t cfg;
    nodelio_nvs_cache_get(&cfg);
    if (cfg.server_url[0] == '\0' || cfg.owner[0] == '\0') {
        nodelio_panel_ui_set_voice_hint("未绑定");
        nodelio_panel_ui_toast("未绑定：设置里填服务器与绑定码");
        ESP_LOGW(TAG, "begin_listen blocked: not provisioned");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg.device_id[0] == '\0' || cfg.device_key[0] == '\0') {
        nodelio_panel_ui_set_voice_hint("未注册");
        nodelio_panel_ui_toast("尚未注册成功，请联网后重试");
        ESP_LOGW(TAG, "begin_listen blocked: not registered");
        return ESP_ERR_INVALID_STATE;
    }
    /* 无布局仍可听「重启/退下」；控家口令需布局 */
    const bool have_layout = nodelio_panel_ui_get_layout() != NULL;

    esp_err_t err = ensure_multinet();
    if (err != ESP_OK) {
        nodelio_panel_ui_set_voice_hint("口令未就绪");
        nodelio_panel_ui_toast("本地口令模型未就绪（需 MN6 assets）");
        ESP_LOGW(TAG, "begin_listen blocked: multinet err=%s", esp_err_to_name(err));
        return err;
    }
    err = ensure_cmd_worker();
    if (err != ESP_OK) {
        nodelio_panel_ui_toast("本地口令启动失败");
        return err;
    }
    if (s_mn && s_mn_data) {
        s_mn->clean(s_mn_data);
    }
    s_mn_div = 0;
    s_deadline_us = esp_timer_get_time() + (int64_t)LISTEN_MS * 1000;
    s_session = true;
    s_listening = true;
    nodelio_panel_ui_note_activity();
    if (s_dyn_cmd_count > 0) {
        nodelio_panel_ui_set_voice_hint("请说设备名或重启/退下");
        nodelio_panel_ui_toast("本地口令");
    } else if (have_layout) {
        nodelio_panel_ui_set_voice_hint("可说重启或退下");
        nodelio_panel_ui_toast("本地口令（重启/退下）");
    } else {
        nodelio_panel_ui_set_voice_hint("可说重启或退下");
        nodelio_panel_ui_toast("无布局：仅重启/退下");
    }
    nodelio_panel_ui_play_cue(0); /* 与小智进听同款 POPUP */
    ESP_LOGI(TAG, "local command window %dms dyn=%d layout=%d", LISTEN_MS, s_dyn_cmd_count,
             have_layout ? 1 : 0);
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
    if (!s_listening || !s_mn || !s_mn_data || !data || samples <= 0) {
        return;
    }
    if (esp_timer_get_time() > s_deadline_us) {
        ESP_LOGI(TAG, "mn timeout");
        finish_listen("没听清，再说唤醒词", false);
        return;
    }
    /* 隔帧跑 MultiNet，减轻 FEED/CPU 压力 */
    if ((++s_mn_div & 1) != 0) {
        return;
    }
    int chunk = s_mn->get_samp_chunksize(s_mn_data);
    if (chunk <= 0 || samples < chunk) {
        return;
    }
    /* 取末尾对齐 chunk（AFE fetch 通常正好一块） */
    const int16_t *p = data + (samples - chunk);
    esp_mn_state_t st = s_mn->detect(s_mn_data, (int16_t *)p);
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
    esp_mn_results_t *r = s_mn->get_results(s_mn_data);
    s_listening = false; /* 先停 feed，避免重入 */
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
    s_session = false;
    s_listening = false;
    if (s_mn && s_mn_data) {
        s_mn->destroy(s_mn_data);
        s_mn_data = NULL;
        s_mn = NULL;
    }
    if (s_owns_models && s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        s_owns_models = false;
    }
    ESP_LOGI(TAG, "offline cmd stopped, internal free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

#endif /* CONFIG_NODELIO_PANEL */
