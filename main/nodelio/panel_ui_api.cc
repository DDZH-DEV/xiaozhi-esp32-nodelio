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
#include "wifi_provision.h"
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

extern "C" esp_err_t nodelio_panel_ui_apply_layout(const nodelio_panel_layout_t *layout)
{
    if (!layout) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_layout) {
        s_layout = static_cast<nodelio_panel_layout_t *>(
            heap_caps_malloc(sizeof(*s_layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_layout) {
            s_layout = static_cast<nodelio_panel_layout_t *>(malloc(sizeof(*s_layout)));
        }
    }
    if (!s_layout) {
        return ESP_ERR_NO_MEM;
    }
    *s_layout = *layout;
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
        }
    }
    ESP_LOGI(TAG, "layout v%d cached", layout->layout_version);
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

extern "C" void nodelio_panel_ui_skills_changed(void)
{
    Application::GetInstance().Schedule([]() {
        nl_ov_home_skills_invalidate();
        nl_ov_home_skills_ensure_fetched();
        nl_ov_apps_refresh();
    });
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

extern "C" bool nodelio_panel_ui_open_app(const char *name_or_id)
{
    if (!name_or_id || !name_or_id[0]) {
        return false;
    }
    Board::GetInstance().NoteUserActivity();
    char *q = strdup(name_or_id);
    if (!q) {
        return false;
    }
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
            if (!nl_ov_apps_open_by_query(q)) {
                nl_ov_toast("未找到应用");
            }
            nl_ov_restore_ai_drawer_if_session();
        }
        free(q);
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

extern "C" void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index,
                                                      const char *ch_name, const char *state)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d || !state || channel_index < 0 || d->ch_n >= NL_CH_PER_DEV) {
        return;
    }
    d->ch_idx[d->ch_n] = (int16_t)channel_index;
    if (ch_name && ch_name[0]) {
        std::snprintf(d->ch_name[d->ch_n], sizeof(d->ch_name[0]), "%s", ch_name);
    }
    std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", state);
    d->ch_cd[d->ch_n] = 0;
    d->ch_n++;
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
