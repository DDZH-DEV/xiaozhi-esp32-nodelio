#include "overlay_priv.h"

#include "application.h"
#include "device_caps.h"
#include "layout_cache.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "panel_ui.h"
#include "skill_runtime.h"
#include "wifi_provision.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

LV_FONT_DECLARE(font_material_symbols_20_4);

#define TAG "nl_apps"
#define APP_MAX 16
/* 8 个样例应用带 inputSchema 时整包 >7KB；旧 4KB 截断后 cJSON 失败 →「暂无已安装」。 */
#define APP_LIST_BUF 24576

struct nl_app_item {
    char id[72];
    char name[40];
    char desc[72];
    bool enabled;
};

struct nl_app_snap {
    nl_app_item items[APP_MAX];
    int n;
    char err[48];
};

static lv_obj_t *s_page;
static lv_obj_t *s_list;
static lv_obj_t *s_hint;
static nl_app_item s_items[APP_MAX];
static int s_item_n;
static bool s_loading;
static bool s_have_list;

static void copy_field(char *dst, size_t n, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring && n) {
        snprintf(dst, n, "%s", v->valuestring);
    }
}

static void set_hint(const char *text)
{
    if (!s_hint) {
        return;
    }
    /* 已有应用时不占一行；仅空列表显示加载/失败/暂无 */
    if (s_item_n > 0 || text == nullptr || text[0] == '\0') {
        lv_label_set_text(s_hint, "");
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static void render_list(void)
{
    if (!s_list) {
        return;
    }
    lv_obj_clean(s_list);
    if (s_item_n <= 0) {
        return;
    }
    for (int i = 0; i < s_item_n; i++) {
        const nl_app_item *it = &s_items[i];
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 68);
        lv_obj_set_style_bg_color(row, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_MD, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (it->enabled) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(row, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(row, 1, LV_STATE_PRESSED);
            lv_obj_add_event_cb(
                row,
                [](lv_event_t *e) {
                    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                        return;
                    }
                    int idx = (int)(intptr_t)lv_event_get_user_data(e);
                    if (idx < 0 || idx >= s_item_n) {
                        return;
                    }
                    if (!s_items[idx].enabled) {
                        nl_ov_toast("本机暂不支持");
                        return;
                    }
                    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
                    if (!cfgp) {
                        nl_ov_toast("内存不足");
                        return;
                    }
                    *cfgp = s_cfg;
                    char sid[72];
                    snprintf(sid, sizeof(sid), "%s", s_items[idx].id);
                    struct invoke_arg {
                        nodelio_nvs_config_t *cfg;
                        char id[72];
                    };
                    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
                    if (!arg) {
                        free(cfgp);
                        nl_ov_toast("内存不足");
                        return;
                    }
                    arg->cfg = cfgp;
                    snprintf(arg->id, sizeof(arg->id), "%s", sid);
                    nl_ov_toast("正在打开…");
                    /* PSRAM 栈由 WithCaps 分配并在删除时回收。禁止 xTaskCreateStatic 后再 free TCB：
                     * idle 的 prvDeleteTCB 会撞上已释放内存（ucStaticallyAllocated assert）。 */
                    constexpr uint32_t kStackWords = 16384 / sizeof(StackType_t);
                    if (xTaskCreateWithCaps(
                            [](void *p) {
                                auto *a = static_cast<invoke_arg *>(p);
                                const bool music_app = strcmp(a->id, "music.search") == 0 ||
                                                       strcmp(a->id, "music_search") == 0;
                                if (!music_app && nodelio_media_preempt_for_heavy_app()) {
                                    nodelio_panel_ui_toast_defer("已停止音乐以打开应用");
                                }
                                char base[160];
                                nl_url_strip_base(a->cfg->server_url, base, sizeof(base));
                                char url[400];
                                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base,
                                         a->cfg->device_id, a->id);
                                const int cap = nodelio_caps_max_content_bytes() + 512;
                                char *resp = static_cast<char *>(
                                    heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                                if (!resp) {
                                    resp = static_cast<char *>(malloc((size_t)cap));
                                }
                                if (!resp) {
                                    nodelio_panel_ui_toast_defer("内存不足");
                                } else {
                                    resp[0] = '\0';
                                    int status = 0;
                                    /* 天气/空气等要回源公网，默认 2.5s 读超时会 recv=0 → 假「打开失败」 */
                                    esp_err_t err = nodelio_http_post_json_ms(
                                        url, a->cfg->device_key, "{\"keyword\":\"\"}", &status, resp, cap - 1,
                                        10000);
                                    /* E5：4xx/5xx 若体仍是 Envelope（error）也要上屏，不假成功 */
                                    if (err == ESP_OK && resp[0] &&
                                        (status == 200 || status == 201 ||
                                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                                        nodelio_skill_deliver_json(a->cfg, resp, false, nullptr, 0);
                                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                                        nodelio_panel_ui_toast_defer("服务器无响应");
                                    } else if (status == 401 || status == 403) {
                                        nodelio_panel_ui_toast_defer("未授权");
                                    } else {
                                        nodelio_panel_ui_toast_defer("打开失败");
                                    }
                                    free(resp);
                                }
                                free(a->cfg);
                                free(a);
                                vTaskDeleteWithCaps(nullptr);
                            },
                            "nl_app_open", kStackWords, arg, 3, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                        free(arg->cfg);
                        free(arg);
                        nl_ov_toast("无法打开");
                    }
                },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        lv_obj_t *pod = lv_obj_create(row);
        lv_obj_remove_style_all(pod);
        lv_obj_set_size(pod, 36, 36);
        lv_obj_set_style_radius(pod, 10, 0);
        lv_obj_set_style_bg_color(pod, it->enabled ? UI_TILE_SWITCH : UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
        lv_obj_clear_flag(pod, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = lv_label_create(pod);
        lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
        const bool music = strcmp(it->id, "music.search") == 0 || strcmp(it->id, "music_search") == 0;
        lv_label_set_text(ic, music ? MATERIAL_SYMBOLS_MUSIC_NOTE : MATERIAL_SYMBOLS_EXPLORE);
        lv_obj_set_style_text_color(ic, it->enabled ? UI_ACCENT_SOFT : UI_TEXT_MUTED, 0);
        lv_obj_center(ic);

        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(txt, 2, 0);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *title = lv_label_create(txt);
        nl_ov_set_font(title);
        lv_label_set_text(title, it->name[0] ? it->name : it->id);
        lv_obj_set_style_text_color(title, UI_TEXT, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, 250);
        lv_obj_t *sub = lv_label_create(txt);
        nl_ov_set_font(sub);
        lv_label_set_text(sub, it->desc[0] ? it->desc : "点按打开");
        lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 250);

        lv_obj_t *badge = lv_label_create(row);
        nl_ov_set_font(badge);
        lv_label_set_text(badge, it->enabled ? "可用" : "不可用");
        lv_obj_set_style_text_color(badge, it->enabled ? UI_ACCENT_SOFT : UI_TEXT_MUTED, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    }
}

static bool items_from_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    int n = 0;
    const cJSON *it = nullptr;
    nl_app_item tmp[APP_MAX];
    memset(tmp, 0, sizeof(tmp));
    cJSON_ArrayForEach(it, root) {
        if (n >= APP_MAX) {
            break;
        }
        copy_field(tmp[n].id, sizeof(tmp[n].id), it, "id");
        if (!tmp[n].id[0]) {
            continue;
        }
        copy_field(tmp[n].name, sizeof(tmp[n].name), it, "name");
        copy_field(tmp[n].desc, sizeof(tmp[n].desc), it, "desc");
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "en");
        tmp[n].enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
        n++;
    }
    cJSON_Delete(root);
    s_item_n = n;
    if (n > 0) {
        memcpy(s_items, tmp, sizeof(nl_app_item) * (size_t)n);
    }
    s_have_list = true;
    return true;
}

static void persist_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }
    for (int i = 0; i < s_item_n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            continue;
        }
        cJSON_AddStringToObject(o, "id", s_items[i].id);
        cJSON_AddStringToObject(o, "name", s_items[i].name);
        cJSON_AddStringToObject(o, "desc", s_items[i].desc);
        cJSON_AddBoolToObject(o, "en", s_items[i].enabled);
        cJSON_AddItemToArray(arr, o);
    }
    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        return;
    }
    (void)nodelio_apps_cache_save(printed, strlen(printed));
    cJSON_free(printed);
}

static void load_disk_cache(void)
{
    if (s_have_list) {
        return;
    }
    char *json = nullptr;
    if (nodelio_apps_cache_load(&json) != ESP_OK || !json) {
        return;
    }
    if (!items_from_json(json)) {
        ESP_LOGW(TAG, "apps cache parse fail");
    }
    free(json);
}

static void apply_snap(nl_app_snap *snap)
{
    Application::GetInstance().Schedule([snap]() {
        DisplayLockGuard lock(nl_ov_disp());
        s_loading = false;
        if (!lock) {
            free(snap);
            return;
        }
        if (snap->err[0]) {
            if (!s_have_list) {
                s_item_n = 0;
                set_hint(snap->err);
                render_list();
            } else {
                nl_ov_toast(snap->err);
            }
            free(snap);
            return;
        }
        s_item_n = snap->n;
        if (s_item_n > APP_MAX) {
            s_item_n = APP_MAX;
        }
        memcpy(s_items, snap->items, sizeof(nl_app_item) * (size_t)s_item_n);
        s_have_list = true;
        set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
        render_list();
        persist_list();
        /* 钉选可能变化：首页下次 ensure 再拉 */
        nl_ov_home_skills_invalidate();
        free(snap);
    });
}

void nl_ov_apps_ensure(void)
{
    if (!s_page) {
        return;
    }
    if (!s_have_list) {
        load_disk_cache();
    }
    if (s_have_list) {
        /* 已画过则只显示，不重拉、不重建（避免每次进页都滚回顶部） */
        if (s_list && lv_obj_get_child_cnt(s_list) == 0) {
            set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            render_list();
        }
        return;
    }
    nl_ov_apps_refresh();
}

void nl_ov_apps_refresh(void)
{
    if (!s_page || s_loading) {
        return;
    }
    if (!s_cfg.server_url[0] || !s_cfg.device_id[0]) {
        s_item_n = 0;
        set_hint("未绑定服务器");
        render_list();
        return;
    }
    s_loading = true;
    set_hint("正在加载…");
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        s_loading = false;
        set_hint("内存不足");
        return;
    }
    *cfgp = s_cfg;
    /* 栈深按字；16KB 放 PSRAM。删除必须 WithCaps，勿事后 free TCB。 */
    constexpr uint32_t kListStackWords = 16384 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *cfg = static_cast<nodelio_nvs_config_t *>(p);
                auto *snap = static_cast<nl_app_snap *>(calloc(1, sizeof(nl_app_snap)));
                if (!snap) {
                    nodelio_panel_ui_toast_defer("内存不足");
                    free(cfg);
                    Application::GetInstance().Schedule([]() { s_loading = false; });
                    vTaskDeleteWithCaps(nullptr);
                    return;
                }
                char base[160];
                nl_url_strip_base(cfg->server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);
                char *buf = static_cast<char *>(heap_caps_malloc(APP_LIST_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!buf) {
                    buf = static_cast<char *>(malloc(APP_LIST_BUF));
                }
                if (!buf) {
                    snprintf(snap->err, sizeof(snap->err), "内存不足");
                    apply_snap(snap);
                    free(cfg);
                    vTaskDeleteWithCaps(nullptr);
                    return;
                }
                buf[0] = '\0';
                int status = 0;
                esp_err_t err = ESP_FAIL;
                /* 唤醒空窗：少试几次；连不上就停，避免拖死 HTTP 互斥锁 */
                static const uint32_t k_retry_ms[] = {0, 800, 2000};
                for (unsigned attempt = 0; attempt < sizeof(k_retry_ms) / sizeof(k_retry_ms[0]); attempt++) {
                    if (k_retry_ms[attempt]) {
                        vTaskDelay(pdMS_TO_TICKS(k_retry_ms[attempt]));
                    }
                    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
                        err = ESP_ERR_INVALID_STATE;
                        break;
                    }
                    buf[0] = '\0';
                    status = 0;
                    err = nodelio_http_get(url, cfg->device_key, nullptr, &status, buf, APP_LIST_BUF - 1);
                    if (err == ESP_OK && (status == 200 || status == 201)) {
                        break;
                    }
                    ESP_LOGW(TAG, "list skills attempt %u status=%d err=%d", attempt + 1, status, err);
                }
                if (err != ESP_OK || (status != 200 && status != 201)) {
                    snprintf(snap->err, sizeof(snap->err), "加载失败");
                    ESP_LOGW(TAG, "list skills status=%d err=%d", status, err);
                } else {
                    cJSON *root = cJSON_Parse(buf);
                    if (!root) {
                        snprintf(snap->err, sizeof(snap->err), "列表过大");
                        ESP_LOGW(TAG, "list skills JSON parse fail len=%u", (unsigned)strlen(buf));
                    } else {
                        const cJSON *skills = cJSON_GetObjectItemCaseSensitive(root, "skills");
                        if (cJSON_IsArray(skills)) {
                            const cJSON *it = nullptr;
                            cJSON_ArrayForEach(it, skills) {
                                if (snap->n >= APP_MAX) {
                                    break;
                                }
                                nl_app_item *row = &snap->items[snap->n];
                                copy_field(row->id, sizeof(row->id), it, "skillId");
                                if (!row->id[0]) {
                                    copy_field(row->id, sizeof(row->id), it, "instanceId");
                                }
                                if (!row->id[0]) {
                                    continue;
                                }
                                copy_field(row->name, sizeof(row->name), it, "name");
                                copy_field(row->desc, sizeof(row->desc), it, "description");
                                const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
                                row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
                                snap->n++;
                            }
                        }
                        /* skills 截断/异常时回退瘦身 apps[] */
                        if (snap->n == 0) {
                            const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
                            if (cJSON_IsArray(apps)) {
                                const cJSON *it = nullptr;
                                cJSON_ArrayForEach(it, apps) {
                                    if (snap->n >= APP_MAX) {
                                        break;
                                    }
                                    nl_app_item *row = &snap->items[snap->n];
                                    copy_field(row->id, sizeof(row->id), it, "instance_id");
                                    if (!row->id[0]) {
                                        copy_field(row->id, sizeof(row->id), it, "app_id");
                                    }
                                    if (!row->id[0]) {
                                        continue;
                                    }
                                    copy_field(row->name, sizeof(row->name), it, "name");
                                    copy_field(row->desc, sizeof(row->desc), it, "summary");
                                    const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
                                    row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
                                    snap->n++;
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                free(buf);
                free(cfg);
                apply_snap(snap);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_apps", kListStackWords, cfgp, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(cfgp);
        s_loading = false;
        set_hint("无法加载");
    }
}

lv_obj_t *nl_ov_apps_scroll_obj(void)
{
    if (!s_page || !s_list) {
        return nullptr;
    }
    if (lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    if (!lv_obj_has_flag(s_list, LV_OBJ_FLAG_SCROLLABLE)) {
        return nullptr;
    }
    return s_list;
}

void nl_ov_apps_set_visible(bool on)
{
    if (!s_page) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    }
}

bool nl_ov_apps_open_by_query(const char *query)
{
    if (!query || !query[0]) {
        return false;
    }
    /* 先扫已缓存列表（应用页拉过一次就有） */
    int hit = -1;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_items[i].enabled) {
            continue;
        }
        if (strstr(s_items[i].name, query) || strstr(s_items[i].id, query) ||
            strstr(query, s_items[i].name) || strstr(query, s_items[i].id)) {
            hit = i;
            break;
        }
    }
    /* 样例市场别名（列表未刷时也能开） */
    const char *alias_id = nullptr;
    if (hit < 0) {
        if (strstr(query, "曲库") || strstr(query, "音乐") || strstr(query, "music")) {
            alias_id = "music.search";
        } else if (strstr(query, "新闻") || strstr(query, "news")) {
            alias_id = "news.latest";
        } else if (strstr(query, "天气") || strstr(query, "weather")) {
            alias_id = "weather.today";
        } else if (strstr(query, "电台") || strstr(query, "radio")) {
            alias_id = "radio.stations";
        } else if (strstr(query, "播客") || strstr(query, "podcast")) {
            alias_id = "podcast.latest";
        } else if (strstr(query, "空气") || strstr(query, "air")) {
            alias_id = "air.now";
        } else if (strstr(query, "一句") || strstr(query, "名言") || strstr(query, "quote")) {
            alias_id = "quote.daily";
        }
    }
    const char *sid = nullptr;
    if (hit >= 0) {
        sid = s_items[hit].id;
    } else if (alias_id) {
        sid = alias_id;
    }
    if (!sid || !sid[0]) {
        return false;
    }
    nl_ov_invoke_skill_async(sid, "{}");
    return true;
}

void nl_ov_build_apps(void)
{
    if (s_page || !s_body) {
        return;
    }
    s_page = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_page, 16, 0);
    lv_obj_set_style_pad_ver(s_page, 8, 0);
    lv_obj_set_style_pad_row(s_page, 4, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    load_disk_cache();

    lv_obj_t *hdr = lv_obj_create(s_page);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, 40);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    nl_ov_set_font(title);
    lv_label_set_text(title, "已安装应用");
    lv_obj_set_style_text_color(title, UI_TEXT, 0);

    lv_obj_t *ref = lv_obj_create(hdr);
    lv_obj_remove_style_all(ref);
    lv_obj_set_size(ref, 36, 36);
    lv_obj_set_style_radius(ref, 18, 0);
    lv_obj_set_style_bg_color(ref, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(ref, LV_OPA_COVER, 0);
    lv_obj_add_flag(ref, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ref, 8);
    lv_obj_add_event_cb(ref, [](lv_event_t *) { nl_ov_apps_refresh(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ric = lv_label_create(ref);
    lv_obj_set_style_text_font(ric, &font_material_symbols_20_4, 0);
    lv_label_set_text(ric, MATERIAL_SYMBOLS_REFRESH);
    lv_obj_set_style_text_color(ric, UI_ACCENT_SOFT, 0);
    lv_obj_center(ric);

    s_hint = lv_label_create(s_page);
    nl_ov_set_font(s_hint);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, UI_TEXT_MUTED, 0);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
}
