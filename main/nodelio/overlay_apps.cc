#include "overlay_priv.h"

#include "application.h"
#include "board_info.h"
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
LV_FONT_DECLARE(font_material_symbols_30_4);

#define TAG "nl_apps"
#define APP_MAX 17 /* 16 cloud entries plus the built-in daily view. */
/* 8 个样例应用带 inputSchema 时整包 >7KB；旧 4KB 截断后 cJSON 失败 →「暂无已安装」。 */
#define APP_LIST_BUF 24576

struct nl_app_item {
    char id[72];
    char name[40];
    char desc[72];
    bool enabled;
    bool outbound_bridge; /* invoke.transport=outbound_bridge */
};

struct nl_app_snap {
    nl_app_item items[APP_MAX];
    int n;
    char err[48];
};

static lv_obj_t *s_page;
static lv_obj_t *s_list_pane; /* 宽屏可收成左侧轨，给技能卡让位 */
static lv_obj_t *s_list;
static lv_obj_t *s_hint;
static lv_obj_t *s_refresh;
static nl_app_item s_items[APP_MAX];

static void with_builtin_daily(nl_app_item* items, int& n) {
    int keep = 0;
    for (int i = 0; i < n; ++i) {
        const char* bare = strstr(items[i].id, "::");
        bare = bare ? bare + 2 : items[i].id;
        if (strcmp(bare, "builtin.daily") == 0 || strcmp(bare, "weather.today") == 0 ||
            strcmp(bare, "air.now") == 0)
            continue;
        items[keep++] = items[i];
    }
    if (keep >= APP_MAX)
        keep = APP_MAX - 1;
    memmove(items + 1, items, sizeof(*items) * (size_t)keep);
    items[0] = {};
    snprintf(items[0].id, sizeof(items[0].id), "builtin.daily");
    snprintf(items[0].name, sizeof(items[0].name), "日历与天气");
    snprintf(items[0].desc, sizeof(items[0].desc), "内置 · 月历 / 15 天天气 / 空气质量");
    items[0].enabled = true;
    n = keep + 1;
}

/** 仅供进度卡「允许/拒绝」按钮 invoke，不进应用列表 / 语音打开。 */
static bool skill_is_button_only(const char *id)
{
    return id && strstr(id, "dsh.answer") != nullptr;
}
static int s_item_n;
static bool s_loading;
static bool s_daily_loading;
static bool s_have_list;
static bool s_split;
static int s_focus_idx = -1;

static constexpr int kAppsPagePadH = 16;
static constexpr int kAppsAnimMs = 180;
static constexpr int kAppsBrowseTileH = 84;
static constexpr int kAppsRailTileH = 84;

static bool apps_wide(void)
{
    return nodelio_display_profile()->nav_width != 0;
}

static int apps_body_w(void)
{
    const auto *p = nodelio_display_profile();
    return (int)p->width - (int)p->nav_width;
}

static int apps_pane_target_w(bool split)
{
    const int full = apps_body_w() - 2 * kAppsPagePadH;
    if (apps_wide() && split) {
        return apps_body_w() - nodelio_display_profile()->result_width - kAppsPagePadH - 2 * UI_PAD;
    }
    return full > 120 ? full : 120;
}

static void apps_exec_pane_w(void *obj, int32_t v)
{
    lv_obj_set_width(static_cast<lv_obj_t *>(obj), v);
}

static void apps_set_split(bool on, bool animate);
static void render_list(void);

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
        if (items[i].enabled != s_items[i].enabled ||
            items[i].outbound_bridge != s_items[i].outbound_bridge) {
            return false;
        }
        if (strcmp(items[i].id, s_items[i].id) != 0 || strcmp(items[i].name, s_items[i].name) != 0 ||
            strcmp(items[i].desc, s_items[i].desc) != 0) {
            return false;
        }
    }
    return true;
}

static void apps_split_failed(void)
{
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock)
            nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
    });
}

static void open_app_at(int idx, bool reset_daily = true) {
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
    s_focus_idx = idx;
    const bool daily = strcmp(s_items[idx].id, "builtin.daily") == 0;
    if (daily && s_daily_loading) {
        nl_ov_toast("天气正在更新，请稍候");
        return;
    }
    const uint32_t daily_generation = daily ? nodelio_skill_daily_open(reset_daily) : 0;
    if (daily && !daily_generation)
        return;
    if (apps_wide()) {
        if (s_split) {
            render_list();
        } else {
            apps_set_split(true, true);
        }
    }
    if (daily && !nodelio_wifi_is_connected()) {
        nl_ov_toast("当前离线，日历可用；联网后刷新天气");
        return;
    }
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        nl_ov_toast("内存不足");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    if (!nl_ov_cfg_http_ready(cfgp, true)) {
        free(cfgp);
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    struct invoke_arg {
        nodelio_nvs_config_t *cfg;
        char id[72];
        uint32_t daily_generation;
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        free(cfgp);
        nl_ov_toast("内存不足");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    arg->cfg = cfgp;
    arg->daily_generation = daily_generation;
    snprintf(arg->id, sizeof(arg->id), "%s", s_items[idx].id);
    nl_ov_toast("正在打开…");
    if (daily)
        s_daily_loading = true;
    constexpr uint32_t kStackWords = 16384 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<invoke_arg *>(p);
                const bool music_app = nodelio_skill_id_is_music(a->id);
                if (music_app) {
                    Application::GetInstance().Schedule([]() {
                        auto st = Application::GetInstance().GetDeviceState();
                        if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                            st == kDeviceStateConnecting) {
                            Application::GetInstance().EndVoiceSessionAfterLocalSkill();
                        }
                    });
                } else if (!a->daily_generation && nodelio_media_preempt_for_heavy_app()) {
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
                bool ok = false;
                if (!resp) {
                    nodelio_panel_ui_toast_defer("内存不足");
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err = nodelio_http_post_json_ms(
                        url, a->cfg->device_key, "{\"keyword\":\"\"}", &status, resp, cap - 1,
                        a->daily_generation ? 16000 : 10000);
                    if (err == ESP_OK && resp[0] &&
                        (status == 200 || status == 201 ||
                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                        ESP_LOGI(TAG, "deliver id=%s music=%d status=%d", a->id, music_app ? 1 : 0,
                                 status);
                        if (a->daily_generation) {
                            auto* env = static_cast<nl_content_envelope_t*>(
                                heap_caps_calloc(1, sizeof(nl_content_envelope_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                            if (env && nodelio_envelope_parse(resp, env) &&
                                strcmp(env->card_type, "calendar_weather") == 0) {
                                const uint32_t generation = a->daily_generation;
                                const auto cfg = *a->cfg;
                                Application::GetInstance().Schedule([env, generation, cfg]() {
                                    nodelio_skill_daily_update(&cfg, env, generation);
                                });
                                ok = true;
                            } else {
                                if (env)
                                    free(env->detail_body);
                                free(env);
                            }
                        } else {
                            char spoken[NL_ENV_SPOKEN];
                            spoken[0] = '\0';
                            nodelio_skill_deliver_json(a->cfg, resp, false, spoken, sizeof(spoken));
                            if (strstr(a->id, "dsh") != nullptr && spoken[0]) {
                                const char* au = nodelio_skill_last_spoken_audio_url();
                                nodelio_panel_ui_announce_spoken_ex(spoken, au);
                            }
                            ok = true;
                        }
                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                        nl_ov_toast_invoke_unreachable(a->id);
                    } else {
                        nodelio_panel_ui_toast_defer("打开失败");
                    }
                    free(resp);
                }
                if (!ok && !a->daily_generation) {
                    apps_split_failed();
                } else if (!ok) {
                    nodelio_panel_ui_toast_defer("天气未更新，日历仍可使用");
                }
                if (a->daily_generation) {
                    Application::GetInstance().Schedule([]() { s_daily_loading = false; });
                }
                free(a->cfg);
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_app_open", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        free(arg->cfg);
        free(arg);
        if (daily)
            s_daily_loading = false;
        nl_ov_toast("无法打开");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
    }
}

void nl_ov_apps_refresh_daily(void) {
    for (int i = 0; i < s_item_n; ++i) {
        if (strcmp(s_items[i].id, "builtin.daily") == 0) {
            open_app_at(i, false);
            return;
        }
    }
}

static const char *apps_glyph_for(const char *id)
{
    if (id && (strstr(id, "radio.stations") != nullptr || strstr(id, "podcast.latest") != nullptr)) {
        return MATERIAL_SYMBOLS_VOLUME_UP;
    }
    if (id && nodelio_skill_id_is_music(id)) {
        return MATERIAL_SYMBOLS_MUSIC_NOTE;
    }
    return MATERIAL_SYMBOLS_EXPLORE;
}

static void render_list(void)
{
    if (!s_list) {
        return;
    }
    const lv_coord_t keep_y = lv_obj_get_scroll_y(s_list);
    lv_obj_clean(s_list);
    if (s_item_n <= 0) {
        return;
    }
    /* 分栏仍保留两列及描述；右窗宽度来自当前板的 display_profile。 */
    const int list_w = apps_pane_target_w(s_split);
    const int gap = UI_GAP;
    int cols = 2;
    if (apps_wide() && !s_split) {
        cols = 3;
    }
    int card_w = (list_w - gap * (cols - 1)) / cols;
    if (card_w < 160) {
        card_w = list_w;
        cols = 1;
    }
    const int text_line = nl_ov_ui_line_box();
    /* 标题 1 行 + 描述 2 行，与设置页同一套行框，大字不再被卡片裁切。 */
    int card_h = 12 + text_line + 4 + text_line * 2 + 12;
    const int floor_h = s_split ? kAppsRailTileH : kAppsBrowseTileH;
    if (card_h < floor_h) {
        card_h = floor_h;
    }
    const int pod_sz = s_split ? 44 : 48;

    for (int i = 0; i < s_item_n; i++) {
        const nl_app_item *it = &s_items[i];
        const bool focused = s_split && i == s_focus_idx;
        lv_obj_t *card = lv_obj_create(s_list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_style_bg_color(card, focused ? UI_TILE_SWITCH : UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
        lv_obj_set_style_border_width(card, focused ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, focused ? UI_ACCENT : UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(card, focused ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_pad_hor(card, 12, 0);
        lv_obj_set_style_pad_column(card, 16, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        if (it->enabled) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(card, 8);
            lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(card, 1, LV_STATE_PRESSED);
            lv_obj_add_event_cb(
                card,
                [](lv_event_t *e) {
                    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                        return;
                    }
                    open_app_at((int)(intptr_t)lv_event_get_user_data(e));
                },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        lv_obj_t *pod = lv_obj_create(card);
        lv_obj_remove_style_all(pod);
        lv_obj_set_size(pod, pod_sz, pod_sz);
        lv_obj_set_style_radius(pod, 12, 0);
        lv_obj_set_style_bg_color(pod, it->enabled ? UI_CAPSULE_ON : UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_opa(pod, it->enabled ? LV_OPA_40 : LV_OPA_COVER, 0);
        lv_obj_clear_flag(pod, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = lv_label_create(pod);
        lv_obj_set_style_text_font(ic, &font_material_symbols_30_4, 0);
        lv_label_set_text(ic, apps_glyph_for(it->id));
        lv_obj_set_style_text_color(ic, it->enabled ? UI_ACCENT_SOFT : UI_TEXT_MUTED, 0);
        lv_obj_center(ic);

        lv_obj_t *txt = lv_obj_create(card);
        lv_obj_remove_style_all(txt);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(txt, 2, 0);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);

        const int text_w = card_w - 12 * 2 - 16 - pod_sz;
        const char *desc = "点按打开";
        if (!it->enabled) {
            desc = "不可用";
        } else if (it->desc[0]) {
            desc = it->desc;
        }

        lv_obj_t *title = lv_label_create(txt);
        nl_ov_set_font(title);
        lv_label_set_text(title, it->name[0] ? it->name : it->id);
        lv_obj_set_style_text_color(title, it->enabled ? UI_TEXT : UI_TEXT_MUTED, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_width(title, lv_pct(100));
        lv_obj_set_height(title, nl_ov_ui_font_line_h());

        {
            lv_obj_t *sub = lv_label_create(txt);
            nl_ov_set_font(sub);
            lv_label_set_text(sub, desc);
            lv_obj_set_style_text_color(sub, it->enabled ? UI_TEXT_DIM : UI_TEXT_MUTED, 0);
            lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
            lv_obj_set_width(sub, lv_pct(100));
            lv_obj_set_height(sub, nl_ov_wrapped_text_h(desc, text_w));
        }
    }
    if (keep_y > 0) {
        lv_obj_update_layout(s_list);
        lv_obj_scroll_to_y(s_list, keep_y, LV_ANIM_OFF);
    }
}

static void apps_split_anim_ready(lv_anim_t *a)
{
    (void)a;
    render_list();
    nodelio_skill_result_layout();
    nl_ov_apps_layout_chrome();
}

static void apps_set_split(bool on, bool animate)
{
    if (!apps_wide()) {
        on = false;
    }
    if (!s_list_pane) {
        s_split = on;
        return;
    }
    if (s_split == on) {
        return;
    }
    s_split = on;
    if (!on) {
        s_focus_idx = -1;
    }
    const int to_w = apps_pane_target_w(on);
    lv_anim_delete(s_list_pane, apps_exec_pane_w);
    if (!animate) {
        lv_obj_set_width(s_list_pane, to_w);
        render_list();
        nodelio_skill_result_layout();
        nl_ov_apps_layout_chrome();
        return;
    }
    /* 先只滑轨宽（旧卡 flex 折行），完成后再 clean 重排——开卡瞬间勿双开重布局 */
    int from_w = (int)lv_obj_get_width(s_list_pane);
    if (from_w < 80) {
        from_w = apps_pane_target_w(!on);
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_list_pane);
    lv_anim_set_values(&a, from_w, to_w);
    lv_anim_set_time(&a, kAppsAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, apps_exec_pane_w);
    lv_anim_set_completed_cb(&a, apps_split_anim_ready);
    lv_anim_start(&a);
    nodelio_skill_result_layout();
    nl_ov_apps_layout_chrome();
}

void nl_ov_apps_relayout(void)
{
    if (!s_list) {
        return;
    }
    if (s_hint) {
        nl_ov_set_font(s_hint);
    }
    render_list();
    nl_ov_apps_layout_chrome();
}

int nl_ov_apps_split_rail_w(void)
{
    if (!s_split || !apps_wide()) {
        return 0;
    }
    return apps_pane_target_w(true);
}

void nl_ov_apps_notify_skill_open(bool open) {
    /* 已分栏时技能卡静默刷新勿重摆列表 */
    if (open && s_split) {
        return;
    }
    apps_set_split(open, true);
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
        const cJSON *ob = cJSON_GetObjectItemCaseSensitive(it, "ob");
        tmp[n].outbound_bridge = cJSON_IsTrue(ob);
        n++;
    }
    cJSON_Delete(root);
    with_builtin_daily(tmp, n);
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
        if (s_items[i].outbound_bridge) {
            cJSON_AddBoolToObject(o, "ob", true);
        }
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
                    with_builtin_daily(s_items, s_item_n);
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
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(it, "invokeTransport");
            row->outbound_bridge =
                cJSON_IsString(tr) && tr->valuestring &&
                strcmp(tr->valuestring, "outbound_bridge") == 0;
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
                const cJSON *tr = cJSON_GetObjectItemCaseSensitive(it, "invoke_transport");
                row->outbound_bridge =
                    cJSON_IsString(tr) && tr->valuestring &&
                    strcmp(tr->valuestring, "outbound_bridge") == 0;
                snap->n++;
            }
        }
    }
    with_builtin_daily(snap->items, snap->n);
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
    nodelio_nvs_config_t http_cfg{};
    nl_ov_cfg_for_http(&http_cfg);
    if (!http_cfg.server_url[0] || !http_cfg.device_id[0]) {
        s_item_n = 0;
        with_builtin_daily(s_items, s_item_n);
        set_hint(!http_cfg.server_url[0] ? "未绑定服务器" : "设备未注册");
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
    *cfgp = http_cfg;
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
                /* nl_http 已 grace+空收重试；外层再隔开 1 次兜底（新设备首开） */
                static const uint32_t k_retry_ms[] = {0, 1200};
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
                    err = nodelio_http_get_ms(url, cfg->device_key, nullptr, &status, buf, APP_LIST_BUF - 1, 8000);
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

void nl_ov_apps_layout_chrome(void)
{
    if (!s_refresh) {
        return;
    }
    /* s_page 已被 workspace_reserve 收成左工作区，贴页右下即可；勿再减 result 宽（会漂到屏中）。 */
    constexpr int kEdge = 14;
    constexpr int kPagePadH = 16;
    constexpr int kPagePadV = 8;
    constexpr int kFabSz = 52; /* 与 SKILL_MINI_SZ 一致 */
    constexpr int kStackGap = 10;
    const int ox = kPagePadH - kEdge; /* ≈2，抵消页 pad 后视觉右边距 14 */
    const int oy_base = -(kEdge - kPagePadV);
    if (nodelio_skill_mini_fab_visible()) {
        lv_obj_align(s_refresh, LV_ALIGN_BOTTOM_RIGHT, ox, oy_base - kFabSz - kStackGap);
    } else {
        lv_obj_align(s_refresh, LV_ALIGN_BOTTOM_RIGHT, ox, oy_base);
    }
}

void nl_ov_apps_set_visible(bool on)
{
    if (!s_page) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
        nl_ov_apps_layout_chrome();
        if (s_refresh) {
            lv_obj_move_foreground(s_refresh);
        }
    } else {
        lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    }
}

void nl_ov_apps_reserve_bottom(int bottom) {
    if (s_page && apps_wide()) {
        lv_obj_set_height(s_page, nodelio_display_profile()->height - NL_UI_STATUS_H - bottom);
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

/* cJSON 保留指令、路径和会话 ID 中的引号/反斜杠；队列拒绝超长 JSON，绝不截断。 */
static bool invoke_dsh(const char* skill, const char* instruction, const char* workspace,
                       const char* session) {
    cJSON* params = cJSON_CreateObject();
    if (!params) {
        nl_ov_toast("内存不足，未发送");
        return false;
    }
    const bool valid =
        (!instruction || cJSON_AddStringToObject(params, "instruction", instruction)) &&
        (!workspace || !workspace[0] || cJSON_AddStringToObject(params, "workspace", workspace)) &&
        (!session || !session[0] || cJSON_AddStringToObject(params, "session", session));
    char* body = valid ? cJSON_PrintUnformatted(params) : nullptr;
    cJSON_Delete(params);
    if (!body) {
        nl_ov_toast("内存不足，未发送");
        return false;
    }
    const bool queued = nl_ov_invoke_skill_async(skill, body);
    cJSON_free(body);
    return queued;
}

void nl_ov_apps_invoke_dsh_status(const char *workspace, const char *session)
{
    const char *sid = find_installed_app_id("dsh.status", true);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.status";
    }
    (void)invoke_dsh(sid, nullptr, workspace, session);
}

bool nl_ov_apps_invoke_dsh_task(const char* instruction, const char* workspace,
                                const char* session) {
    if (!instruction || !instruction[0]) {
        nodelio_panel_ui_announce_spoken("请说让它，再加上要做的事。");
        return true;
    }
    const char *sid = find_installed_app_id("dsh.task", false);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.task";
    }
    if (!invoke_dsh(sid, instruction, workspace, session)) {
        return false;
    }
    nodelio_skill_dsh_on_dispatched(instruction);
    /* dsh.task 已返回含目标会话的进度列表。定时补拉会与本次响应竞态，覆盖真实结果。 */
    return true;
}

bool nl_ov_apps_open_by_query(const char *query)
{
    if (!query || !query[0]) {
        ESP_LOGW(TAG, "open_by_query: empty");
        return false;
    }
    ESP_LOGI(TAG, "open_by_query q='%s' installed=%d", query, s_item_n);

    /* 「打开面板曲库」→ 口语「面板」前缀；「取库」≈「曲库」ASR */
    char qbuf[64];
    snprintf(qbuf, sizeof(qbuf), "%s", query);
    if (strncmp(qbuf, "面板", 6) == 0 && qbuf[6] != '\0') {
        memmove(qbuf, qbuf + 6, strlen(qbuf + 6) + 1);
    }
    if (strcmp(qbuf, "取库") == 0 || strcmp(qbuf, "去库") == 0 || strcmp(qbuf, "趣库") == 0) {
        snprintf(qbuf, sizeof(qbuf), "%s", "曲库");
    }
    if (strcmp(qbuf, "应用") == 0 || strcmp(qbuf, "面板应用") == 0) {
        ESP_LOGI(TAG, "open_by_query: apps page intent q='%s'", query);
        nl_ov_show_page(NL_TAB_APPS);
        nl_ov_toast("应用");
        return true;
    }
    query = qbuf;

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
    } else if (strstr(query, "曲库") || strstr(query, "取库") || strstr(query, "去库") ||
               strstr(query, "趣库") || strstr(query, "音乐") || strstr(query, "music") ||
               strstr(query, "听歌") || strstr(query, "放歌") || strstr(query, "爱音乐")) {
        alias_id = "music.search";
    } else if (strstr(query, "RSS订阅") || strstr(query, "rss订阅") || strstr(query, "RSS 订阅") ||
               strstr(query, "新闻") || strstr(query, "news")) {
        alias_id = "news.latest";
    } else if (strstr(query, "天气") || strstr(query, "weather") || strstr(query, "日历") ||
               strstr(query, "calendar")) {
        alias_id = "builtin.daily";
    } else if (strstr(query, "电台") || strstr(query, "radio")) {
        alias_id = "radio.stations";
    } else if (strstr(query, "播客") || strstr(query, "podcast")) {
        alias_id = "podcast.latest";
    } else if (strstr(query, "空气") || strstr(query, "air")) {
        alias_id = "builtin.daily";
    } else if (strstr(query, "一篇") || strstr(query, "美文") || strstr(query, "article") ||
               strstr(query, "每日一文")) {
        alias_id = "article.daily";
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
    if (strcmp(sid, "builtin.daily") == 0) {
        for (int i = 0; i < s_item_n; ++i) {
            if (strcmp(s_items[i].id, sid) == 0) {
                open_app_at(i);
                return true;
            }
        }
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
    lv_obj_set_style_pad_hor(s_page, kAppsPagePadH, 0);
    lv_obj_set_style_pad_ver(s_page, 8, 0);
    lv_obj_set_style_pad_row(s_page, 4, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    load_disk_cache();
    with_builtin_daily(s_items, s_item_n);

    s_list_pane = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list_pane);
    lv_obj_set_width(s_list_pane, apps_pane_target_w(false));
    lv_obj_set_flex_grow(s_list_pane, 1);
    lv_obj_set_flex_flow(s_list_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_pane, 4, 0);
    lv_obj_clear_flag(s_list_pane, LV_OBJ_FLAG_SCROLLABLE);

    s_hint = lv_label_create(s_list_pane);
    nl_ov_set_font(s_hint);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, UI_TEXT_MUTED, 0);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_obj_create(s_list_pane);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_list, UI_GAP, 0);
    lv_obj_set_style_pad_column(s_list, UI_GAP, 0);
    lv_obj_set_style_pad_bottom(s_list, 120, 0); /* 避让右下刷新+音乐球叠放 */
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    render_list();

    /* 右下角浮动刷新（有音乐球时叠在上方） */
    s_refresh = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_refresh);
    lv_obj_set_size(s_refresh, 52, 52);
    lv_obj_set_style_radius(s_refresh, 26, 0);
    lv_obj_set_style_bg_color(s_refresh, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_refresh, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_refresh, 2, 0);
    lv_obj_set_style_border_color(s_refresh, UI_ACCENT, 0);
    lv_obj_set_style_border_opa(s_refresh, 200, 0);
    lv_obj_set_style_shadow_width(s_refresh, 14, 0);
    lv_obj_set_style_shadow_opa(s_refresh, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_refresh, UI_ACCENT, 0);
    lv_obj_set_style_bg_color(s_refresh, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(s_refresh, 1, LV_STATE_PRESSED);
    lv_obj_add_flag(s_refresh, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING |
                                               LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_set_ext_click_area(s_refresh, 10);
    lv_obj_add_event_cb(s_refresh, [](lv_event_t *) { nl_ov_apps_refresh(); }, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t *ric = lv_label_create(s_refresh);
    lv_obj_set_style_text_font(ric, &font_material_symbols_20_4, 0);
    lv_label_set_text(ric, MATERIAL_SYMBOLS_REFRESH);
    lv_obj_set_style_text_color(ric, UI_ACCENT_SOFT, 0);
    lv_obj_center(ric);
    nl_ov_apps_layout_chrome();
    lv_obj_move_foreground(s_refresh);
}

bool nl_ov_skill_needs_outbound_bridge(const char *skill_id)
{
    if (!skill_id || !skill_id[0] || !s_have_list) {
        return false;
    }
    for (int i = 0; i < s_item_n; i++) {
        if (!s_items[i].id[0]) {
            continue;
        }
        if (strcmp(s_items[i].id, skill_id) == 0) {
            return s_items[i].outbound_bridge;
        }
        /* market::app 与裸 app_id 均可匹配 */
        const char *p = strstr(s_items[i].id, "::");
        const char *bare = p ? p + 2 : s_items[i].id;
        if (strcmp(bare, skill_id) == 0 || strstr(skill_id, bare) != nullptr) {
            return s_items[i].outbound_bridge;
        }
        if (strstr(s_items[i].id, skill_id) != nullptr) {
            return s_items[i].outbound_bridge;
        }
    }
    return false;
}

void nl_ov_toast_invoke_unreachable(const char *skill_id)
{
    if (nl_ov_skill_needs_outbound_bridge(skill_id)) {
        nodelio_panel_ui_toast_defer("本地桥未连接");
    } else {
        nodelio_panel_ui_toast_defer("服务器无响应");
    }
}
