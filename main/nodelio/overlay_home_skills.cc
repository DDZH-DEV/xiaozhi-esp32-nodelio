#include "overlay_priv.h"

#include "application.h"
#include "device_caps.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "panel_ui.h"
#include "skill_runtime.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

LV_FONT_DECLARE(font_material_symbols_20_4);

#define TAG "nl_home_sk"
#define HOME_SKILL_MAX 6
#define HOME_SKILL_STRIP_H 76
#define HOME_INVOKE_BODY_MAX 192

struct nl_home_skill {
    char id[48];
    char name[40];
    char body[HOME_INVOKE_BODY_MAX];
    bool enabled;
};

static nl_home_skill s_pins[HOME_SKILL_MAX];
static int s_pin_n;
static bool s_fetching;
static bool s_dirty = true;

int nl_ov_home_skill_strip_h(void)
{
    return s_pin_n > 0 ? HOME_SKILL_STRIP_H : 0;
}

int nl_ov_home_skill_pinned_count(void)
{
    return s_pin_n;
}

void nl_ov_home_skills_invalidate(void)
{
    s_dirty = true;
}

static const char *skill_icon(const char *id)
{
    if (id && (strcmp(id, "music.search") == 0 || strcmp(id, "music_search") == 0)) {
        return MATERIAL_SYMBOLS_MUSIC_NOTE;
    }
    return MATERIAL_SYMBOLS_EXPLORE;
}

static void copy_json_field(char *dst, size_t n, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    if (!obj || !dst || n == 0) {
        return;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, n, "%s", v->valuestring);
    }
}

static void store_default_invoke(char *dst, size_t n, const cJSON *it)
{
    if (!dst || n == 0) {
        return;
    }
    snprintf(dst, n, "{}");
    const cJSON *di = cJSON_GetObjectItemCaseSensitive(it, "defaultInvoke");
    if (!cJSON_IsObject(di)) {
        return;
    }
    char *printed = cJSON_PrintUnformatted(di);
    if (!printed) {
        return;
    }
    if (printed[0] == '{') {
        snprintf(dst, n, "%s", printed);
    }
    cJSON_free(printed);
}

static void invoke_home_skill_async(const char *skill_id, const char *body)
{
    if (!skill_id || !skill_id[0]) {
        return;
    }
    if (!s_cfg.server_url[0] || !s_cfg.device_id[0]) {
        nl_ov_toast("未绑定服务器");
        return;
    }

    struct invoke_arg {
        nodelio_nvs_config_t cfg;
        char id[48];
        char body[HOME_INVOKE_BODY_MAX];
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        nl_ov_toast("内存不足");
        return;
    }
    arg->cfg = s_cfg;
    snprintf(arg->id, sizeof(arg->id), "%s", skill_id);
    snprintf(arg->body, sizeof(arg->body), "%s", (body && body[0]) ? body : "{}");

    struct open_job {
        invoke_arg *arg;
        StackType_t *stack;
        StaticTask_t *tcb;
    };
    constexpr uint32_t kStackBytes = 16384;
    auto *job = static_cast<open_job *>(malloc(sizeof(open_job)));
    StackType_t *stack =
        static_cast<StackType_t *>(heap_caps_malloc(kStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stack) {
        stack = static_cast<StackType_t *>(malloc(kStackBytes));
    }
    auto *tcb =
        static_cast<StaticTask_t *>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!job || !stack || !tcb) {
        free(job);
        free(stack);
        free(tcb);
        free(arg);
        nl_ov_toast("无法打开");
        return;
    }
    job->arg = arg;
    job->stack = stack;
    job->tcb = tcb;
    nl_ov_toast("正在打开…");

    if (xTaskCreateStatic(
            [](void *p) {
                auto *job = static_cast<open_job *>(p);
                auto *a = job->arg;
                const bool music_app =
                    strcmp(a->id, "music.search") == 0 || strcmp(a->id, "music_search") == 0;
                if (!music_app && nodelio_media_preempt_for_heavy_app()) {
                    nodelio_panel_ui_toast_defer("已停止音乐以打开应用");
                }
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, a->cfg.device_id,
                         a->id);
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp =
                    static_cast<char *>(heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                if (!resp) {
                    nodelio_panel_ui_toast_defer("内存不足");
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err =
                        nodelio_http_post_json(url, a->cfg.device_key, a->body, &status, resp, cap - 1);
                    if (err == ESP_OK && (status == 200 || status == 201) && resp[0]) {
                        nodelio_skill_deliver_json(&a->cfg, resp, false, nullptr, 0);
                    } else {
                        nodelio_panel_ui_toast_defer("打开失败");
                    }
                    free(resp);
                }
                free(a);
                StackType_t *st = job->stack;
                StaticTask_t *tb = job->tcb;
                free(job);
                Application::GetInstance().Schedule([st, tb]() {
                    free(st);
                    free(tb);
                });
                vTaskDelete(nullptr);
            },
            "nl_home_sk", kStackBytes / sizeof(StackType_t), job, 3, stack, tcb) == nullptr) {
        free(job);
        free(stack);
        free(tcb);
        free(arg);
        nl_ov_toast("无法打开");
    }
}

static void on_pin_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_pin_n) {
        return;
    }
    if (!s_pins[idx].enabled) {
        nl_ov_toast("本机暂不支持");
        return;
    }
    invoke_home_skill_async(s_pins[idx].id, s_pins[idx].body);
}

static lv_obj_t *make_pin_card(lv_obj_t *parent, int idx, bool compact)
{
    if (idx < 0 || idx >= s_pin_n || !parent) {
        return nullptr;
    }
    const nl_home_skill *it = &s_pins[idx];

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    if (compact) {
        lv_obj_set_size(card, 132, 64);
    } else {
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, 72);
    }
    lv_obj_set_style_bg_color(card, UI_TILE_OFF, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(card, compact ? 8 : 12, 0);
    lv_obj_set_style_pad_column(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(card, 1, LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, on_pin_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *pod = lv_obj_create(card);
    lv_obj_remove_style_all(pod);
    lv_obj_set_size(pod, compact ? 36 : 42, compact ? 36 : 42);
    lv_obj_set_style_radius(pod, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(pod, UI_CAPSULE_SCENE, 0);
    lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pod, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *ic = lv_label_create(pod);
    lv_label_set_text(ic, skill_icon(it->id));
    lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
    lv_obj_center(ic);

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *tit = lv_label_create(col);
    nl_ov_set_font(tit);
    lv_label_set_text(tit, it->name[0] ? it->name : it->id);
    lv_obj_set_style_text_color(tit, UI_TEXT, 0);
    lv_label_set_long_mode(tit, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tit, lv_pct(100));

    lv_obj_t *sub = lv_label_create(col);
    nl_ov_set_font(sub);
    lv_label_set_text(sub, it->enabled ? "应用 · 点按打开" : "能力不足");
    lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, lv_pct(100));

    /* 右上微标：区分控家通道 */
    lv_obj_t *badge = lv_label_create(card);
    nl_ov_set_font(badge);
    lv_label_set_text(badge, "应用");
    lv_obj_set_style_text_color(badge, UI_PURPLE, 0);
    lv_obj_add_flag(badge, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -4, 4);

    return card;
}

void nl_ov_home_skills_append_launcher(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    for (int i = 0; i < s_pin_n; i++) {
        make_pin_card(parent, i, false);
    }
}

void nl_ov_home_skills_append_strip(lv_obj_t *parent)
{
    if (!parent || s_pin_n <= 0) {
        return;
    }
    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, lv_pct(100), HOME_SKILL_STRIP_H);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_hor(strip, 4, 0);
    lv_obj_set_style_pad_column(strip, 8, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < s_pin_n; i++) {
        make_pin_card(strip, i, true);
    }
}

static void apply_pins_and_refill(nl_home_skill *items, int n)
{
    Application::GetInstance().Schedule([items, n]() {
        DisplayLockGuard lock(nl_ov_disp());
        s_fetching = false;
        s_dirty = false;
        if (!lock) {
            free(items);
            return;
        }
        s_pin_n = n > HOME_SKILL_MAX ? HOME_SKILL_MAX : n;
        if (s_pin_n > 0 && items) {
            memcpy(s_pins, items, sizeof(nl_home_skill) * (size_t)s_pin_n);
        } else {
            s_pin_n = 0;
        }
        free(items);
        if (s_active_tab == NL_TAB_HOME && s_home) {
            nl_ov_fill_home();
        }
    });
}

void nl_ov_home_skills_ensure_fetched(void)
{
    if (s_fetching || !s_dirty) {
        return;
    }
    if (!s_cfg.server_url[0] || !s_cfg.device_id[0] || !s_cfg.device_key[0]) {
        s_dirty = false;
        s_pin_n = 0;
        return;
    }
    s_fetching = true;
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        s_fetching = false;
        return;
    }
    *cfgp = s_cfg;
    if (xTaskCreate(
            [](void *arg) {
                auto *cfg = static_cast<nodelio_nvs_config_t *>(arg);
                auto *items = static_cast<nl_home_skill *>(calloc(HOME_SKILL_MAX, sizeof(nl_home_skill)));
                int n = 0;
                if (!items) {
                    free(cfg);
                    Application::GetInstance().Schedule([]() { s_fetching = false; });
                    vTaskDelete(nullptr);
                    return;
                }
                char base[160];
                nl_url_strip_base(cfg->server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);
                char *buf = static_cast<char *>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!buf) {
                    buf = static_cast<char *>(malloc(4096));
                }
                if (!buf) {
                    free(items);
                    free(cfg);
                    Application::GetInstance().Schedule([]() { s_fetching = false; });
                    vTaskDelete(nullptr);
                    return;
                }
                buf[0] = '\0';
                int status = 0;
                esp_err_t err = nodelio_http_get(url, cfg->device_key, nullptr, &status, buf, 4095);
                if (err == ESP_OK && (status == 200 || status == 201)) {
                    cJSON *root = cJSON_Parse(buf);
                    const cJSON *skills = root ? cJSON_GetObjectItemCaseSensitive(root, "skills") : nullptr;
                    if (cJSON_IsArray(skills)) {
                        const cJSON *it = nullptr;
                        cJSON_ArrayForEach(it, skills) {
                            if (n >= HOME_SKILL_MAX) {
                                break;
                            }
                            const cJSON *pin = cJSON_GetObjectItemCaseSensitive(it, "pinned");
                            if (!cJSON_IsTrue(pin)) {
                                continue;
                            }
                            nl_home_skill *row = &items[n];
                            copy_json_field(row->id, sizeof(row->id), it, "skillId");
                            if (!row->id[0]) {
                                continue;
                            }
                            copy_json_field(row->name, sizeof(row->name), it, "name");
                            store_default_invoke(row->body, sizeof(row->body), it);
                            const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
                            row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
                            n++;
                        }
                    }
                    if (root) {
                        cJSON_Delete(root);
                    }
                } else {
                    ESP_LOGW(TAG, "fetch pins status=%d err=%d", status, err);
                }
                free(buf);
                free(cfg);
                apply_pins_and_refill(items, n);
                vTaskDelete(nullptr);
            },
            "nl_home_pins", 10240, cfgp, 3, nullptr) != pdPASS) {
        s_fetching = false;
        free(cfgp);
    }
}
