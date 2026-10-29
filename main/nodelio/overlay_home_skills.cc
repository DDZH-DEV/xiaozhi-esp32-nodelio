#include "overlay_priv.h"

#include "application.h"
#include "device_caps.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "panel_ui.h"
#include "content_envelope.h"
#include "skill_runtime.h"
#include <esp_timer.h>
#include "wifi_provision.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

LV_FONT_DECLARE(font_material_symbols_20_4);

#define TAG "nl_home_sk"
#define HOME_SKILL_MAX 6
#define HOME_SKILL_STRIP_H 76
/* DSH：instruction + workspace + session；原 192 会截断路径 */
/* dsh.task/status：ws_esc≈220 + sess_esc≈160 + JSON 外壳 ≥408；对齐 overlay_apps body[544] */
#define HOME_INVOKE_BODY_MAX 544

struct nl_home_skill {
    char id[72];
    char name[40];
    char body[HOME_INVOKE_BODY_MAX];
    bool enabled;
};

static nl_home_skill s_pins[HOME_SKILL_MAX];
static int s_pin_n;
static bool s_fetching;
static bool s_dirty = true;

static int home_skill_strip_visible_n(void)
{
    int n = 0;
    for (int i = 0; i < s_pin_n; i++) {
        /* P3a：当前屏已挂同 skill_tile 则底条跳过，避免双入口 */
        if (nl_ov_layout_current_page_has_skill(s_pins[i].id)) {
            continue;
        }
        n++;
    }
    return n;
}

int nl_ov_home_skill_strip_h(void)
{
    return home_skill_strip_visible_n() > 0 ? HOME_SKILL_STRIP_H : 0;
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
    if (!id || !id[0]) {
        return MATERIAL_SYMBOLS_EXPLORE;
    }
    if (strstr(id, "radio.stations") != NULL || strstr(id, "podcast.latest") != NULL) {
        return MATERIAL_SYMBOLS_VOLUME_UP;
    }
    if (nodelio_skill_id_is_music(id)) {
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

bool nl_ov_invoke_skill_async(const char* skill_id, const char* body) {
    if (!skill_id || !skill_id[0]) {
        return false;
    }
    if (strlen(skill_id) >= 72 || (body && strlen(body) >= HOME_INVOKE_BODY_MAX)) {
        nl_ov_toast("内容过长，未发送，请缩短后重说");
        return false;
    }
    if (!s_cfg.server_url[0] || !s_cfg.device_id[0]) {
        nl_ov_toast("未绑定服务器");
        return false;
    }

    struct invoke_arg {
        nodelio_nvs_config_t cfg;
        char id[72];
        char body[HOME_INVOKE_BODY_MAX];
    };
    auto *arg = static_cast<invoke_arg *>(
        heap_caps_malloc(sizeof(invoke_arg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!arg) {
        arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    }
    if (!arg) {
        nl_ov_toast("内存不足");
        return false;
    }
    arg->cfg = s_cfg;
    snprintf(arg->id, sizeof(arg->id), "%s", skill_id);
    snprintf(arg->body, sizeof(arg->body), "%s", (body && body[0]) ? body : "{}");
    nl_ov_toast("正在打开…");

    constexpr uint32_t kStackWords = 16384 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<invoke_arg *>(p);
                const bool music_app = nodelio_skill_id_is_music(a->id);
                const bool dsh_app = strstr(a->id, "dsh") != nullptr;
                const bool dsh_mutation = strstr(a->id, "dsh.task") || strstr(a->id, "dsh.answer");
                /* 仅媒体退会话；天气等保留云端 TTS */
                if (music_app) {
                    Application::GetInstance().Schedule([]() {
                        auto st = Application::GetInstance().GetDeviceState();
                        if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                            st == kDeviceStateConnecting) {
                            Application::GetInstance().EndVoiceSessionAfterLocalSkill();
                        }
                    });
                } else if (nodelio_media_preempt_for_heavy_app()) {
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
                    /* DSH 桥可能等待 20s；HTTP 层对派活/审批禁自动重试。 */
                    esp_err_t err =
                        nodelio_http_post_json_ms(url, a->cfg.device_key, a->body, &status, resp,
                                                  cap - 1, dsh_app ? 22000 : 10000);
                    if (err == ESP_OK && resp[0] &&
                        (status == 200 || status == 201 ||
                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                        /* 是否开播看 Envelope auto_play（Host 技能配置）；error 也上屏+toast */
                        ESP_LOGI(TAG, "deliver id=%s music=%d status=%d", a->id, music_app ? 1 : 0,
                                 status);
                        char spoken[NL_ENV_SPOKEN];
                        spoken[0] = '\0';
                        nodelio_skill_deliver_json(&a->cfg, resp, false, spoken, sizeof(spoken));
                        /* 续聊武装中勿 TTS：会掐麦；且叠 MQTT hello 易超时 */
                        if (strstr(a->id, "dsh") != nullptr && spoken[0] &&
                            !nodelio_panel_ui_dsh_is_armed()) {
                            nodelio_panel_ui_announce_spoken(spoken);
                        }
                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                        if (dsh_mutation) {
                            nodelio_panel_ui_toast_defer("结果未确认，请先查看任务，勿重复发送");
                        } else {
                            nl_ov_toast_invoke_unreachable(a->id);
                        }
                    } else {
                        nodelio_panel_ui_toast_defer("打开失败");
                    }
                    free(resp);
                }
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_home_sk", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(arg);
        nl_ov_toast("无法打开");
        return false;
    }
    return true;
}

#define WIDGET_CACHE_N 8

struct widget_cache_slot {
    char id[NL_WIDGET_SKILL_ID_MAX];
    nl_widget_preview preview;
    int64_t ms;
    bool fetching;
};

static widget_cache_slot s_wcache[WIDGET_CACHE_N];
/* 首页多个组件同时 invoke 会撞上服务端 400ms 限流，并和公网 TLS 抢射频。 */
static SemaphoreHandle_t s_widget_http_mu = nullptr;
/* 上一发 invoke 结束时刻。服务端按到达时间限 400ms，这里从结束再隔 500ms。 */
static int64_t s_widget_invoke_done_us = 0;

static widget_cache_slot *widget_cache_slot_for(const char *skill_id, bool create)
{
    widget_cache_slot *free_slot = nullptr;
    for (int i = 0; i < WIDGET_CACHE_N; i++) {
        if (s_wcache[i].id[0] && skill_id && strcmp(s_wcache[i].id, skill_id) == 0) {
            return &s_wcache[i];
        }
        if (!free_slot && !s_wcache[i].id[0]) {
            free_slot = &s_wcache[i];
        }
    }
    if (!create || !free_slot || !skill_id || !skill_id[0]) {
        return create ? free_slot : nullptr;
    }
    snprintf(free_slot->id, sizeof(free_slot->id), "%s", skill_id);
    memset(&free_slot->preview, 0, sizeof(free_slot->preview));
    free_slot->ms = 0;
    free_slot->fetching = false;
    return free_slot;
}

const char *nl_ov_skill_widget_line(const char *skill_id)
{
    widget_cache_slot *s = widget_cache_slot_for(skill_id, false);
    if (!s || !s->preview.ready || s->preview.n <= 0 || !s->preview.title[0][0]) {
        return "";
    }
    return s->preview.title[0];
}

bool nl_ov_skill_widget_preview(const char *skill_id, nl_widget_preview *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    widget_cache_slot *s = widget_cache_slot_for(skill_id, false);
    if (!s || !s->preview.ready) {
        return false;
    }
    *out = s->preview;
    return true;
}

void nl_ov_skill_widget_prefetch(const char *skill_id, const char *body, int refresh_sec)
{
    if (!skill_id || !skill_id[0] || !s_cfg.server_url[0] || !s_cfg.device_id[0]) {
        return;
    }
    if (!s_widget_http_mu) {
        s_widget_http_mu = xSemaphoreCreateMutex();
    }
    if (refresh_sec < 120) {
        refresh_sec = 600;
    }
    widget_cache_slot *slot = widget_cache_slot_for(skill_id, true);
    if (!slot || slot->fetching) {
        return;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    /* 失败只挡 20s。每日一句 refresh 为一天，失败若按成功缓存就会一直空白。 */
    const int64_t hold_ms = (slot->preview.ready && slot->preview.ok) ? (int64_t)refresh_sec * 1000 : 20000;
    if (slot->preview.ready && slot->ms > 0 && (now - slot->ms) < hold_ms) {
        return;
    }
    slot->fetching = true;

    struct invoke_arg {
        nodelio_nvs_config_t cfg;
        char id[NL_WIDGET_SKILL_ID_MAX];
        char body[HOME_INVOKE_BODY_MAX];
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        slot->fetching = false;
        return;
    }
    arg->cfg = s_cfg;
    snprintf(arg->id, sizeof(arg->id), "%s", skill_id);
    snprintf(arg->body, sizeof(arg->body), "%s", (body && body[0]) ? body : "{}");

    constexpr uint32_t kStackWords = 16384 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<invoke_arg *>(p);
                nl_widget_preview pv{};
                snprintf(pv.head, sizeof(pv.head), "加载失败");
                pv.n = 1;
                snprintf(pv.title[0], sizeof(pv.title[0]), "加载失败");
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, a->cfg.device_id, a->id);
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp = static_cast<char *>(heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                if (resp) {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err = ESP_FAIL;
                    /* 先等语音/MQTT 空窗，再进串行队列，避免占着 widget 锁空等 grace */
                    (void)nodelio_http_wait_lan_ready(6000);
                    /* 串行：后一个组件等前一个结束再发，避开 400ms 限流。
                     * 429 / 连不上 / 读超时（status 仍为 0）再试，间隔拉开。 */
                    const bool locked = s_widget_http_mu &&
                                        xSemaphoreTake(s_widget_http_mu, pdMS_TO_TICKS(40000)) == pdTRUE;
                    if (locked && s_widget_invoke_done_us != 0) {
                        const int64_t wait_us = 500000 - (esp_timer_get_time() - s_widget_invoke_done_us);
                        if (wait_us > 0 && wait_us < 3000000) {
                            vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
                        }
                    }
                    for (int attempt = 0; attempt < 3; attempt++) {
                        if (attempt > 0) {
                            vTaskDelay(pdMS_TO_TICKS(600 * attempt));
                        }
                        resp[0] = '\0';
                        status = 0;
                        err = nodelio_http_post_json_ms(url, a->cfg.device_key, a->body, &status, resp, cap - 1,
                                                        8000);
                        /* 429 再试；空收/锁忙已由 nl_http 消化，仍失败则隔开换下一组件 */
                        if (err == ESP_OK && status != 429 && status != 0) {
                            break;
                        }
                        if (status > 0 && status != 429) {
                            break;
                        }
                    }
                    if (locked) {
                        s_widget_invoke_done_us = esp_timer_get_time();
                        xSemaphoreGive(s_widget_http_mu);
                    }
                    (void)err;
                    if (resp[0]) {
                        nl_content_envelope_t *env = static_cast<nl_content_envelope_t *>(
                            heap_caps_malloc(sizeof(*env), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                        if (!env) {
                            env = static_cast<nl_content_envelope_t *>(malloc(sizeof(*env)));
                        }
                        if (env && nodelio_envelope_parse(resp, env)) {
                            const bool status_ok = strcmp(env->status, "ok") == 0;
                            const bool is_list = strcmp(env->card_type, "list") == 0 ||
                                                 strcmp(env->card_type, "news_list") == 0;
                            if (env->display_title[0]) {
                                strlcpy(pv.head, env->display_title, sizeof(pv.head));
                            }
                            if (is_list && env->track_n > 0) {
                                pv.list = true;
                                pv.ok = status_ok;
                                pv.n = env->track_n > 4 ? 4 : env->track_n;
                                for (int i = 0; i < pv.n; i++) {
                                    strlcpy(pv.title[i],
                                            env->tracks[i].title[0] ? env->tracks[i].title : "—",
                                            sizeof(pv.title[i]));
                                    if (env->tracks[i].artist[0]) {
                                        strlcpy(pv.sub[i], env->tracks[i].artist, sizeof(pv.sub[i]));
                                    }
                                }
                            } else if (env->spoken[0]) {
                                pv.ok = status_ok;
                                pv.n = 1;
                                strlcpy(pv.title[0], env->spoken, sizeof(pv.title[0]));
                            } else if (!status_ok && env->error_message[0]) {
                                pv.n = 1;
                                snprintf(pv.head, sizeof(pv.head), "加载失败");
                                strlcpy(pv.title[0], env->error_message, sizeof(pv.title[0]));
                            } else if (env->track_n > 0 && env->tracks[0].title[0]) {
                                pv.ok = status_ok;
                                pv.n = 1;
                                strlcpy(pv.title[0], env->tracks[0].title, sizeof(pv.title[0]));
                            } else {
                                pv.n = 1;
                                snprintf(pv.title[0], sizeof(pv.title[0]), "暂无内容");
                            }
                            pv.ready = true;
                        }
                        free(env);
                    }
                    free(resp);
                }
                pv.ready = true;
                std::string id_s(a->id);
                Application::GetInstance().Schedule([id_s, pv]() {
                    DisplayLockGuard lock(nl_ov_disp());
                    widget_cache_slot *s = widget_cache_slot_for(id_s.c_str(), true);
                    if (s) {
                        s->preview = pv;
                        s->ms = esp_timer_get_time() / 1000;
                        s->fetching = false;
                    }
                    if (lock && s_active_tab == NL_TAB_HOME && s_home) {
                        nl_ov_fill_home();
                    }
                });
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_sk_wdg", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(arg);
        slot->fetching = false;
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
    nl_ov_invoke_skill_async(s_pins[idx].id, s_pins[idx].body);
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

    /* 上行名称、下行「应用」；勿再叠右上浮动角标（会压住标题） */
    lv_obj_t *sub = lv_label_create(col);
    nl_ov_set_font(sub);
    lv_label_set_text(sub, it->enabled ? "应用" : "能力不足");
    lv_obj_set_style_text_color(sub, it->enabled ? UI_PURPLE : UI_TEXT_MUTED, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, lv_pct(100));

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
    if (!parent || home_skill_strip_visible_n() <= 0) {
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
        if (nl_ov_layout_current_page_has_skill(s_pins[i].id)) {
            continue;
        }
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
        const int nn = n > HOME_SKILL_MAX ? HOME_SKILL_MAX : n;
        const bool same = (nn == s_pin_n) &&
                          (nn == 0 || (items && memcmp(s_pins, items, sizeof(nl_home_skill) * (size_t)nn) == 0));
        if (same) {
            free(items);
            ESP_LOGI(TAG, "pins unchanged n=%d — skip home refill", nn);
            return;
        }
        s_pin_n = nn;
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

void nl_ov_home_skills_ingest_skills_root(const cJSON *root)
{
    if (!root) {
        return;
    }
    auto *items = static_cast<nl_home_skill *>(calloc(HOME_SKILL_MAX, sizeof(nl_home_skill)));
    if (!items) {
        return;
    }
    int n = 0;
    const cJSON *skills = cJSON_GetObjectItemCaseSensitive(root, "skills");
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
            if (!row->id[0] || strstr(row->id, "dsh.answer") != nullptr) {
                continue;
            }
            copy_json_field(row->name, sizeof(row->name), it, "name");
            store_default_invoke(row->body, sizeof(row->body), it);
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
            n++;
        }
    }
    s_fetching = false;
    apply_pins_and_refill(items, n);
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
    /* 断网/息屏休眠：保留 dirty，等有 IP 再拉；禁止进 getaddrinfo（Invalid mbox） */
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
        return;
    }
    s_fetching = true;
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        s_fetching = false;
        return;
    }
    *cfgp = s_cfg;
    constexpr uint32_t kStackWords = 16384 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *cfg = static_cast<nodelio_nvs_config_t *>(p);
                auto finish = [](bool clear_fetching) {
                    if (clear_fetching) {
                        Application::GetInstance().Schedule([]() { s_fetching = false; });
                    }
                    vTaskDeleteWithCaps(nullptr);
                };
                auto *items = static_cast<nl_home_skill *>(calloc(HOME_SKILL_MAX, sizeof(nl_home_skill)));
                int n = 0;
                if (!items) {
                    free(cfg);
                    finish(true);
                    return;
                }
                char base[160];
                nl_url_strip_base(cfg->server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);
                constexpr int kListBuf = 24576;
                char *buf = static_cast<char *>(heap_caps_malloc(kListBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!buf) {
                    buf = static_cast<char *>(malloc(kListBuf));
                }
                if (!buf) {
                    free(items);
                    free(cfg);
                    finish(true);
                    return;
                }
                buf[0] = '\0';
                int status = 0;
                if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
                    free(buf);
                    free(items);
                    free(cfg);
                    finish(true);
                    return;
                }
                esp_err_t err =
                    nodelio_http_get_ms(url, cfg->device_key, nullptr, &status, buf, kListBuf - 1, 8000);
                if (err != ESP_OK || (status != 200 && status != 201)) {
                    ESP_LOGW(TAG, "fetch pins status=%d err=%d", status, err);
                    free(buf);
                    free(items);
                    free(cfg);
                    /* 保留 dirty，唤醒后重试；勿用 n=0 清空已有钉选 */
                    finish(true);
                    return;
                }
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
                        /* dsh.answer 仅审批按钮，不进首页钉选条 */
                        if (!row->id[0] || strstr(row->id, "dsh.answer") != nullptr) {
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
                free(buf);
                free(cfg);
                apply_pins_and_refill(items, n);
                finish(false);
            },
            "nl_home_pins", kStackWords, cfgp, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(cfgp);
        s_fetching = false;
    }
}
