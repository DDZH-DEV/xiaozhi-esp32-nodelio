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
#include "offline_cmd_map.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
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
#include <nvs.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

static int s_home_filled_version = -1;

extern "C" void nodelio_panel_ui_mark_home_filled(int layout_version)
{
    s_home_filled_version = layout_version;
}

extern "C" int nodelio_panel_ui_home_filled_version(void)
{
    return s_home_filled_version;
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
        /* 同版本已画过：禁止再 lv_obj_clean（开机二次灌缓存会闪几下） */
        if (s_home_filled_version == layout->layout_version && layout->layout_version > 0) {
            ESP_LOGI(TAG, "layout v%d already on home — skip rebuild", layout->layout_version);
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                char msg[20];
                std::snprintf(msg, sizeof(msg), "v%d", layout->layout_version);
                nl_ov_refresh_status_icons(s_wifi_ok, true, msg);
            }
            return ESP_OK;
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_fill_home();
            s_home_filled_version = layout->layout_version;
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

extern "C" void nodelio_panel_ui_request_cloud_speak(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    char *copy = strdup(text);
    if (!copy) {
        return;
    }
    Application::GetInstance().Schedule([copy]() {
        Application::GetInstance().QueueCloudSpeakPrompt(copy);
        free(copy);
    });
}

extern "C" void nodelio_panel_ui_news_read_local(const char *op)
{
    const char *use = (op && op[0]) ? op : "read";
    char *copy = strdup(use);
    if (!copy) {
        return;
    }
    Application::GetInstance().Schedule([copy]() {
        Application::GetInstance().StartLocalNewsRead(copy);
        free(copy);
    });
}

static bool s_news_read_offer_armed = false;

extern "C" void nodelio_panel_ui_arm_news_read_offer(void)
{
    s_news_read_offer_armed = true;
    ESP_LOGI(TAG, "arm news read offer");
}

extern "C" bool nodelio_panel_ui_take_news_read_offer(void)
{
    if (!s_news_read_offer_armed) {
        return false;
    }
    s_news_read_offer_armed = false;
    return true;
}

extern "C" void nodelio_panel_ui_announce_spoken_ex(const char *text, const char *audio_url)
{
    if (audio_url && audio_url[0]) {
        char one[320];
        snprintf(one, sizeof(one), "%s", audio_url);
        const char (*rows)[320] = reinterpret_cast<const char (*)[320]>(&one);
        nodelio_panel_ui_announce_spoken_playlist(text, rows, 1);
        return;
    }
    nodelio_panel_ui_announce_spoken_playlist(text, nullptr, 0);
}

extern "C" void nodelio_panel_ui_announce_spoken_playlist(const char *text, const char (*urls)[320],
                                                          int n)
{
    if ((!text || !text[0]) && n <= 0) {
        return;
    }
    char *copy = (text && text[0]) ? strdup(text) : strdup("");
    if (!copy) {
        return;
    }
    char (*url_copy)[320] = nullptr;
    int url_n = 0;
    if (urls && n > 0) {
        url_n = n > 6 ? 6 : n;
        url_copy = static_cast<char (*)[320]>(calloc((size_t)url_n, sizeof(*url_copy)));
        if (url_copy) {
            for (int i = 0; i < url_n; i++) {
                snprintf(url_copy[i], 320, "%s", urls[i]);
            }
        } else {
            url_n = 0;
        }
    }
    Application::GetInstance().Schedule([copy, url_copy, url_n]() {
        auto &app = Application::GetInstance();
        auto *disp = Board::GetInstance().GetDisplay();
        if (disp && copy[0]) {
            disp->SetChatMessage("assistant", copy);
        }
        if (copy[0]) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nodelio_overlay_on_chat_locked("助手", copy);
                if (nl_ov_ai_session_active() && !s_skill_ui_active) {
                    nodelio_overlay_show_chat();
                }
            }
            char tip[64];
            snprintf(tip, sizeof(tip), "%.56s", copy);
            nl_ov_toast(tip);
        }
        if (url_copy && url_n > 0) {
            const char *ptrs[6] = {};
            for (int i = 0; i < url_n && i < 6; i++) {
                ptrs[i] = url_copy[i];
            }
            ESP_LOGI(TAG, "announce_spoken → bridge wav playlist n=%d", url_n);
            app.PlaySpokenPlaylistAfterLocalSkill(copy, ptrs, url_n);
        } else {
            ESP_LOGI(TAG, "announce_spoken → toast only");
            app.PlaySpokenAfterLocalSkill(copy, nullptr);
        }
        free(copy);
        free(url_copy);
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

extern "C" void nodelio_panel_ui_apply_skills_http_body(const char *json)
{
    if (!json || !json[0]) {
        return;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "apply skills body parse fail");
        return;
    }
    nl_ov_apps_ingest_skills_root(root);
    nl_ov_home_skills_ingest_skills_root(root);
    cJSON_Delete(root);
}

/* Notice 短队列：息屏 info 延迟；action 置顶同时最多积压若干条 */
static constexpr int kNoticeDeferMax = 5;
static nodelio_panel_notice_t s_notice_defer[kNoticeDeferMax];
static int s_notice_defer_n = 0;
static char s_notice_active_id[40] = {0};

static void notice_copy(nodelio_panel_notice_t *dst, const nodelio_panel_notice_t *src)
{
    if (!dst || !src) {
        return;
    }
    *dst = *src;
}

static bool notice_sev_at_least_warning(const char *sev)
{
    return sev && (strcmp(sev, "warning") == 0 || strcmp(sev, "critical") == 0);
}

static bool notice_wants_wake(const nodelio_panel_notice_t *n)
{
    if (!n) {
        return false;
    }
    if (n->wake) {
        return true;
    }
    if (strcmp(n->mode, "action") == 0) {
        return true;
    }
    if (strcmp(n->mode, "alert") == 0 && notice_sev_at_least_warning(n->severity)) {
        return true;
    }
    return false;
}

static void notice_maybe_wake(const nodelio_panel_notice_t *n)
{
    if (!notice_wants_wake(n)) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
}

static void notice_do_invoke(const nodelio_panel_notice_t *n)
{
    if (!n) {
        return;
    }
    const char *sid = n->invoke_app[0] ? n->invoke_app : n->app_id;
    if (!sid || !sid[0]) {
        return;
    }
    /* DSH 兼容：有 session 时走专用 status 详情 */
    if (strstr(sid, "dsh.status") != nullptr ||
        (strstr(sid, "dsh") != nullptr && n->session[0])) {
        nl_ov_apps_invoke_dsh_status(nullptr, n->session[0] ? n->session : nullptr);
        return;
    }
    const char *body = n->invoke_params[0] ? n->invoke_params : "{}";
    nl_ov_invoke_skill_async(sid, body);
}

static void notice_present(const nodelio_panel_notice_t *n)
{
    if (!n) {
        return;
    }
    if (n->notice_id[0]) {
        snprintf(s_notice_active_id, sizeof(s_notice_active_id), "%s", n->notice_id);
    }
    const char *toast = n->title[0] ? n->title : nullptr;
    if (!toast) {
        if (strcmp(n->reason, "needs_you") == 0) {
            toast = "有任务等你确认";
        } else if (strcmp(n->reason, "completed") == 0) {
            toast = "有任务已完成";
        } else if (strcmp(n->mode, "alert") == 0) {
            toast = "监测告警";
        } else if (strcmp(n->mode, "action") == 0) {
            toast = "需要你确认";
        } else {
            toast = "新通知";
        }
    }
    nl_ov_toast(toast);

    /* 有 invoke_app（或 action/alert 带 app_id）才拉 Envelope；纯 title info 零 HTTP */
    const bool has_invoke_target = n->invoke_app[0] || n->app_id[0];
    if (strcmp(n->mode, "action") == 0 && has_invoke_target) {
        notice_do_invoke(n);
    } else if (strcmp(n->mode, "alert") == 0 && has_invoke_target &&
               (strcmp(n->severity, "critical") == 0 || n->invoke_app[0])) {
        notice_do_invoke(n);
    } else if (strcmp(n->mode, "info") == 0 && n->invoke_app[0]) {
        notice_do_invoke(n);
    }
}

static void notice_enqueue_deferred(const nodelio_panel_notice_t *n)
{
    if (!n) {
        return;
    }
    if (s_notice_defer_n >= kNoticeDeferMax) {
        /* 丢最旧 */
        memmove(&s_notice_defer[0], &s_notice_defer[1],
                sizeof(s_notice_defer[0]) * (kNoticeDeferMax - 1));
        s_notice_defer_n = kNoticeDeferMax - 1;
    }
    notice_copy(&s_notice_defer[s_notice_defer_n++], n);
}

extern "C" void nodelio_panel_ui_notice_flush_deferred(void)
{
    if (s_notice_defer_n <= 0) {
        return;
    }
    const int n = s_notice_defer_n;
    auto *local = static_cast<nodelio_panel_notice_t *>(
        malloc(sizeof(nodelio_panel_notice_t) * (size_t)n));
    if (!local) {
        s_notice_defer_n = 0;
        return;
    }
    memcpy(local, s_notice_defer, sizeof(nodelio_panel_notice_t) * (size_t)n);
    s_notice_defer_n = 0;
    Application::GetInstance().Schedule([local, n]() {
        for (int i = 0; i < n; ++i) {
            notice_present(&local[i]);
        }
        free(local);
    });
}

extern "C" void nodelio_panel_ui_notice_clear(const char *notice_id)
{
    if (!notice_id || !notice_id[0]) {
        return;
    }
    char *id = strdup(notice_id);
    Application::GetInstance().Schedule([id]() {
        if (id && s_notice_active_id[0] && strcmp(s_notice_active_id, id) == 0) {
            s_notice_active_id[0] = '\0';
        }
        if (id) {
            for (int i = 0; i < s_notice_defer_n; ++i) {
                if (strcmp(s_notice_defer[i].notice_id, id) == 0) {
                    if (i + 1 < s_notice_defer_n) {
                        memmove(&s_notice_defer[i], &s_notice_defer[i + 1],
                                sizeof(s_notice_defer[0]) * (size_t)(s_notice_defer_n - i - 1));
                    }
                    --s_notice_defer_n;
                    break;
                }
            }
            free(id);
        }
    });
}

extern "C" void nodelio_panel_ui_notice(const nodelio_panel_notice_t *notice)
{
    if (!notice) {
        return;
    }
    auto *copy = static_cast<nodelio_panel_notice_t *>(malloc(sizeof(nodelio_panel_notice_t)));
    if (!copy) {
        return;
    }
    notice_copy(copy, notice);
    if (!copy->mode[0]) {
        if (strcmp(copy->reason, "needs_you") == 0) {
            snprintf(copy->mode, sizeof(copy->mode), "action");
        } else if (strcmp(copy->reason, "completed") == 0) {
            snprintf(copy->mode, sizeof(copy->mode), "info");
        } else {
            snprintf(copy->mode, sizeof(copy->mode), "info");
        }
    }
    if (!copy->severity[0]) {
        snprintf(copy->severity, sizeof(copy->severity), "normal");
    }
    Application::GetInstance().Schedule([copy]() {
        const bool sleeping = Board::GetInstance().IsInSleepMode();
        const bool is_info = strcmp(copy->mode, "info") == 0;
        if (sleeping && is_info && !notice_wants_wake(copy)) {
            notice_enqueue_deferred(copy);
            free(copy);
            return;
        }
        notice_maybe_wake(copy);
        notice_present(copy);
        free(copy);
    });
}

extern "C" void nodelio_panel_ui_skill_hint(const char *app_or_instance_id, const char *reason,
                                            const char *session)
{
    nodelio_panel_notice_t n;
    memset(&n, 0, sizeof(n));
    if (app_or_instance_id && app_or_instance_id[0]) {
        snprintf(n.app_id, sizeof(n.app_id), "%s", app_or_instance_id);
        snprintf(n.invoke_app, sizeof(n.invoke_app), "%s", app_or_instance_id);
    }
    if (reason && reason[0]) {
        snprintf(n.reason, sizeof(n.reason), "%s", reason);
    }
    if (session && session[0]) {
        snprintf(n.session, sizeof(n.session), "%s", session);
        snprintf(n.invoke_params, sizeof(n.invoke_params), "{\"session\":\"%s\"}", session);
    }
    if (reason && strcmp(reason, "needs_you") == 0) {
        snprintf(n.mode, sizeof(n.mode), "action");
        snprintf(n.title, sizeof(n.title), "有任务等你确认");
        n.wake = 1;
    } else if (reason && strcmp(reason, "completed") == 0) {
        snprintf(n.mode, sizeof(n.mode), "info");
        snprintf(n.title, sizeof(n.title), "有任务已完成");
    } else {
        snprintf(n.mode, sizeof(n.mode), "info");
    }
    /* 旧 hint 无 app 时默认 dsh.status */
    if (!n.app_id[0]) {
        snprintf(n.app_id, sizeof(n.app_id), "dsh.status");
        snprintf(n.invoke_app, sizeof(n.invoke_app), "dsh.status");
    }
    nodelio_panel_ui_notice(&n);
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

/** 已收到过至少一次 snapshot；之前未登记设备按离线画，避免亮→暗集体闪。 */
static bool s_have_device_snapshot = false;

extern "C" bool nodelio_panel_ui_device_online(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return true;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        /* snapshot 前未登记 → 离线（暗）；snapshot 后未列 → 不拦（兼容旧逻辑） */
        return s_have_device_snapshot;
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
        /* 媒体须退会话才能 HTTP 播；新闻 keep_listening 追问要不要读；天气问内容 keep_cloud */
        if (!nodelio_offline_query_keeps_listening(q)) {
            auto st = Application::GetInstance().GetDeviceState();
            if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                st == kDeviceStateConnecting) {
                Application::GetInstance().EndVoiceSessionAfterLocalSkill();
            }
        }
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
static char s_dsh_pending_instr[200];
static esp_timer_handle_t s_dsh_arm_timer;

static void dsh_pending_clear(void)
{
    s_dsh_pending_instr[0] = '\0';
    nodelio_skill_dsh_on_confirm_pending(nullptr);
}

static void dsh_arm_timer_stop(void)
{
    if (s_dsh_arm_timer) {
        esp_timer_stop(s_dsh_arm_timer);
    }
}

/** 剥标点后的短肯定/否定（与打开 DSH 确认同口径）。 */
static void dsh_norm_stt(const char *text, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!text) {
        return;
    }
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < buf_len; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || c == '?' || c == '!' || c == ',' || c == ' ' || c == '\t') {
            continue;
        }
        if ((c == 0xE3 && (unsigned char)p[1] == 0x80 &&
             ((unsigned char)p[2] == 0x82 || (unsigned char)p[2] == 0x81)) ||
            (c == 0xEF && (unsigned char)p[1] == 0xBC &&
             ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81 ||
              (unsigned char)p[2] == 0x8C))) {
            p += 2;
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = '\0';
}

static bool dsh_stt_is_yes(const char *norm)
{
    if (!norm || !norm[0]) {
        return false;
    }
    return strcmp(norm, "是的") == 0 || strcmp(norm, "是") == 0 || strcmp(norm, "好的") == 0 ||
           strcmp(norm, "好") == 0 || strcmp(norm, "嗯") == 0 || strcmp(norm, "行") == 0 ||
           strcmp(norm, "可以") == 0 || strcmp(norm, "对") == 0 || strcmp(norm, "要") == 0 ||
           strcmp(norm, "发送") == 0 || strcmp(norm, "确认") == 0 || strcmp(norm, "发吧") == 0 ||
           strcmp(norm, "发") == 0 || strstr(norm, "发送吧") != nullptr;
}

static bool dsh_stt_is_no(const char *norm)
{
    if (!norm || !norm[0]) {
        return false;
    }
    return strcmp(norm, "不") == 0 || strcmp(norm, "不用") == 0 || strcmp(norm, "不要") == 0 ||
           strcmp(norm, "算了") == 0 || strcmp(norm, "取消") == 0 || strcmp(norm, "重说") == 0 ||
           strstr(norm, "不用了") != nullptr || strstr(norm, "重新说") != nullptr;
}

static void dsh_arm_timeout_cb(void *arg)
{
    (void)arg;
    Application::GetInstance().Schedule([]() {
        if (!nodelio_panel_ui_dsh_is_armed()) {
            return;
        }
        nodelio_panel_ui_clear_dsh_arm();
        nodelio_skill_dsh_on_arm_cleared();
        nl_ov_toast("续聊已超时取消");
    });
}

static void dsh_arm_timer_restart(void)
{
    if (!s_dsh_arm_timer) {
        const esp_timer_create_args_t args = {
            .callback = &dsh_arm_timeout_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dsh_arm",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_dsh_arm_timer) != ESP_OK) {
            s_dsh_arm_timer = nullptr;
            return;
        }
    }
    esp_timer_stop(s_dsh_arm_timer);
    esp_timer_start_once(s_dsh_arm_timer, 60 * 1000000ULL);
}

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
    dsh_arm_timer_restart();
}

extern "C" void nodelio_panel_ui_clear_dsh_arm(void)
{
    dsh_arm_timer_stop();
    s_dsh_arm_ws[0] = '\0';
    s_dsh_arm_sess[0] = '\0';
    dsh_pending_clear();
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

extern "C" bool nodelio_panel_ui_dsh_has_pending_send(void)
{
    return s_dsh_pending_instr[0] != '\0';
}

extern "C" void nodelio_panel_ui_dsh_get_pending_send(char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", s_dsh_pending_instr);
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

extern "C" bool nodelio_panel_ui_dsh_confirm_send(const char *instruction)
{
    if (!instruction || !instruction[0]) {
        return false;
    }
    if (!nodelio_panel_ui_dsh_is_armed()) {
        /* 无武装则直接派活（「让它…」路径） */
        return nodelio_panel_ui_dsh_task(instruction);
    }
    snprintf(s_dsh_pending_instr, sizeof(s_dsh_pending_instr), "%s", instruction);
    dsh_arm_timer_restart();
    Board::GetInstance().NoteUserActivity();
    /* toast 只预览前缀，完整句在状态条 / pending（勿用 %s 拷满长串，躲 -Wformat-truncation） */
    char toast[128];
    static const char kPrefix[] = "确认发送？";
    const size_t pre_len = sizeof(kPrefix) - 1;
    memcpy(toast, kPrefix, pre_len);
    const size_t room = sizeof(toast) - pre_len - 1;
    const size_t n = strnlen(s_dsh_pending_instr, room);
    memcpy(toast + pre_len, s_dsh_pending_instr, n);
    toast[pre_len + n] = '\0';
    Application::GetInstance().Schedule([toast_copy = std::string(toast),
                                         instr = std::string(s_dsh_pending_instr)]() {
        nl_ov_toast(toast_copy.c_str());
        nodelio_skill_dsh_on_confirm_pending(instr.c_str());
    });
    ESP_LOGI(TAG, "dsh confirm pending '%s'", s_dsh_pending_instr);
    return true;
}

extern "C" void nodelio_panel_ui_dsh_confirm_accept(void)
{
    if (!s_dsh_pending_instr[0]) {
        return;
    }
    char instr[200];
    snprintf(instr, sizeof(instr), "%s", s_dsh_pending_instr);
    s_dsh_pending_instr[0] = '\0';
    ESP_LOGI(TAG, "dsh confirm accept '%s'", instr);
    Board::GetInstance().NoteUserActivity();
    /* 统一丢到主任务：收确认条 + 投递（避免 LVGL 回调里嵌套 DisplayLock） */
    Application::GetInstance().Schedule([instr_copy = std::string(instr)]() {
        nodelio_skill_dsh_on_confirm_pending(nullptr);
        if (!nodelio_overlay_visible()) {
            nodelio_overlay_show(false);
        }
        (void)nl_ov_apps_invoke_dsh_task(instr_copy.c_str());
    });
}

extern "C" void nodelio_panel_ui_dsh_confirm_reject(void)
{
    if (!s_dsh_pending_instr[0]) {
        return;
    }
    ESP_LOGI(TAG, "dsh confirm reject '%s'", s_dsh_pending_instr);
    s_dsh_pending_instr[0] = '\0';
    dsh_arm_timer_restart();
    Board::GetInstance().NoteUserActivity();
    Application::GetInstance().Schedule([]() {
        nl_ov_toast("已取消，请重新说");
        nodelio_skill_dsh_on_confirm_pending(nullptr);
    });
}

extern "C" int nodelio_panel_ui_dsh_try_confirm_send(const char *stt)
{
    if (!s_dsh_pending_instr[0] || !stt || !stt[0]) {
        return 0;
    }
    char norm[64];
    dsh_norm_stt(stt, norm, sizeof(norm));
    if (dsh_stt_is_yes(norm)) {
        nodelio_panel_ui_dsh_confirm_accept();
        return 1;
    }
    if (dsh_stt_is_no(norm)) {
        nodelio_panel_ui_dsh_confirm_reject();
        return 2;
    }
    /* 其它话当作改口：整句替换待确认内容 */
    if (strlen(norm) >= 2) {
        ESP_LOGI(TAG, "dsh confirm replace → '%s'", stt);
        (void)nodelio_panel_ui_dsh_confirm_send(stt);
        return 2;
    }
    return 0;
}

/* 局域网设备：云端 online 恒 false / 通道 stub OFF，跨 snapshot 保留上次探测结果防闪烁 */
static nl_dev_cache_t s_lan_sticky[NL_DEV_CACHE_MAX];
static int s_lan_sticky_n = 0;
/** snapshot_begin 前视觉签名；end 时相同则跳过整页刷磁贴 */
static uint32_t s_snap_vis_before;
static uint32_t s_lan_pack_sig_saved;
static int64_t s_lan_nvs_backoff_until_ms;
#define LAN_NVS_BACKOFF_MS (10 * 60 * 1000)

static const nl_dev_cache_t *lan_sticky_find(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_lan_sticky_n; i++) {
        if (strcmp(s_lan_sticky[i].id, device_id) == 0) {
            return &s_lan_sticky[i];
        }
    }
    return nullptr;
}

static uint32_t mix_u32(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t mix_str(uint32_t h, const char *s)
{
    if (!s) {
        return mix_u32(h, 0);
    }
    while (*s) {
        h = mix_u32(h, (uint8_t)*s++);
    }
    return h;
}

extern "C" uint32_t nodelio_panel_ui_dev_visual_sig(void)
{
    nl_ov_ensure_devs_cache();
    uint32_t h = 2166136261u;
    h = mix_u32(h, (uint32_t)s_dev_n);
    if (!s_devs) {
        return h;
    }
    for (int i = 0; i < s_dev_n; i++) {
        const nl_dev_cache_t *d = &s_devs[i];
        h = mix_str(h, d->id);
        h = mix_u32(h, d->online ? 1u : 0u);
        h = mix_u32(h, d->pass_required ? 1u : 0u);
        h = mix_str(h, d->power);
        h = mix_u32(h, (uint32_t)(uint16_t)d->countdown);
        h = mix_u32(h, d->ch_n);
        for (uint8_t c = 0; c < d->ch_n; c++) {
            h = mix_u32(h, (uint32_t)(uint16_t)d->ch_idx[c]);
            h = mix_str(h, d->ch_st[c]);
            h = mix_u32(h, (uint32_t)(uint16_t)d->ch_cd[c]);
        }
    }
    return h;
}

static uint32_t lan_pack_sig_now(void)
{
    nl_ov_ensure_devs_cache();
    uint32_t h = 2166136261u;
    if (!s_devs) {
        return h;
    }
    for (int i = 0; i < s_dev_n; i++) {
        if (!s_devs[i].lan_control || !s_devs[i].id[0] || !s_devs[i].ip[0]) {
            continue;
        }
        h = mix_str(h, s_devs[i].id);
        h = mix_str(h, s_devs[i].ip);
        h = mix_u32(h, s_devs[i].pass_required ? 1u : 0u);
    }
    return h;
}

extern "C" void nodelio_panel_ui_snapshot_begin(void)
{
    s_snap_vis_before = nodelio_panel_ui_dev_visual_sig();
    s_lan_sticky_n = 0;
    if (s_devs) {
        for (int i = 0; i < s_dev_n && s_lan_sticky_n < NL_DEV_CACHE_MAX; i++) {
            if (!s_devs[i].lan_control || !s_devs[i].id[0]) {
                continue;
            }
            s_lan_sticky[s_lan_sticky_n++] = s_devs[i];
        }
    }
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
    if (!lan_control) {
        return;
    }
    /* 忽略云端 online=false / stub 通道：恢复上次探测态。
     * 首次见默认离线（黑）：勿乐观在线，否则重刷/冷启动死设备会先黄再黑。 */
    const nl_dev_cache_t *prev = lan_sticky_find(device_id);
    if (prev) {
        d->online = prev->online;
        if (prev->power[0]) {
            std::snprintf(d->power, sizeof(d->power), "%s", prev->power);
        }
        d->ch_n = prev->ch_n;
        std::memcpy(d->ch_idx, prev->ch_idx, sizeof(d->ch_idx));
        std::memcpy(d->ch_name, prev->ch_name, sizeof(d->ch_name));
        std::memcpy(d->ch_st, prev->ch_st, sizeof(d->ch_st));
        std::memcpy(d->ch_cd, prev->ch_cd, sizeof(d->ch_cd));
        d->countdown = prev->countdown;
    } else {
        d->online = false;
        d->ch_n = 0;
        d->power[0] = '\0';
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

#define LAN_NVS_NS "nodelio"
#define LAN_NVS_KEY "lan_devs"
#define LAN_PACK_MAX 8

typedef struct {
    char id[NL_LAYOUT_STR];
    char ip[48];
    uint8_t pass_required;
} nl_lan_pack_t;

typedef struct {
    uint8_t n;
    nl_lan_pack_t items[LAN_PACK_MAX];
} nl_lan_blob_t;

typedef struct {
    uint32_t pack_sig;
    nl_lan_blob_t blob;
} nl_lan_save_job_t;

extern "C" void nodelio_run_on_app_task(void (*fn)(void *), void *arg)
{
    if (!fn) {
        return;
    }
    Application::GetInstance().Schedule([fn, arg]() { fn(arg); });
}

static void lan_cache_save_flash(void *arg)
{
    nl_lan_save_job_t *job = static_cast<nl_lan_save_job_t *>(arg);
    if (!job) {
        return;
    }
    nl_lan_blob_t *blob = &job->blob;
    nvs_handle_t h;
    if (nvs_open(LAN_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        heap_caps_free(job);
        return;
    }
    esp_err_t err = nvs_set_blob(h, LAN_NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        (void)nvs_erase_key(h, LAN_NVS_KEY);
        (void)nvs_erase_key(h, "apps_list"); /* 可重建；勿挤掉 server_url */
        err = nvs_set_blob(h, LAN_NVS_KEY, blob, sizeof(*blob));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_lan_nvs_backoff_until_ms = 0;
        s_lan_pack_sig_saved = job->pack_sig;
        ESP_LOGI(TAG, "lan cache saved n=%d", blob->n);
    } else {
        /* 满盘时退避，避免每次 snapshot 都打 WARN + 擦写 flash；成功前不记 sig */
        s_lan_nvs_backoff_until_ms = esp_timer_get_time() / 1000 + LAN_NVS_BACKOFF_MS;
        ESP_LOGW(TAG, "lan cache save: %s (backoff %ds)", esp_err_to_name(err),
                 LAN_NVS_BACKOFF_MS / 1000);
    }
    heap_caps_free(job);
}

static void lan_cache_save(void)
{
    /* layout_sync 在 PSRAM 栈：禁止本任务写 NVS（会 assert cache_disabled） */
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_lan_nvs_backoff_until_ms > 0 && now_ms < s_lan_nvs_backoff_until_ms) {
        return;
    }
    const uint32_t pack_sig = lan_pack_sig_now();
    if (pack_sig == s_lan_pack_sig_saved && s_lan_pack_sig_saved != 0) {
        return; /* IP/口令清单未变，勿反复写 NVS */
    }
    nl_ov_ensure_devs_cache();
    if (!s_devs) {
        return;
    }
    nl_lan_save_job_t *job = static_cast<nl_lan_save_job_t *>(
        heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job) {
        job = static_cast<nl_lan_save_job_t *>(calloc(1, sizeof(*job)));
    }
    if (!job) {
        return;
    }
    job->pack_sig = pack_sig;
    for (int i = 0; i < s_dev_n && job->blob.n < LAN_PACK_MAX; i++) {
        if (!s_devs[i].lan_control || !s_devs[i].id[0] || !s_devs[i].ip[0]) {
            continue;
        }
        nl_lan_pack_t *p = &job->blob.items[job->blob.n++];
        std::snprintf(p->id, sizeof(p->id), "%s", s_devs[i].id);
        std::snprintf(p->ip, sizeof(p->ip), "%s", s_devs[i].ip);
        p->pass_required = s_devs[i].pass_required ? 1 : 0;
    }
    if (job->blob.n == 0) {
        heap_caps_free(job);
        s_lan_pack_sig_saved = pack_sig;
        return;
    }
    nodelio_run_on_app_task(lan_cache_save_flash, job);
}

extern "C" void nodelio_panel_ui_lan_cache_load(void)
{
    nvs_handle_t h;
    if (nvs_open(LAN_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nl_lan_blob_t blob;
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, LAN_NVS_KEY, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || blob.n == 0) {
        return;
    }
    if (blob.n > LAN_PACK_MAX) {
        blob.n = LAN_PACK_MAX;
    }
    nl_ov_ensure_devs_cache();
    if (!s_devs) {
        return;
    }
    for (int i = 0; i < blob.n; i++) {
        const nl_lan_pack_t *p = &blob.items[i];
        if (!p->id[0] || !p->ip[0]) {
            continue;
        }
        nl_dev_cache_t *d = nl_ov_find_dev(p->id);
        if (!d) {
            if (s_dev_n >= NL_DEV_CACHE_MAX) {
                continue;
            }
            d = &s_devs[s_dev_n++];
            std::memset(d, 0, sizeof(*d));
            std::snprintf(d->id, sizeof(d->id), "%s", p->id);
        }
        std::snprintf(d->ip, sizeof(d->ip), "%s", p->ip);
        d->lan_control = true;
        d->pass_required = p->pass_required != 0;
        /* 只恢复 IP；在线态等 DIOL 探测，避免重刷后死设备先亮黄 */
        d->online = false;
    }
    s_lan_pack_sig_saved = lan_pack_sig_now();
    ESP_LOGI(TAG, "lan cache loaded n=%d devs=%d", blob.n, s_dev_n);
}

extern "C" void nodelio_panel_ui_snapshot_end(void)
{
    lan_cache_save();
    s_have_device_snapshot = true;
    const uint32_t after = nodelio_panel_ui_dev_visual_sig();
    if (after == s_snap_vis_before) {
        ESP_LOGD(TAG, "snapshot end: visual unchanged — skip tile refresh");
        return;
    }
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
