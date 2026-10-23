#include "overlay_priv.h"

#include "application.h"
#include "device_caps.h"
#include "layout_cache.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "content_envelope.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
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

/** 仅供进度卡「允许/拒绝」按钮 invoke，不进应用列表 / 语音打开。 */
static bool skill_is_button_only(const char *id)
{
    return id && strstr(id, "dsh.answer") != nullptr;
}
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

static bool apps_list_eq(const nl_app_item *items, int n)
{
    if (n != s_item_n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (items[i].enabled != s_items[i].enabled) {
            return false;
        }
        if (strcmp(items[i].id, s_items[i].id) != 0 || strcmp(items[i].name, s_items[i].name) != 0 ||
            strcmp(items[i].desc, s_items[i].desc) != 0) {
            return false;
        }
    }
    return true;
}

static void render_list(void)
{
    if (!s_list) {
        return;
    }
    /* 重建前记下滚动位置，避免刷新后跳顶/闪一下像闪屏 */
    const lv_coord_t keep_y = lv_obj_get_scroll_y(s_list);
    lv_obj_clean(s_list);
    if (s_item_n <= 0) {
        return;
    }
    /* 与设置页一致：半宽两列 */
    const int pad_x = 16;
    const int tile_w = (LV_HOR_RES - pad_x * 2 - UI_GAP) / 2;
    for (int i = 0; i < s_item_n; i++) {
        const nl_app_item *it = &s_items[i];
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, tile_w, UI_SETTINGS_TILE_H);
        lv_obj_set_style_bg_color(row, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_MD, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (it->enabled) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(row, 6);
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
                    /* 派活须带 instruction；点应用只提示口令 */
                    if (strstr(s_items[idx].id, "dsh.task") != nullptr) {
                        (void)nl_ov_apps_invoke_dsh_task(nullptr);
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
                                const bool music_app = nodelio_skill_id_is_music(a->id);
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
                                    /* 打开即播与否由 Host Envelope display.auto_play（技能配置）决定 */
                                    esp_err_t err = nodelio_http_post_json_ms(
                                        url, a->cfg->device_key, "{\"keyword\":\"\"}", &status, resp, cap - 1,
                                        10000);
                                    /* E5：4xx/5xx 若体仍是 Envelope（error）也要上屏，不假成功 */
                                    if (err == ESP_OK && resp[0] &&
                                        (status == 200 || status == 201 ||
                                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                                        ESP_LOGI(TAG, "deliver id=%s music=%d status=%d", a->id,
                                                 music_app ? 1 : 0, status);
                                        char spoken[NL_ENV_SPOKEN];
                                        spoken[0] = '\0';
                                        nodelio_skill_deliver_json(a->cfg, resp, false, spoken,
                                                                   sizeof(spoken));
                                        /* DSH 列表：无 spoken.audio 仅 toast；详情 WAV 另走 announce_ex */
                                        if (strstr(a->id, "dsh") != nullptr && spoken[0]) {
                                            const char *au = nodelio_skill_last_spoken_audio_url();
                                            nodelio_panel_ui_announce_spoken_ex(spoken, au);
                                        }
                                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                                        nodelio_panel_ui_toast_defer("服务器无响应");
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
        const bool music = nodelio_skill_id_is_music(it->id);
        lv_label_set_text(ic, music ? MATERIAL_SYMBOLS_MUSIC_NOTE : MATERIAL_SYMBOLS_EXPLORE);
        lv_obj_set_style_text_color(ic, it->enabled ? UI_ACCENT_SOFT : UI_TEXT_MUTED, 0);
        lv_obj_center(ic);

        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_height(txt, lv_pct(100));
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(txt, 2, 0);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *title = lv_label_create(txt);
        nl_ov_set_font(title);
        lv_label_set_text(title, it->name[0] ? it->name : it->id);
        lv_obj_set_style_text_color(title, UI_TEXT, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, lv_pct(100));
        const lv_font_t *card_font = lv_obj_get_style_text_font(title, LV_PART_MAIN);
        const int32_t line_h = card_font ? lv_font_get_line_height(card_font) : 16;
        lv_obj_set_height(title, line_h);
        lv_obj_t *sub = lv_label_create(txt);
        nl_ov_set_font(sub);
        if (!it->enabled) {
            lv_label_set_text(sub, "不可用");
            lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
        } else if (it->desc[0]) {
            lv_label_set_text(sub, it->desc);
            lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
        } else {
            lv_label_set_text(sub, "可用");
            lv_obj_set_style_text_color(sub, UI_ACCENT_SOFT, 0);
        }
        /* 标题 1 行 + 描述 2 行打点；超长省略，不挤成第三行标题 */
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, lv_pct(100));
        lv_obj_set_height(sub, line_h * 2);
    }
    if (keep_y > 0) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, keep_y, LV_ANIM_OFF);
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
        s_loading = false;
        if (snap->err[0]) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                if (!s_have_list) {
                    s_item_n = 0;
                    set_hint(snap->err);
                    render_list();
                } else {
                    nl_ov_toast(snap->err);
                }
            }
            free(snap);
            return;
        }
        const int n = snap->n > APP_MAX ? APP_MAX : snap->n;
        /* 清单未变：不 clean/重建、不反复写 NVS（否则刷新必闪屏 + NVS 空间告警） */
        if (s_have_list && apps_list_eq(snap->items, n)) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            }
            free(snap);
            return;
        }
        /* 先落盘再刷 UI：MQTT skills 推送时应用页可能未显示，锁失败也不能丢缓存 */
        s_item_n = n;
        if (s_item_n > 0) {
            memcpy(s_items, snap->items, sizeof(nl_app_item) * (size_t)s_item_n);
        }
        s_have_list = true;
        persist_list();
        nl_ov_home_skills_invalidate();
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
                render_list();
            }
        }
        free(snap);
    });
}

static void fill_snap_from_skills_root(const cJSON *root, nl_app_snap *snap)
{
    if (!root || !snap) {
        return;
    }
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
            if (!row->id[0] || skill_is_button_only(row->id)) {
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
                if (!row->id[0] || skill_is_button_only(row->id)) {
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
}

void nl_ov_apps_ingest_skills_root(const cJSON *root)
{
    if (!root) {
        return;
    }
    auto *snap = static_cast<nl_app_snap *>(calloc(1, sizeof(nl_app_snap)));
    if (!snap) {
        return;
    }
    fill_snap_from_skills_root(root, snap);
    const int n = snap->n > APP_MAX ? APP_MAX : snap->n;
    const bool same = s_have_list && apps_list_eq(snap->items, n);
    if (!same) {
        /* 同步落盘；UI 延后。不 invalidate 钉选——bootstrap 会紧接着 ingest pins */
        s_item_n = n;
        if (s_item_n > 0) {
            memcpy(s_items, snap->items, sizeof(nl_app_item) * (size_t)s_item_n);
        } else {
            s_item_n = 0;
        }
        s_have_list = true;
        persist_list();
    } else {
        s_have_list = true;
    }
    Application::GetInstance().Schedule([snap, same]() {
        s_loading = false;
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            if (!same) {
                render_list();
            }
        }
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
    /* 不依赖 s_page：云端装/卸推 skills 时也要拉清单并 persist，否则重启仍读旧缓存 */
    if (s_loading) {
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
                        fill_snap_from_skills_root(root, snap);
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

/** 在已装列表里找 app_id 后缀匹配（兼容 market::app_id）。prefer_status：DSH 浏览优先进度。 */
static const char *find_installed_app_id(const char *app_key, bool prefer_status)
{
    if (!app_key || !app_key[0]) {
        return nullptr;
    }
    const char *fallback = nullptr;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_items[i].enabled || !s_items[i].id[0]) {
            continue;
        }
        const char *id = s_items[i].id;
        const char *tail = strstr(id, "::");
        const char *key = tail ? tail + 2 : id;
        if (strcmp(key, app_key) != 0 && strstr(id, app_key) == nullptr) {
            continue;
        }
        if (prefer_status && strstr(id, "dsh.status") != nullptr) {
            return id;
        }
        if (!fallback) {
            fallback = id;
        }
    }
    return fallback;
}

void nl_ov_apps_invoke_dsh_status(const char *workspace, const char *session)
{
    const char *sid = find_installed_app_id("dsh.status", true);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.status";
    }
    char ws_esc[220];
    char sess_esc[160];
    size_t wi = 0;
    ws_esc[0] = '\0';
    if (workspace && workspace[0]) {
        for (const char *p = workspace; *p && wi + 2 < sizeof(ws_esc); ++p) {
            if (*p == '\\' || *p == '"') {
                ws_esc[wi++] = '\\';
            }
            ws_esc[wi++] = *p;
        }
        ws_esc[wi] = '\0';
    }
    sess_esc[0] = '\0';
    if (session && session[0]) {
        size_t si = 0;
        for (const char *p = session; *p && si + 2 < sizeof(sess_esc); ++p) {
            if (*p == '\\' || *p == '"') {
                sess_esc[si++] = '\\';
            }
            sess_esc[si++] = *p;
        }
        sess_esc[si] = '\0';
    }
    /* ws_esc≤219 + sess_esc≤159 + 外壳 30 → ≤408 */
    char body[420];
    if (ws_esc[0] && sess_esc[0]) {
        snprintf(body, sizeof(body), "{\"workspace\":\"%s\",\"session\":\"%s\"}", ws_esc, sess_esc);
    } else if (sess_esc[0]) {
        snprintf(body, sizeof(body), "{\"session\":\"%s\"}", sess_esc);
    } else if (ws_esc[0]) {
        snprintf(body, sizeof(body), "{\"workspace\":\"%s\"}", ws_esc);
    } else {
        snprintf(body, sizeof(body), "{}");
    }
    ESP_LOGI(TAG, "invoke dsh.status sid=%s body=%s", sid, body);
    nl_ov_invoke_skill_async(sid, body);
}

bool nl_ov_apps_invoke_dsh_task(const char *instruction)
{
    if (!instruction || !instruction[0]) {
        nl_ov_toast("请说：让它 + 要做的事");
        nodelio_panel_ui_announce_spoken("请说让它，再加上要做的事。");
        return true;
    }
    char esc[180];
    size_t o = 0;
    for (const char *p = instruction; *p && o + 1 < sizeof(esc); ++p) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            continue;
        }
        esc[o++] = *p;
    }
    esc[o] = '\0';
    if (!esc[0]) {
        nl_ov_toast("请说：让它 + 要做的事");
        return true;
    }
    char ws[200];
    char sess[80];
    nodelio_panel_ui_get_dsh_arm(ws, sizeof(ws), sess, sizeof(sess));

    const char *sid = find_installed_app_id("dsh.task", false);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.task";
    }
    /* 未武装：仍发请求（Host 可注入 APP default_workspace）；先提示避免「不知道建在哪」 */
    if (!ws[0] && !sess[0]) {
        nl_ov_toast("未选工作区，将用 APP 默认");
        ESP_LOGW(TAG, "dsh.task no arm — Host will inject default_workspace; instr='%s'", esc);
    } else {
        ESP_LOGI(TAG, "dsh.task armed ws='%s' sess='%s' instr='%s'", ws, sess, esc);
    }
    /* esc≤179 + ws_esc≤219 + sess≤79 + JSON 外壳 ≈524；给足余量消 -Wformat-truncation */
    char body[544];
    /* 转义 path 反斜杠供 JSON */
    char ws_esc[220];
    size_t wi = 0;
    for (const char *p = ws; *p && wi + 2 < sizeof(ws_esc); ++p) {
        if (*p == '\\' || *p == '"') {
            ws_esc[wi++] = '\\';
        }
        ws_esc[wi++] = *p;
    }
    ws_esc[wi] = '\0';

    if (sess[0] && ws_esc[0]) {
        snprintf(body, sizeof(body),
                 "{\"instruction\":\"%s\",\"workspace\":\"%s\",\"session\":\"%s\"}", esc, ws_esc, sess);
    } else if (ws_esc[0]) {
        snprintf(body, sizeof(body), "{\"instruction\":\"%s\",\"workspace\":\"%s\"}", esc, ws_esc);
    } else if (sess[0]) {
        snprintf(body, sizeof(body), "{\"instruction\":\"%s\",\"session\":\"%s\"}", esc, sess);
    } else {
        snprintf(body, sizeof(body), "{\"instruction\":\"%s\"}", esc);
    }
    ESP_LOGI(TAG, "invoke dsh.task sid=%s body=%s", sid, body);
    /* 续聊/派活发出瞬间：进度卡切「已投递」动画（HTTP 回包后会被真实列表覆盖） */
    nodelio_skill_dsh_on_dispatched(esc);
    /* 保存工作区：task 回包后链式拉 status（arm 即将清空） */
    char chain_ws[200];
    snprintf(chain_ws, sizeof(chain_ws), "%s", ws);
    nl_ov_invoke_skill_async(sid, body);
    /* 链式进度：稍后再拉，避免与 task 回包竞态盖掉 */
    struct chain_arg {
        char ws[200];
    };
    auto *ca = static_cast<chain_arg *>(malloc(sizeof(chain_arg)));
    if (ca) {
        snprintf(ca->ws, sizeof(ca->ws), "%s", chain_ws);
        constexpr uint32_t kStack = 4096 / sizeof(StackType_t);
        if (xTaskCreateWithCaps(
                [](void *p) {
                    auto *a = static_cast<chain_arg *>(p);
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    nl_ov_apps_invoke_dsh_status(a->ws[0] ? a->ws : nullptr, nullptr);
                    free(a);
                    vTaskDeleteWithCaps(nullptr);
                },
                "nl_dsh_chain", kStack, ca, 2, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            free(ca);
            nl_ov_apps_invoke_dsh_status(chain_ws[0] ? chain_ws : nullptr, nullptr);
        }
    } else {
        nl_ov_apps_invoke_dsh_status(chain_ws[0] ? chain_ws : nullptr, nullptr);
    }
    nodelio_panel_ui_clear_dsh_arm();
    return true;
}

bool nl_ov_apps_open_by_query(const char *query)
{
    if (!query || !query[0]) {
        ESP_LOGW(TAG, "open_by_query: empty");
        return false;
    }
    ESP_LOGI(TAG, "open_by_query q='%s' installed=%d", query, s_item_n);

    /* 裸「派活」无 instruction → 只提示，避免 error 卡「缺少参数」 */
    if (strcmp(query, "派活") == 0 || strcmp(query, "DSH派活") == 0 ||
        strcmp(query, "dsh.task") == 0 || strcmp(query, "dsh_task") == 0) {
        return nl_ov_apps_invoke_dsh_task(nullptr);
    }

    /* DSH 意图优先于模糊名匹配：裸「DSH」曾误开「DSH 派活」→ 缺 instruction 报错 */
    const char *alias_id = nullptr;
    if (strstr(query, "派活") != nullptr || strstr(query, "dsh.task") != nullptr) {
        /* 「派活查一下…」少见；含派活且还有别的字 → 当进度？仍提示派活口令 */
        return nl_ov_apps_invoke_dsh_task(nullptr);
    } else if (strstr(query, "任务情况") != nullptr || strstr(query, "查看任务") != nullptr ||
               strstr(query, "任务列表") != nullptr || strstr(query, "任务详情") != nullptr ||
               strstr(query, "任务应用") != nullptr || strstr(query, "任务表情") != nullptr ||
               strstr(query, "dsh.status") != nullptr || strstr(query, "进度") != nullptr ||
               strstr(query, "DSH") != nullptr || strstr(query, "dsh") != nullptr ||
               (strstr(query, "任务") != nullptr && strstr(query, "派活") == nullptr)) {
        /* 含「打开DSH / 查看任务 / 任务应用」一律进度列表；派活须带「派活」 */
        alias_id = "dsh.status";
    } else if (strstr(query, "曲库") || strstr(query, "音乐") || strstr(query, "music") ||
               strstr(query, "听歌") || strstr(query, "放歌") || strstr(query, "爱音乐")) {
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

    const char *sid = nullptr;
    if (alias_id) {
        const bool dsh_browse = strcmp(alias_id, "dsh.status") == 0;
        sid = find_installed_app_id(alias_id, dsh_browse);
        if (!sid) {
            sid = alias_id;
        }
        ESP_LOGI(TAG, "open_by_query alias=%s → sid=%s", alias_id, sid ? sid : "(null)");
    } else {
        /* 单字/过短不 fuzzy：「电」会命中「网络电台」 */
        const size_t qn = strlen(query);
        int qchars = 0;
        for (const unsigned char *p = (const unsigned char *)query; *p; ) {
            if (*p < 0x80) {
                p++;
                qchars++;
            } else if ((*p & 0xE0) == 0xC0 && p[1]) {
                p += 2;
                qchars++;
            } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
                p += 3;
                qchars++;
            } else {
                p++;
                qchars++;
            }
        }
        if (qchars >= 2 && qn >= 4) {
            for (int i = 0; i < s_item_n; i++) {
                if (!s_items[i].enabled || skill_is_button_only(s_items[i].id)) {
                    continue;
                }
                if (strstr(s_items[i].name, query) || strstr(s_items[i].id, query) ||
                    strstr(query, s_items[i].name) || strstr(query, s_items[i].id)) {
                    sid = s_items[i].id;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "open_by_query fuzzy sid=%s qchars=%d", sid ? sid : "(none)", qchars);
    }
    if (!sid || !sid[0]) {
        ESP_LOGW(TAG, "open_by_query FAIL q='%s'", query);
        return false;
    }
    ESP_LOGI(TAG, "open_by_query invoke sid=%s", sid);
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

    s_hint = lv_label_create(s_page);
    nl_ov_set_font(s_hint);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, UI_TEXT_MUTED, 0);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, UI_GAP, 0);
    lv_obj_set_style_pad_column(s_list, UI_GAP, 0);
    lv_obj_set_style_pad_bottom(s_list, 56, 0); /* 避让右下刷新 FAB */
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* 右下角浮动刷新 */
    lv_obj_t *ref = lv_obj_create(s_page);
    lv_obj_remove_style_all(ref);
    lv_obj_set_size(ref, 48, 48);
    lv_obj_set_style_radius(ref, 24, 0);
    lv_obj_set_style_bg_color(ref, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(ref, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ref, 2, 0);
    lv_obj_set_style_border_color(ref, UI_ACCENT, 0);
    lv_obj_set_style_border_opa(ref, 200, 0);
    lv_obj_set_style_shadow_width(ref, 14, 0);
    lv_obj_set_style_shadow_opa(ref, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(ref, UI_ACCENT, 0);
    lv_obj_set_style_bg_color(ref, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(ref, 1, LV_STATE_PRESSED);
    lv_obj_add_flag(ref, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING |
                                         LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_set_ext_click_area(ref, 10);
    lv_obj_align(ref, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_add_event_cb(ref, [](lv_event_t *) { nl_ov_apps_refresh(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ric = lv_label_create(ref);
    lv_obj_set_style_text_font(ric, &font_material_symbols_20_4, 0);
    lv_label_set_text(ric, MATERIAL_SYMBOLS_REFRESH);
    lv_obj_set_style_text_color(ric, UI_ACCENT_SOFT, 0);
    lv_obj_center(ric);
    lv_obj_move_foreground(ref);
}
