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
#include "panel_jobs.h"
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

static void notice_load_disk_cache(void);

extern "C" esp_err_t nodelio_panel_ui_start(const nodelio_nvs_config_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
        nodelio_nvs_cache_put(cfg);
    }
    notice_load_disk_cache();
    return nodelio_overlay_init();
}

extern "C" void nodelio_panel_ui_apply_cfg(const nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    nodelio_nvs_cache_put(cfg);
    Display *disp = nl_ov_disp();
    if (disp) {
        DisplayLockGuard lock(disp);
        s_cfg = *cfg;
    } else {
        s_cfg = *cfg;
    }
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
    nl_ov_status_bar_on_screen(on);
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
                if (nl_ov_ai_session_active()) {
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

/* Notice 仓库（PSRAM，最多 20 条）+ 息屏短队列 + 已读墓碑环（防异步 resolve 期间补拉回流） */
static constexpr int kNoticeStoreMax = 20;
static constexpr int kNoticeDeferMax = 5;
static constexpr int kNoticeTombstoneMax = 16;

static nodelio_panel_notice_t *s_notice_items = nullptr;
static int s_notice_n = 0;
static nodelio_panel_notice_t s_notice_defer[kNoticeDeferMax];
static int s_notice_defer_n = 0;
static char s_notice_active_id[40] = {0};
static char s_resolved_ids[kNoticeTombstoneMax][40] = {{0}};
static int s_resolved_ring_idx = 0;

static bool notice_store_ensure(void)
{
    if (s_notice_items) {
        return true;
    }
    s_notice_items = static_cast<nodelio_panel_notice_t *>(
        heap_caps_calloc(kNoticeStoreMax, sizeof(nodelio_panel_notice_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_notice_items) {
        s_notice_items = static_cast<nodelio_panel_notice_t *>(
            calloc(kNoticeStoreMax, sizeof(nodelio_panel_notice_t)));
    }
    return s_notice_items != nullptr;
}

static void notice_copy(nodelio_panel_notice_t *dst, const nodelio_panel_notice_t *src)
{
    if (!dst || !src) {
        return;
    }
    *dst = *src;
}

static bool notice_is_tombstoned(const char *notice_id)
{
    if (!notice_id || !notice_id[0]) {
        return false;
    }
    for (int i = 0; i < kNoticeTombstoneMax; ++i) {
        if (s_resolved_ids[i][0] && strcmp(s_resolved_ids[i], notice_id) == 0) {
            return true;
        }
    }
    return false;
}

static void notice_mark_tombstone(const char *notice_id)
{
    if (!notice_id || !notice_id[0] || notice_is_tombstoned(notice_id)) {
        return;
    }
    snprintf(s_resolved_ids[s_resolved_ring_idx], sizeof(s_resolved_ids[0]), "%s", notice_id);
    s_resolved_ring_idx = (s_resolved_ring_idx + 1) % kNoticeTombstoneMax;
}

static void notice_normalize_defaults(nodelio_panel_notice_t *n)
{
    if (!n) {
        return;
    }
    if (!n->mode[0]) {
        if (strcmp(n->reason, "needs_you") == 0) {
            snprintf(n->mode, sizeof(n->mode), "action");
        } else {
            snprintf(n->mode, sizeof(n->mode), "info");
        }
    }
    if (!n->severity[0]) {
        snprintf(n->severity, sizeof(n->severity), "normal");
    }
    if (!n->coalesce_key[0] && n->session[0]) {
        const char *ka = n->app_id[0] ? n->app_id : "app";
        snprintf(n->coalesce_key, sizeof(n->coalesce_key), "%.70s:session:%.79s", ka, n->session);
    }
}

static void notice_persist_disk_cache(void)
{
    if (!s_notice_items || s_notice_n <= 0) {
        (void)nodelio_notices_cache_clear();
        return;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return;
    }
    cJSON_AddItemToObject(root, "notices", arr);
    for (int i = 0; i < s_notice_n; ++i) {
        const nodelio_panel_notice_t *it = &s_notice_items[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "notice_id", it->notice_id);
        if (it->coalesce_key[0]) {
            cJSON_AddStringToObject(obj, "coalesce_key", it->coalesce_key);
        }
        cJSON_AddStringToObject(obj, "mode", it->mode);
        cJSON_AddStringToObject(obj, "severity", it->severity);
        cJSON_AddStringToObject(obj, "title", it->title);
        if (it->app_id[0]) {
            cJSON_AddStringToObject(obj, "app_id", it->app_id);
        }
        if (it->invoke_app[0]) {
            cJSON *inv = cJSON_CreateObject();
            if (inv) {
                cJSON_AddStringToObject(inv, "app_id", it->invoke_app);
                if (it->invoke_params[0]) {
                    cJSON *pm = cJSON_Parse(it->invoke_params);
                    if (pm) {
                        cJSON_AddItemToObject(inv, "params", pm);
                    }
                }
                cJSON_AddItemToObject(obj, "invoke", inv);
            }
        }
        if (it->session[0]) {
            cJSON_AddStringToObject(obj, "session", it->session);
        }
        if (it->reason[0]) {
            cJSON_AddStringToObject(obj, "reason", it->reason);
        }
        cJSON_AddBoolToObject(obj, "wake", it->wake != 0);
        cJSON_AddItemToArray(arr, obj);
    }
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed) {
        (void)nodelio_notices_cache_save(printed, strlen(printed));
        cJSON_free(printed);
    }
}

/**
 * 写入/合并单条未读通知（0=最新）。
 * @return true=新增或 coalesce 产生新通知；false=已存在同 notice_id 或已在已读墓碑中（勿重复弹 Toast）。
 */
static bool notice_store_upsert(const nodelio_panel_notice_t *in, bool persist)
{
    if (!in || !notice_store_ensure()) {
        return false;
    }
    nodelio_panel_notice_t item;
    notice_copy(&item, in);
    notice_normalize_defaults(&item);

    if (item.notice_id[0] && notice_is_tombstoned(item.notice_id)) {
        return false;
    }

    /* 1) notice_id 精确去重：已存在则原地更新，返回 false 阻止重复弹窗 */
    if (item.notice_id[0]) {
        for (int i = 0; i < s_notice_n; ++i) {
            if (strcmp(s_notice_items[i].notice_id, item.notice_id) == 0) {
                s_notice_items[i] = item;
                if (persist) {
                    notice_persist_disk_cache();
                }
                return false;
            }
        }
    }

    /* 2) coalesce_key 同类合并：删掉旧同源条目，新条目置顶 */
    if (item.coalesce_key[0]) {
        for (int i = 0; i < s_notice_n; ++i) {
            if (s_notice_items[i].coalesce_key[0] &&
                strcmp(s_notice_items[i].coalesce_key, item.coalesce_key) == 0) {
                if (i + 1 < s_notice_n) {
                    memmove(&s_notice_items[i], &s_notice_items[i + 1],
                            sizeof(s_notice_items[0]) * (size_t)(s_notice_n - i - 1));
                }
                --s_notice_n;
                break;
            }
        }
    }

    /* 3) 置顶插入（0=最新），超出 kNoticeStoreMax 丢弃最旧 */
    if (s_notice_n >= kNoticeStoreMax) {
        s_notice_n = kNoticeStoreMax - 1;
    }
    if (s_notice_n > 0) {
        memmove(&s_notice_items[1], &s_notice_items[0],
                sizeof(s_notice_items[0]) * (size_t)s_notice_n);
    }
    s_notice_items[0] = item;
    ++s_notice_n;
    if (persist) {
        notice_persist_disk_cache();
    }
    return true;
}

static bool notice_store_remove_by_id(const char *notice_id, nodelio_panel_notice_t *out_removed)
{
    if (!notice_id || !notice_id[0] || !s_notice_items) {
        return false;
    }
    for (int i = 0; i < s_notice_n; ++i) {
        if (strcmp(s_notice_items[i].notice_id, notice_id) == 0) {
            if (out_removed) {
                *out_removed = s_notice_items[i];
            }
            if (i + 1 < s_notice_n) {
                memmove(&s_notice_items[i], &s_notice_items[i + 1],
                        sizeof(s_notice_items[0]) * (size_t)(s_notice_n - i - 1));
            }
            --s_notice_n;
            notice_persist_disk_cache();
            return true;
        }
    }
    return false;
}

static bool parse_notice_json_item(const cJSON *obj, nodelio_panel_notice_t *out)
{
    if (!cJSON_IsObject(obj) || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *nid = cJSON_GetObjectItemCaseSensitive(obj, "notice_id");
    const cJSON *ck = cJSON_GetObjectItemCaseSensitive(obj, "coalesce_key");
    const cJSON *inst = cJSON_GetObjectItemCaseSensitive(obj, "instance_id");
    const cJSON *app = cJSON_GetObjectItemCaseSensitive(obj, "app_id");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(obj, "mode");
    const cJSON *sev = cJSON_GetObjectItemCaseSensitive(obj, "severity");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(obj, "title");
    const cJSON *reason = cJSON_GetObjectItemCaseSensitive(obj, "reason");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(obj, "session");
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(obj, "wake");
    const cJSON *inv = cJSON_GetObjectItemCaseSensitive(obj, "invoke");
    if (cJSON_IsString(nid) && nid->valuestring) {
        snprintf(out->notice_id, sizeof(out->notice_id), "%s", nid->valuestring);
    }
    if (cJSON_IsString(ck) && ck->valuestring) {
        snprintf(out->coalesce_key, sizeof(out->coalesce_key), "%s", ck->valuestring);
    }
    if (cJSON_IsString(mode) && mode->valuestring) {
        snprintf(out->mode, sizeof(out->mode), "%s", mode->valuestring);
    }
    if (cJSON_IsString(sev) && sev->valuestring) {
        snprintf(out->severity, sizeof(out->severity), "%s", sev->valuestring);
    }
    if (cJSON_IsString(title) && title->valuestring) {
        snprintf(out->title, sizeof(out->title), "%s", title->valuestring);
    }
    if (cJSON_IsString(reason) && reason->valuestring) {
        snprintf(out->reason, sizeof(out->reason), "%s", reason->valuestring);
    }
    if (cJSON_IsString(session) && session->valuestring) {
        snprintf(out->session, sizeof(out->session), "%s", session->valuestring);
    }
    if (cJSON_IsTrue(wake)) {
        out->wake = 1;
    }
    if (cJSON_IsString(app) && app->valuestring) {
        snprintf(out->app_id, sizeof(out->app_id), "%s", app->valuestring);
    } else if (cJSON_IsString(inst) && inst->valuestring) {
        snprintf(out->app_id, sizeof(out->app_id), "%s", inst->valuestring);
    }
    if (cJSON_IsObject(inv)) {
        const cJSON *ia = cJSON_GetObjectItemCaseSensitive(inv, "app_id");
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(inv, "params");
        if (cJSON_IsString(ia) && ia->valuestring) {
            snprintf(out->invoke_app, sizeof(out->invoke_app), "%s", ia->valuestring);
        }
        if (ip) {
            char *printed = cJSON_PrintUnformatted(ip);
            if (printed) {
                snprintf(out->invoke_params, sizeof(out->invoke_params), "%s", printed);
                cJSON_free(printed);
            }
        }
    }
    if (!out->invoke_app[0] && out->app_id[0]) {
        snprintf(out->invoke_app, sizeof(out->invoke_app), "%s", out->app_id);
    }
    notice_normalize_defaults(out);
    return out->notice_id[0] != '\0' || out->title[0] != '\0' || out->app_id[0] != '\0';
}

static void notice_load_disk_cache(void)
{
    char *json = nullptr;
    if (nodelio_notices_cache_load(&json) != ESP_OK || !json) {
        return;
    }
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        return;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "notices");
    if (cJSON_IsArray(arr) && notice_store_ensure()) {
        s_notice_n = 0;
        const int total = cJSON_GetArraySize(arr);
        /* 数组 0=最新，逆序 upsert 或顺次追加保持原顺序 */
        for (int i = total - 1; i >= 0; --i) {
            nodelio_panel_notice_t item;
            if (parse_notice_json_item(cJSON_GetArrayItem(arr, i), &item)) {
                (void)notice_store_upsert(&item, false);
            }
        }
        ESP_LOGI(TAG, "loaded %d unread notices from cache", s_notice_n);
    }
    cJSON_Delete(root);
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
    if (strcmp(n->mode, "alert") == 0 && strcmp(n->severity, "critical") == 0) {
        nodelio_panel_ui_play_cue(0);
    }

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
    /* 息屏队列内按 notice_id / coalesce_key 去重，避免息屏期间重复堆积 */
    for (int i = 0; i < s_notice_defer_n; ++i) {
        if ((n->notice_id[0] && strcmp(s_notice_defer[i].notice_id, n->notice_id) == 0) ||
            (n->coalesce_key[0] && strcmp(s_notice_defer[i].coalesce_key, n->coalesce_key) == 0)) {
            notice_copy(&s_notice_defer[i], n);
            return;
        }
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
        if (!id) {
            return;
        }
        notice_mark_tombstone(id);
        (void)notice_store_remove_by_id(id, nullptr);
        if (s_notice_active_id[0] && strcmp(s_notice_active_id, id) == 0) {
            s_notice_active_id[0] = '\0';
        }
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
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_notices_refresh_badge();
            }
        }
        free(id);
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
    notice_normalize_defaults(copy);
    Application::GetInstance().Schedule([copy]() {
        /* 先入库去重：若已存在同 notice_id 或已在墓碑中，直接跳过，绝不重复弹 Toast */
        if (!notice_store_upsert(copy, true)) {
            free(copy);
            return;
        }
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_notices_refresh_badge();
            }
        }
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

extern "C" void nodelio_panel_ui_apply_notices_http_body(const char *json)
{
    if (!json || !json[0]) {
        return;
    }
    char *dup = strdup(json);
    if (!dup) {
        return;
    }
    Application::GetInstance().Schedule([dup]() {
        cJSON *root = cJSON_Parse(dup);
        free(dup);
        if (!root) {
            return;
        }
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "notices");
        if (cJSON_IsArray(arr) && notice_store_ensure()) {
            /* 以服务端 pending 列表为真值静默对齐（过滤本地刚点已读的墓碑 ID，去重，不弹 Toast） */
            s_notice_n = 0;
            const int total = cJSON_GetArraySize(arr);
            for (int i = total - 1; i >= 0; --i) {
                nodelio_panel_notice_t item;
                if (parse_notice_json_item(cJSON_GetArrayItem(arr, i), &item)) {
                    (void)notice_store_upsert(&item, false);
                }
            }
            notice_persist_disk_cache();
            {
                DisplayLockGuard lock(nl_ov_disp());
                if (lock) {
                    nl_ov_notices_refresh_badge();
                }
            }
            ESP_LOGI(TAG, "reconciled %d unread notices from cloud", s_notice_n);
        }
        cJSON_Delete(root);
    });
}

static void notices_sync_worker(void *arg)
{
    (void)arg;
    nodelio_nvs_config_t cfg;
    if (!nl_ov_cfg_http_ready(&cfg, false) || !cfg.device_key[0]) {
        return;
    }
    char base[160];
    nl_url_strip_base(cfg.server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/notices", base, cfg.device_id);
    const int cap = 8 * 1024;
    char *body = static_cast<char *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!body) {
        body = static_cast<char *>(malloc(cap));
    }
    if (!body) {
        return;
    }
    body[0] = '\0';
    int status = 0;
    if (nodelio_http_get_ms(url, cfg.device_key, nullptr, &status, body, cap, 6000) == ESP_OK &&
        status == 200) {
        nodelio_panel_ui_apply_notices_http_body(body);
    }
    free(body);
}

extern "C" void nodelio_panel_ui_notices_sync_async(void)
{
    (void)nodelio::SubmitPanelJob(nodelio::PanelJobLane::Background, notices_sync_worker, nullptr,
                                  nullptr, 0, "notices.sync", true);
}

extern "C" int nodelio_panel_ui_notice_unread_count(void)
{
    return s_notice_n;
}

extern "C" const nodelio_panel_notice_t *nodelio_panel_ui_notice_get(int index)
{
    if (!s_notice_items || index < 0 || index >= s_notice_n) {
        return nullptr;
    }
    return &s_notice_items[index];
}

static void notice_resolve_worker(void *arg)
{
    char *nid = static_cast<char *>(arg);
    if (!nid) {
        return;
    }
    nodelio_nvs_config_t cfg;
    if (nl_ov_cfg_http_ready(&cfg, false) && cfg.device_key[0] && nid[0]) {
        char base[160];
        nl_url_strip_base(cfg.server_url, base, sizeof(base));
        char url[280];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/notices/%s/resolve", base,
                 cfg.device_id, nid);
        int status = 0;
        (void)nodelio_http_post_json_ms(url, cfg.device_key, "{}", &status, nullptr, 0, 5000);
    }
    free(nid);
}

extern "C" void nodelio_panel_ui_notice_resolve(const char *notice_id, bool open_target)
{
    if (!notice_id || !notice_id[0]) {
        return;
    }
    char *id = strdup(notice_id);
    if (!id) {
        return;
    }
    Application::GetInstance().Schedule([id, open_target]() {
        notice_mark_tombstone(id);
        nodelio_panel_notice_t removed;
        memset(&removed, 0, sizeof(removed));
        const bool found = notice_store_remove_by_id(id, &removed);
        if (s_notice_active_id[0] && strcmp(s_notice_active_id, id) == 0) {
            s_notice_active_id[0] = '\0';
        }
        if (open_target && found && (removed.invoke_app[0] || removed.app_id[0])) {
            notice_do_invoke(&removed);
        }
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_notices_refresh_badge();
            }
        }
        if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Control, notice_resolve_worker, id,
                                     free, 0, nullptr, true)) {
            free(id);
        }
    });
}

extern "C" void nodelio_panel_ui_notice_resolve_all(void)
{
    Application::GetInstance().Schedule([]() {
        if (!s_notice_items || s_notice_n <= 0) {
            return;
        }
        for (int i = 0; i < s_notice_n; ++i) {
            const char *nid = s_notice_items[i].notice_id;
            if (!nid[0]) {
                continue;
            }
            notice_mark_tombstone(nid);
            char *id_copy = strdup(nid);
            if (id_copy) {
                if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Control, notice_resolve_worker,
                                             id_copy, free, 0, nullptr, true)) {
                    free(id_copy);
                }
            }
        }
        s_notice_n = 0;
        s_notice_active_id[0] = '\0';
        s_notice_defer_n = 0;
        notice_persist_disk_cache();
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_notices_refresh_badge();
        }
    });
}

extern "C" void nodelio_panel_ui_open_notices(void)
{
    Application::GetInstance().Schedule([]() {
        if (!nodelio_overlay_visible()) {
            nodelio_overlay_show(false);
        }
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_open_notices();
        }
    });
}

extern "C" void nodelio_panel_ui_close_notices(void)
{
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_close_notices();
        }
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

