#include "overlay_priv.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "media_player.h"
#include "mqtt_panel.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "offline_cmd.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_board.h"
#include "wifi_manager.h"
#include "wifi_provision.h" /* nodelio_log_heap */
#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#include "audio_codec.h"
#include "backlight.h"

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_pui"

extern "C" esp_err_t nodelio_panel_ui_start(const nodelio_nvs_config_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    }
    return nodelio_overlay_init();
}

static esp_err_t alloc_layout_buf(void)
{
    if (s_layout) {
        return ESP_OK;
    }
    s_layout = static_cast<nodelio_panel_layout_t *>(
        heap_caps_malloc(sizeof(*s_layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_layout) {
        ESP_LOGW(TAG, "layout alloc PSRAM fail size=%u, try DRAM", (unsigned)sizeof(*s_layout));
        s_layout = static_cast<nodelio_panel_layout_t *>(malloc(sizeof(*s_layout)));
    }
    if (!s_layout) {
        ESP_LOGE(TAG, "layout apply NO_MEM size=%u", (unsigned)sizeof(*s_layout));
        nodelio_log_heap("apply_layout fail");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" esp_err_t nodelio_panel_ui_reserve_layout(void)
{
    return alloc_layout_buf();
}

extern "C" esp_err_t nodelio_panel_ui_apply_layout(const nodelio_panel_layout_t *layout)
{
    if (!layout) {
        return ESP_ERR_INVALID_ARG;
    }
    nodelio_log_heap("apply_layout");
    if (alloc_layout_buf() != ESP_OK) {
        ESP_LOGE(TAG, "layout apply NO_MEM size=%u pages=%d", (unsigned)sizeof(*s_layout),
                 layout->page_count);
        return ESP_ERR_NO_MEM;
    }
    if (layout != s_layout) {
        *s_layout = *layout;
    }
    s_have_layout = true;
    s_cloud_ok = true; /* 拉到 layout 说明 NodeLio 服务可达 */
    nodelio_offline_cmd_rebuild_from_layout(s_layout);
    if (s_ready && s_home) {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_fill_home();
            char msg[20];
            std::snprintf(msg, sizeof(msg), "v%d", layout->layout_version);
            nl_ov_refresh_status_icons(s_wifi_ok, true, msg);
        } else {
            ESP_LOGW(TAG, "layout cached but display lock fail — home may stay 初始化中");
        }
    } else {
        ESP_LOGW(TAG, "layout cached but UI not ready (ready=%d home=%p)", s_ready ? 1 : 0,
                 (void *)s_home);
    }
    ESP_LOGI(TAG, "layout v%d cached pages=%d", layout->layout_version, layout->page_count);
    return ESP_OK;
}

extern "C" void nodelio_panel_ui_set_status(bool wifi_ok, bool cloud_ok, const char *msg)
{
    if (!s_ready) {
        s_wifi_ok = wifi_ok;
        s_cloud_ok = cloud_ok;
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_refresh_status_icons(wifi_ok, cloud_ok, msg);
}

extern "C" void nodelio_panel_ui_note_activity(void)
{
    Board::GetInstance().NoteUserActivity();
}

extern "C" bool nodelio_panel_ui_screen_on(void)
{
    return s_screen_on;
}

extern "C" void nodelio_panel_ui_set_screen_cb(nodelio_panel_ui_screen_cb_t cb)
{
    s_screen_cb = cb;
}

extern "C" void nodelio_panel_ui_set_net_cb(nodelio_panel_ui_net_cb_t cb)
{
    s_net_cb = cb;
}

extern "C" void nodelio_panel_ui_notify_screen(bool on)
{
    if (on == s_screen_on) {
        return;
    }
    s_screen_on = on;
    if (s_activity_tmr) {
        if (on) {
            lv_timer_resume(s_activity_tmr);
        } else {
            lv_timer_pause(s_activity_tmr);
        }
    }
    nl_ov_status_poll_set_period(on);
    if (s_screen_cb) {
        s_screen_cb(on);
    }
}

extern "C" void nodelio_panel_ui_notify_net(bool on)
{
    if (s_net_cb) {
        s_net_cb(on);
    }
}

extern "C" void nodelio_panel_ui_toast(const char *text) { nl_ov_toast(text); }
extern "C" void nodelio_panel_ui_toast_defer(const char *text) { nl_ov_toast(text); }

extern "C" void nodelio_panel_ui_announce_spoken(const char *text)
{
    nodelio_panel_ui_announce_spoken_ex(text, nullptr);
}

extern "C" void nodelio_panel_ui_announce_spoken_ex(const char *text, const char *audio_url)
{
    if (!text || !text[0]) {
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        return;
    }
    char *audio = (audio_url && audio_url[0]) ? strdup(audio_url) : nullptr;
    Application::GetInstance().Schedule([copy, audio]() {
        auto &app = Application::GetInstance();
        auto *disp = Board::GetInstance().GetDisplay();
        if (disp) {
            disp->SetChatMessage("assistant", copy);
        }
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nodelio_overlay_on_chat_locked("助手", copy);
                if (nl_ov_ai_session_active()) {
                    nodelio_overlay_show_chat();
                }
            }
        }
        char tip[64];
        snprintf(tip, sizeof(tip), "%.56s", copy);
        nl_ov_toast(tip);
        ESP_LOGI(TAG, "announce_spoken audio=%s", audio ? audio : "(none)");
        app.PlaySpokenAfterLocalSkill(audio);
        free(copy);
        free(audio);
    });
}

extern "C" void nodelio_panel_ui_skills_changed(void)
{
    Application::GetInstance().Schedule([]() {
        nl_ov_home_skills_invalidate();
        nl_ov_home_skills_ensure_fetched();
        nl_ov_apps_refresh();
    });
}

extern "C" void nodelio_panel_ui_skill_hint(const char *app_or_instance_id)
{
    /* 复用 open_app：按名/id 找到已装技能并 invoke 一次（事件驱动，无轮询） */
    (void)nodelio_panel_ui_open_app(app_or_instance_id);
}

extern "C" void nodelio_panel_ui_play_cue(int cue)
{
    Application::GetInstance().Schedule([cue]() {
        auto& app = Application::GetInstance();
        switch (cue) {
        case 0:
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            break;
        case 1:
            app.PlaySound(Lang::Sounds::OGG_SUCCESS);
            break;
        default:
            app.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
            break;
        }
    });
}
extern "C" void nodelio_panel_ui_set_voice_hint(const char *hint)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        nodelio_overlay_on_status_locked(hint ? hint : "待命");
    }
}
extern "C" void nodelio_panel_ui_append_chat(const char *role, const char *text)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        nodelio_overlay_on_chat_locked(role, text);
    }
}
extern "C" void nodelio_panel_ui_stream_assistant(const char *delta_or_full, bool, bool done)
{
    if (!done || !delta_or_full) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        nodelio_overlay_on_chat_locked("assistant", delta_or_full);
    }
}
extern "C" const nodelio_panel_layout_t *nodelio_panel_ui_get_layout(void)
{
    return s_have_layout ? s_layout : nullptr;
}

extern "C" bool nodelio_panel_ui_device_online(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return true;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return true; /* snapshot 未列 → 不拦 */
    }
    return d->online;
}

extern "C" void nodelio_panel_ui_local_reboot(void)
{
    nl_ov_toast("正在重启…");
    Application::GetInstance().Schedule([]() {
        ESP_LOGW(TAG, "offline cmd: reboot");
        Application::GetInstance().Reboot();
    });
}

extern "C" void nodelio_panel_ui_local_wifi_config(void)
{
    nl_ov_toast("进入配网热点…");
    Application::GetInstance().Schedule([]() {
        ESP_LOGW(TAG, "offline cmd: enter SoftAP provision");
        static_cast<WifiBoard &>(Board::GetInstance()).EnterWifiConfigMode();
    });
}

extern "C" void nodelio_panel_ui_local_goodbye(void)
{
    /* 触发抽屉 farewell 收束（与云端「退下」同路径） */
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nodelio_overlay_on_chat_locked("assistant", "退下");
        }
    }
    nl_ov_toast("好的，退下了");
    Application::GetInstance().Schedule([]() {
        auto &app = Application::GetInstance();
        DeviceState st = app.GetDeviceState();
        if (st == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        if (st != kDeviceStateIdle && st != kDeviceStateStarting &&
            st != kDeviceStateWifiConfiguring && st != kDeviceStateUpgrading) {
            app.SetDeviceState(kDeviceStateIdle);
        }
        if (nodelio_media_resume_after_voice()) {
            Board::GetInstance().NoteUserActivity();
            ESP_LOGI(TAG, "offline cmd: goodbye → resume media");
        } else {
            Board::GetInstance().RequestGoodbyeSleep();
            ESP_LOGI(TAG, "offline cmd: goodbye → sleep");
        }
    });
}

extern "C" esp_err_t nodelio_panel_ui_home_page(int delta)
{
    Board::GetInstance().NoteUserActivity();
    esp_err_t e = nl_ov_home_page_peek(delta);
    const char *toast = nullptr;
    if (e == ESP_ERR_NOT_FOUND) {
        toast = "只有一屏";
    } else if (e == ESP_ERR_INVALID_STATE) {
        toast = delta < 0 ? "已经是第一屏" : "已经是最后一屏";
    } else if (e != ESP_OK) {
        return e;
    } else {
        toast = delta < 0 ? "上一屏" : "下一屏";
    }
    Application::GetInstance().Schedule([delta, e, toast]() {
        if (!nodelio_overlay_visible()) {
            if (nl_ov_ai_session_active()) {
                nodelio_overlay_show_chat();
            } else {
                nodelio_overlay_show(false);
            }
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        if (e == ESP_OK) {
            if (s_active_tab != NL_TAB_HOME) {
                nl_ov_show_page(NL_TAB_HOME);
            }
            nl_ov_home_page_apply(delta);
        }
        if (toast && toast[0]) {
            nl_ov_toast(toast);
        }
    });
    return e;
}

extern "C" esp_err_t nodelio_panel_ui_scroll(int dir)
{
    Board::GetInstance().NoteUserActivity();
    esp_err_t e = nl_ov_voice_scroll_peek(dir);
    const char *toast = nullptr;
    if (e == ESP_ERR_NOT_FOUND) {
        toast = "当前页不能滚动";
    } else if (e == ESP_ERR_INVALID_STATE) {
        toast = dir < 0 ? "已经到顶了" : "已经到底了";
    } else if (e != ESP_OK) {
        return e;
    } else {
        toast = dir < 0 ? "往上翻" : "往下翻";
    }
    Application::GetInstance().Schedule([dir, e, toast]() {
        if (!nodelio_overlay_visible()) {
            if (nl_ov_ai_session_active()) {
                nodelio_overlay_show_chat();
            } else {
                nodelio_overlay_show(false);
            }
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        if (e == ESP_OK) {
            nl_ov_voice_scroll_apply(dir);
        }
        if (toast && toast[0]) {
            nl_ov_toast(toast);
        }
    });
    return e;
}

extern "C" void nodelio_panel_ui_goto(int page)
{
    Board::GetInstance().NoteUserActivity();
    Application::GetInstance().Schedule([page]() {
        /* 已显示则只切页；勿 overlay_show(false)（会清 s_want_chat、像退下） */
        if (!nodelio_overlay_visible()) {
            if (nl_ov_ai_session_active()) {
                nodelio_overlay_show_chat();
            } else {
                nodelio_overlay_show(false);
            }
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        switch (page) {
        case NL_UI_HOME:
            nl_ov_show_page(NL_TAB_HOME);
            nl_ov_toast("首页");
            break;
        case NL_UI_APPS:
            nl_ov_show_page(NL_TAB_APPS);
            nl_ov_toast("应用");
            break;
        case NL_UI_SETTINGS:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_toast("设置");
            break;
        case NL_UI_CONNECT:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_connect();
            nl_ov_toast("网络与连接");
            break;
        case NL_UI_DISP:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_DISP);
            nl_ov_toast("显示与声音");
            break;
        case NL_UI_POWER:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_POWER);
            nl_ov_toast("省电");
            break;
        case NL_UI_VOICE:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_VOICE);
            nl_ov_toast("语音助手");
            break;
        case NL_UI_MEDIA:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_MEDIA);
            nl_ov_toast("播放器");
            break;
        case NL_UI_CTRL:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_CTRL);
            nl_ov_toast("控家");
            break;
        case NL_UI_SYS:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_SYS);
            nl_ov_toast("设备信息");
            break;
        case NL_UI_RESET:
            nl_ov_show_page(NL_TAB_SET);
            nl_ov_open_settings_detail(SET_PAGE_RESET);
            nl_ov_toast("请在屏幕确认恢复出厂");
            break;
        default:
            ESP_LOGW(TAG, "goto unknown page=%d", page);
            break;
        }
        /* 设置子页在 layer_top，须把对话胶囊再置顶，否则像「退下」 */
        nl_ov_restore_ai_drawer_if_session();
    });
}

/** 去掉句末标点，避免 MCP/STT「音乐。」匹配失败。 */
static void strip_open_app_query(char *s)
{
    if (!s) {
        return;
    }
    for (;;) {
        size_t n = strlen(s);
        if (n == 0) {
            break;
        }
        unsigned char c = (unsigned char)s[n - 1];
        if (c == ' ' || c == '\t' || c == '.' || c == ',' || c == '?' || c == '!' || c == ';' ||
            c == ':' || c == '~' || c == '"' || c == '\'') {
            s[n - 1] = '\0';
            continue;
        }
        if (n >= 3) {
            unsigned char a = (unsigned char)s[n - 3];
            unsigned char b = (unsigned char)s[n - 2];
            unsigned char d = (unsigned char)s[n - 1];
            if (a == 0xE3 && b == 0x80 && (d == 0x82 || d == 0x81)) {
                s[n - 3] = '\0';
                continue;
            }
            if (a == 0xEF && b == 0xBC &&
                (d == 0x9F || d == 0x81 || d == 0x8C || d == 0x9B || d == 0x9A)) {
                s[n - 3] = '\0';
                continue;
            }
            if (a == 0xE2 && b == 0x80 && d == 0xA6) {
                s[n - 3] = '\0';
                continue;
            }
        }
        break;
    }
}

extern "C" bool nodelio_panel_ui_open_app(const char *name_or_id)
{
    if (!name_or_id || !name_or_id[0]) {
        ESP_LOGW(TAG, "open_app: empty name");
        return false;
    }
    ESP_LOGI(TAG, "open_app request='%s'", name_or_id);
    Board::GetInstance().NoteUserActivity();
    char *q = strdup(name_or_id);
    if (!q) {
        return false;
    }
    strip_open_app_query(q);
    if (!q[0]) {
        ESP_LOGW(TAG, "open_app: empty after strip from '%s'", name_or_id);
        free(q);
        return false;
    }
    ESP_LOGI(TAG, "open_app stripped='%s'", q);
    Application::GetInstance().Schedule([q]() {
        if (!nodelio_overlay_visible()) {
            if (nl_ov_ai_session_active()) {
                nodelio_overlay_show_chat();
            } else {
                nodelio_overlay_show(false);
            }
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            const bool ok = nl_ov_apps_open_by_query(q);
            ESP_LOGI(TAG, "open_app run q='%s' ok=%d", q, ok ? 1 : 0);
            if (!ok) {
                nl_ov_toast("未找到应用");
            }
            nl_ov_restore_ai_drawer_if_session();
        }
        free(q);
    });
    return true;
}

static char s_dsh_arm_ws[200];
static char s_dsh_arm_sess[80];

extern "C" void nodelio_panel_ui_arm_dsh(const char *workspace, const char *session)
{
    s_dsh_arm_ws[0] = '\0';
    s_dsh_arm_sess[0] = '\0';
    if (workspace && workspace[0]) {
        snprintf(s_dsh_arm_ws, sizeof(s_dsh_arm_ws), "%s", workspace);
    }
    if (session && session[0]) {
        snprintf(s_dsh_arm_sess, sizeof(s_dsh_arm_sess), "%s", session);
    }
    ESP_LOGI(TAG, "arm dsh ws='%s' sess='%s'", s_dsh_arm_ws, s_dsh_arm_sess);
}

extern "C" void nodelio_panel_ui_clear_dsh_arm(void)
{
    s_dsh_arm_ws[0] = '\0';
    s_dsh_arm_sess[0] = '\0';
}

extern "C" bool nodelio_panel_ui_dsh_is_armed(void)
{
    return s_dsh_arm_ws[0] != '\0' || s_dsh_arm_sess[0] != '\0';
}

extern "C" void nodelio_panel_ui_get_dsh_arm(char *workspace, size_t ws_len, char *session,
                                            size_t sess_len)
{
    if (workspace && ws_len) {
        snprintf(workspace, ws_len, "%s", s_dsh_arm_ws);
    }
    if (session && sess_len) {
        snprintf(session, sess_len, "%s", s_dsh_arm_sess);
    }
}

extern "C" bool nodelio_panel_ui_dsh_task(const char *instruction)
{
    Board::GetInstance().NoteUserActivity();
    char *copy = nullptr;
    if (instruction && instruction[0]) {
        copy = strdup(instruction);
        if (!copy) {
            return false;
        }
    }
    Application::GetInstance().Schedule([copy]() {
        if (!nodelio_overlay_visible()) {
            nodelio_overlay_show(false);
        }
        (void)nl_ov_apps_invoke_dsh_task(copy);
        free(copy);
    });
    return true;
}

extern "C" void nodelio_panel_ui_snapshot_begin(void)
{
    s_dev_n = 0;
}

extern "C" void nodelio_panel_ui_snapshot_add(const char *device_id, const char *name, bool online,
                                              const char *power_state)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs || !device_id || !device_id[0] || s_dev_n >= NL_DEV_CACHE_MAX) {
        return;
    }
    nl_dev_cache_t *d = &s_devs[s_dev_n++];
    std::memset(d, 0, sizeof(*d));
    std::snprintf(d->id, sizeof(d->id), "%s", device_id);
    if (name && name[0]) {
        std::snprintf(d->name, sizeof(d->name), "%s", name);
    }
    d->online = online;
    if (power_state && power_state[0]) {
        std::snprintf(d->power, sizeof(d->power), "%s", power_state);
    }
}

extern "C" void nodelio_panel_ui_snapshot_set_lan(const char *device_id, const char *ip, bool lan_control,
                                                  bool has_pass)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->lan_control = lan_control;
    d->pass_required = has_pass;
    if (ip && ip[0]) {
        std::snprintf(d->ip, sizeof(d->ip), "%s", ip);
    } else {
        d->ip[0] = '\0';
    }
}

extern "C" void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index,
                                                      const char *ch_name, const char *state)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d || !state || channel_index < 0 || d->ch_n >= NL_CH_PER_DEV) {
        return;
    }
    /* 同 index 已存在则覆盖（LAN 探测刷新） */
    for (uint8_t i = 0; i < d->ch_n; i++) {
        if (d->ch_idx[i] == (int16_t)channel_index) {
            if (ch_name && ch_name[0]) {
                std::snprintf(d->ch_name[i], sizeof(d->ch_name[0]), "%s", ch_name);
            }
            std::snprintf(d->ch_st[i], sizeof(d->ch_st[0]), "%s", state);
            return;
        }
    }
    d->ch_idx[d->ch_n] = (int16_t)channel_index;
    if (ch_name && ch_name[0]) {
        std::snprintf(d->ch_name[d->ch_n], sizeof(d->ch_name[0]), "%s", ch_name);
    }
    std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", state);
    d->ch_cd[d->ch_n] = 0;
    d->ch_n++;
}

extern "C" void nodelio_panel_ui_set_lan_online(const char *device_id, bool online, const char *power_state)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->online = online;
    if (power_state && power_state[0]) {
        std::snprintf(d->power, sizeof(d->power), "%s", power_state);
    }
}

extern "C" void nodelio_panel_ui_set_lan_pass_required(const char *device_id, bool need)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->pass_required = need;
}

extern "C" void nodelio_panel_ui_clear_lan_channels(const char *device_id)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->ch_n = 0;
    std::memset(d->ch_idx, 0, sizeof(d->ch_idx));
    std::memset(d->ch_name, 0, sizeof(d->ch_name));
    std::memset(d->ch_st, 0, sizeof(d->ch_st));
    std::memset(d->ch_cd, 0, sizeof(d->ch_cd));
}

extern "C" void nodelio_panel_ui_refresh_tiles(void)
{
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_refresh_home_tiles();
}

extern "C" void nodelio_panel_ui_note_countdown(const char *device_id, int channel_index, int seconds)
{
    nl_ov_set_countdown(device_id, channel_index, seconds);
}

extern "C" void nodelio_panel_ui_set_countdown(const char *device_id, int channel_index, int seconds)
{
    nl_ov_set_countdown(device_id, channel_index, seconds);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_refresh_home_tiles();
}

extern "C" void nodelio_panel_ui_snapshot_end(void)
{
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_refresh_home_tiles();
}

extern "C" void nodelio_panel_ui_apply_bind_state(const nl_bind_t *bind, const char *value)
{
    if (!bind || !value) {
        return;
    }
    nl_ov_apply_bind_state(bind, value);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_refresh_home_tiles();
}

extern "C" void nodelio_panel_ui_apply_reachability(const char *const *device_ids, const bool *online,
                                                    int n)
{
    nodelio_panel_ui_snapshot_begin();
    if (device_ids && online && n > 0) {
        for (int i = 0; i < n; i++) {
            nodelio_panel_ui_snapshot_add(device_ids[i], "", online[i], "");
        }
    }
    nodelio_panel_ui_snapshot_end();
}

extern "C" int nodelio_panel_ui_get_dev_count(void)
{
    return s_dev_n;
}

extern "C" const nl_dev_cache_t *nodelio_panel_ui_get_dev(int index)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs || index < 0 || index >= s_dev_n) {
        return nullptr;
    }
    return &s_devs[index];
}
