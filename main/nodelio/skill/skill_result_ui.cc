#include "skill_result_ui.h"

#include "device_caps.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nodelio_overlay.h"
#include "overlay_priv.h"
#include "panel_ui.h"
#include "skill_runtime.h"
#include "ui_theme.h"

#include "application.h"
#include "board.h"
#include "display.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>

LV_FONT_DECLARE(font_material_symbols_20_4);

#define TAG "nl_skill_ui"

#define SKILL_CARD_W 412
/* 480 屏：顶栏 32 + 间隙 + 卡；含 filter chip 时需约 424 才能露出 4 行完整行高。
 * 有 now_playing 条时须再扣 SKILL_NOW_H+SKILL_GAP，否则卡底会画出屏外。 */
#define SKILL_CARD_H_MAX 424
#define SKILL_NOW_H 56
#define SKILL_GAP 12
#define SKILL_CARD_H_MIN 180
#define SKILL_BORDER_OPA 200
#define SKILL_SCRIM_OPA 170
#define SKILL_EQ_N 4
#define SKILL_CTRL_SZ 34
#define SKILL_MINI_SZ 52
#define SKILL_ANIM_MS 240
#define SKILL_ANIM_SLIDE 16
#define SKILL_PAGE_COLS 2
#define SKILL_PAGE_ROWS 5
#define SKILL_PAGE_SIZE (SKILL_PAGE_COLS * SKILL_PAGE_ROWS)
/* 普通 list（DSH 等）：单列舒适行，避免双列挤成省略号 */
#define SKILL_LIST_COLS 1
#define SKILL_LIST_ROWS 4
#define SKILL_LIST_ROW_H 68
#define SKILL_MUSIC_ROW_H 50
#define NEWS_ARTICLE_CAP (20 * 1024)
/* MQTT MCP 回包总长易被对端掐断；约 900 汉字一段，多数文章 1～2 段。 */
#define NEWS_MCP_CHUNK 2800
/* 云端回源拉正文最多约 8s，默认 HTTP 读超时 2.5s 会在状态行到达前断开 */
#define NEWS_HTTP_IO_MS 12000
#define NEWS_TASK_STACK 10240
/* 任务详情页静默拉最新末轮；仅亮屏+停留详情时跑，省电 */
#define DSH_DETAIL_POLL_MS 10000

static lv_obj_t *s_scrim;
static lv_obj_t *s_card;
static lv_obj_t *s_filter_bar;
static lv_obj_t *s_dsh_status;      /* 续聊/执行中状态条：EQ + 文案 */
static lv_obj_t *s_dsh_status_lbl;
static lv_obj_t *s_dsh_eq[3];
static lv_obj_t *s_dsh_detail_bar;  /* 详情底栏：续聊悬浮，不随正文滚走 */
static lv_obj_t *s_card_list;
static lv_obj_t *s_card_title;
static lv_obj_t *s_card_sub;
static lv_obj_t *s_now_bar;
static lv_obj_t *s_now_eq_wrap;
static lv_obj_t *s_now_eq[SKILL_EQ_N];
static lv_obj_t *s_now_lbl;
static lv_obj_t *s_now_sub;
static lv_obj_t *s_now_btns;
static lv_obj_t *s_now_pause_btn;
static lv_obj_t *s_mini_fab;
static lv_obj_t *s_mini_ic;
static lv_timer_t *s_ttl_tmr;
static lv_timer_t *s_dsh_detail_poll;
static char s_dsh_detail_ws[200];
static char s_dsh_detail_sess[80];
static bool s_dsh_detail_busy;
static lv_anim_t s_now_border_anim;
static bool s_now_border_anim_on;
static bool s_mini_pulse_on;
static bool s_dsh_eq_anim_on;
static bool s_dsh_row_pulse_on;
static lv_obj_t *s_dsh_focus_row; /* 当前高亮行，rebuild 时清空 */
enum dsh_ui_phase_t : uint8_t { DSH_UI_IDLE = 0, DSH_UI_ARMED, DSH_UI_DISPATCHED };
static dsh_ui_phase_t s_dsh_phase = DSH_UI_IDLE;
static char s_dsh_focus_sess[80];
static char s_dsh_focus_ws[200];
static char s_dsh_last_instr[96];
static bool s_minimized;
static bool s_ui_closing;
static uint32_t s_close_gen;
static nodelio_nvs_config_t s_skill_cfg;
/** ~50KB：必须 PSRAM。放内部 BSS 会把云端 UDP/AES 连续堆挤到 errno=12。 */
static nl_content_envelope_t *s_env;
static bool s_have_env;

static bool ensure_skill_env(void)
{
    if (s_env) {
        return true;
    }
    s_env = (nl_content_envelope_t *)heap_caps_calloc(1, sizeof(*s_env),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_env) {
        ESP_LOGE(TAG, "skill env PSRAM fail need=%u", (unsigned)sizeof(nl_content_envelope_t));
        return false;
    }
    ESP_LOGI(TAG, "skill env → PSRAM %u bytes", (unsigned)sizeof(nl_content_envelope_t));
    return true;
}
static int s_play_idx = -1;
static int s_list_page;
static lv_obj_t *s_hdr_mid_btn;
static lv_obj_t *s_row_idx_labels[64];  /* 序号胶囊标签（数字/耳机） */
static lv_obj_t *s_row_btns[64];       /* 播放/暂停按钮 */
static int s_news_detail = -1;
static uint32_t s_news_gen = 1;
static bool s_news_screen_hook = false;

struct news_job_t {
    int slot;
    uint32_t gen;
    char url[320];
    int nav; /* 0 拉正文；>0 下一篇并标已读；<0 上一篇并标已读 */
};
static QueueHandle_t s_reshuffle_q = nullptr;
static StackType_t *s_reshuffle_stack = nullptr;
static StaticTask_t s_reshuffle_tcb;
static QueueHandle_t s_news_q = nullptr;
static lv_obj_t *s_news_body_lbl = nullptr;
/** 详情正文驻 PSRAM，供 lv_label_set_text_static，避免长文二次占 LVGL 池失败后仍显示摘要 */
static char *s_news_body_heap = nullptr;
static StackType_t *s_news_stack = nullptr;
static StaticTask_t s_news_tcb;

/** 禁止 move_foreground(status_bar)：flex 列里会把它挤到屏幕最底 */
static void ensure_chrome_order(void)
{
    if (s_status_bar && s_root) {
        lv_obj_move_to_index(s_status_bar, 0);
    }
}

static void apply_card_chrome(void);

/** 列表卡可用高度：now_playing 可见时为顶条让位 */
static int skill_card_h_max(void)
{
    int h = SKILL_CARD_H_MAX;
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        h -= SKILL_NOW_H + SKILL_GAP;
    }
    return h < SKILL_CARD_H_MIN ? SKILL_CARD_H_MIN : h;
}

static void layout_skill_layers(void)
{
    int y = NL_UI_STATUS_H + SKILL_GAP;
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align(s_now_bar, LV_ALIGN_TOP_MID, 0, y);
        y += SKILL_NOW_H + SKILL_GAP;
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        /* 播放条显隐会改 y；同步缩/扩卡高，避免内容顶出屏底 */
        if (s_have_env && s_env) {
            apply_card_chrome();
        } else {
            const int h = skill_card_h_max();
            lv_obj_set_height(s_card, h);
            lv_obj_set_style_max_height(s_card, h, 0);
        }
        lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, y);
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align(s_mini_fab, LV_ALIGN_BOTTOM_RIGHT, -14, -(NL_UI_TAB_H + 14));
    }
}

static void raise_skill_layers(void)
{
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_scrim);
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_card);
    }
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_now_bar);
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_mini_fab);
    }
}

void nodelio_skill_result_raise(void)
{
    if (!s_skill_ui_active) {
        return;
    }
    raise_skill_layers();
    ensure_chrome_order();
}

static void set_drawer_for_skill(bool skill_on)
{
    s_skill_ui_active = skill_on;
    if (skill_on) {
        if (s_ai_drawer) {
            lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_ai_mini_fab) {
            lv_obj_add_flag(s_ai_mini_fab, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    nl_ov_restore_ai_drawer_if_session();
}

static void stop_dsh_detail_poll(void)
{
    if (s_dsh_detail_poll) {
        lv_timer_del(s_dsh_detail_poll);
        s_dsh_detail_poll = nullptr;
    }
}

static void clear_ttl(void)
{
    if (s_ttl_tmr) {
        lv_timer_del(s_ttl_tmr);
    }
    s_ttl_tmr = nullptr;
}

static void dsh_fetch_detail(const char *ws, const char *sess, bool silent);
static void on_dsh_detail_poll(lv_timer_t *t);
static void fill_list(void);
static void anim_eq_height(void *var, int32_t v);
static void anim_border_opa(void *var, int32_t v);

static bool dsh_is_detail_view(void)
{
    return s_have_env && s_env && s_env->detail_body && s_env->detail_body[0] &&
           strstr(s_env->app_id, "dsh") != nullptr && s_card &&
           !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_minimized;
}

/** 从 track 的 arm 动作取出 workspace/session */
static void dsh_extract_arm(const nl_env_track_t *tr, char *ws, size_t ws_len, char *sess,
                            size_t sess_len)
{
    if (ws && ws_len) {
        ws[0] = '\0';
    }
    if (sess && sess_len) {
        sess[0] = '\0';
    }
    if (!tr) {
        return;
    }
    for (int ai = 0; ai < tr->invoke_n; ai++) {
        const char *pj = tr->invokes[ai].params_json;
        if (!pj[0] || strstr(pj, "\"arm\"") == nullptr) {
            continue;
        }
        cJSON *root = cJSON_Parse(pj);
        if (!root) {
            continue;
        }
        const cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "workspace");
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "session");
        if (ws && ws_len && cJSON_IsString(jw) && jw->valuestring) {
            snprintf(ws, ws_len, "%s", jw->valuestring);
        }
        if (sess && sess_len && cJSON_IsString(js) && js->valuestring) {
            snprintf(sess, sess_len, "%s", js->valuestring);
        }
        cJSON_Delete(root);
        if (sess && sess[0]) {
            break;
        }
    }
    if (sess && sess_len && !sess[0] && tr->id[0]) {
        snprintf(sess, sess_len, "%s", tr->id);
    }
}

static void remember_dsh_detail_keys(void)
{
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    if (!s_have_env || !s_env || s_env->track_n <= 0) {
        return;
    }
    dsh_extract_arm(&s_env->tracks[0], s_dsh_detail_ws, sizeof(s_dsh_detail_ws), s_dsh_detail_sess,
                    sizeof(s_dsh_detail_sess));
}

static bool dsh_track_is_focus(int track_idx)
{
    if (s_dsh_phase == DSH_UI_IDLE || !s_dsh_focus_sess[0] || !s_env) {
        return false;
    }
    if (track_idx < 0 || track_idx >= s_env->track_n) {
        return false;
    }
    char ws[200] = {0};
    char sess[80] = {0};
    dsh_extract_arm(&s_env->tracks[track_idx], ws, sizeof(ws), sess, sizeof(sess));
    return sess[0] && strcmp(sess, s_dsh_focus_sess) == 0;
}

static void stop_dsh_status_anims(void)
{
    if (s_dsh_eq_anim_on) {
        for (int i = 0; i < 3; i++) {
            if (s_dsh_eq[i]) {
                lv_anim_delete(s_dsh_eq[i], anim_eq_height);
                lv_obj_set_height(s_dsh_eq[i], 6 + i * 3);
            }
        }
        s_dsh_eq_anim_on = false;
    }
    if (s_dsh_row_pulse_on && s_dsh_focus_row) {
        lv_anim_delete(s_dsh_focus_row, anim_border_opa);
        s_dsh_row_pulse_on = false;
    }
}

static void start_dsh_status_anims(void)
{
    if (!s_dsh_status || lv_obj_has_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    stop_dsh_status_anims();
    static const int32_t k_lo[3] = {5, 8, 6};
    static const int32_t k_hi[3] = {14, 18, 16};
    static const uint32_t k_ms[3] = {360, 440, 400};
    for (int i = 0; i < 3; i++) {
        if (!s_dsh_eq[i]) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dsh_eq[i]);
        lv_anim_set_values(&a, k_lo[i], k_hi[i]);
        lv_anim_set_time(&a, k_ms[i]);
        lv_anim_set_playback_time(&a, k_ms[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_eq_height);
        lv_anim_set_delay(&a, (uint32_t)(i * 60));
        lv_anim_start(&a);
    }
    s_dsh_eq_anim_on = true;
    if (s_dsh_focus_row) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, s_dsh_focus_row);
        lv_anim_set_values(&b, 90, 220);
        lv_anim_set_time(&b, 700);
        lv_anim_set_playback_time(&b, 700);
        lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&b, anim_border_opa);
        lv_anim_start(&b);
        s_dsh_row_pulse_on = true;
    }
}

static void dsh_clear_ui_phase(void)
{
    stop_dsh_status_anims();
    s_dsh_phase = DSH_UI_IDLE;
    s_dsh_focus_sess[0] = '\0';
    s_dsh_focus_ws[0] = '\0';
    s_dsh_last_instr[0] = '\0';
    s_dsh_focus_row = nullptr;
    if (s_dsh_status) {
        lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_status, 0);
    }
}

static void dsh_paint_status_strip(void)
{
    if (!s_dsh_status || !s_dsh_status_lbl) {
        return;
    }
    if (s_dsh_phase == DSH_UI_IDLE) {
        stop_dsh_status_anims();
        lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_status, 0);
        return;
    }
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_dsh_status, 32);
    uint32_t pill = UI_C_TILE_SWITCH;
    uint32_t bar = UI_C_ACCENT_SOFT;
    const char *txt = "续聊中 · 请直接说指令";
    if (s_dsh_phase == DSH_UI_DISPATCHED) {
        pill = UI_C_TILE_PWM;
        bar = UI_C_INFO;
        if (s_dsh_last_instr[0]) {
            static char buf[120];
            snprintf(buf, sizeof(buf), "已投递 · %s", s_dsh_last_instr);
            txt = buf;
        } else {
            txt = "已投递 · 执行中 · 点刷新看进度";
        }
    }
    lv_obj_set_style_bg_color(s_dsh_status, lv_color_hex(pill), 0);
    for (int i = 0; i < 3; i++) {
        if (s_dsh_eq[i]) {
            lv_obj_set_style_bg_color(s_dsh_eq[i], lv_color_hex(bar), 0);
        }
    }
    lv_label_set_text(s_dsh_status_lbl, txt);
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    if (s_card_sub) {
        lv_label_set_text(s_card_sub,
                          s_dsh_phase == DSH_UI_ARMED ? "续聊中 · 直接说 · 勿点别处卡片"
                                                     : "已投递 · 等回复 · 点刷新更新");
        lv_obj_set_style_text_color(s_card_sub, lv_color_hex(bar), 0);
    }
    start_dsh_status_anims();
}

static void dsh_style_focus_row(lv_obj_t *row, int track_idx, lv_obj_t *idx_pod, lv_obj_t *artist_lbl)
{
    if (!row || !dsh_track_is_focus(track_idx)) {
        return;
    }
    s_dsh_focus_row = row;
    const bool dispatched = s_dsh_phase == DSH_UI_DISPATCHED;
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, dispatched ? UI_INFO : UI_ACCENT, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, dispatched ? UI_TILE_PWM : UI_TILE_SWITCH, 0);
    if (idx_pod) {
        lv_obj_set_style_bg_color(idx_pod, dispatched ? UI_CAPSULE_PWM : UI_CAPSULE_ON, 0);
        lv_obj_t *ic = lv_obj_get_child(idx_pod, 0);
        if (ic) {
            lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
            lv_label_set_text(ic, dispatched ? MATERIAL_SYMBOLS_PROGRESS_ACTIVITY : MATERIAL_SYMBOLS_MIC);
            lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
        }
    }
    if (artist_lbl && s_dsh_phase == DSH_UI_ARMED) {
        lv_label_set_text(artist_lbl, "续聊中 · 开麦等待你的指令");
        lv_obj_set_style_text_color(artist_lbl, UI_ACCENT_SOFT, 0);
    } else if (artist_lbl && dispatched) {
        if (s_dsh_last_instr[0]) {
            char line[128];
            snprintf(line, sizeof(line), "已投递 · %s", s_dsh_last_instr);
            lv_label_set_text(artist_lbl, line);
        } else {
            lv_label_set_text(artist_lbl, "已投递 · 正在执行…");
        }
        lv_obj_set_style_text_color(artist_lbl, UI_INFO, 0);
    }
}

static void dsh_enter_armed(const char *ws, const char *sess)
{
    s_dsh_phase = DSH_UI_ARMED;
    s_dsh_focus_ws[0] = '\0';
    s_dsh_focus_sess[0] = '\0';
    s_dsh_last_instr[0] = '\0';
    s_dsh_focus_row = nullptr;
    if (ws && ws[0]) {
        snprintf(s_dsh_focus_ws, sizeof(s_dsh_focus_ws), "%s", ws);
    }
    if (sess && sess[0]) {
        snprintf(s_dsh_focus_sess, sizeof(s_dsh_focus_sess), "%s", sess);
    }
    /* 列表/详情已在：重填高亮与底栏续聊态 */
    if (s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr && s_card_list) {
        fill_list();
    } else {
        dsh_paint_status_strip();
    }
}

static void start_dsh_detail_poll(void)
{
    remember_dsh_detail_keys();
    if (!s_dsh_detail_sess[0]) {
        stop_dsh_detail_poll();
        return;
    }
    if (!s_dsh_detail_poll) {
        s_dsh_detail_poll = lv_timer_create(on_dsh_detail_poll, DSH_DETAIL_POLL_MS, nullptr);
    } else {
        lv_timer_reset(s_dsh_detail_poll);
    }
}

static void on_dsh_detail_poll(lv_timer_t *t)
{
    (void)t;
    if (!dsh_is_detail_view() || s_dsh_detail_busy || !s_dsh_detail_sess[0]) {
        return;
    }
    if (!nodelio_panel_ui_screen_on()) {
        return;
    }
    dsh_fetch_detail(s_dsh_detail_ws, s_dsh_detail_sess, true);
}

static void close_list_keep_player(void);

static void on_ttl(lv_timer_t *t)
{
    (void)t;
    s_ttl_tmr = nullptr;
    /* 播歌中勿自动关列表：ttl_sec=300 常撞上首曲尾声，表现为「列表突然没了又接着播」 */
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        return;
    }
    if (!s_env) {
        nodelio_skill_result_hide();
        return;
    }
    /* 曲库空闲超时：只收起卡片，保留歌单，点播放条「点此打开列表」可再开 */
    if (strcmp(s_env->card_type, "music_list") == 0 && s_env->track_n > 0) {
        close_list_keep_player();
        return;
    }
    /* DSH/新闻等 list：ttl 仅缓存建议，勿自动关（用户还在看） */
    if (strcmp(s_env->card_type, "list") == 0) {
        ESP_LOGI(TAG, "ttl expired — keep list card open");
        return;
    }
    nodelio_skill_result_hide();
}

static void fill_list(void);
static void apply_card_chrome(void);
static void reopen_list(void);
static void request_reshuffle(void);
static void json_escape_str(char *out, size_t out_len, const char *in);
static void minimize_to_fab(void);
static void expand_from_fab(void);
static void anim_show_obj(lv_obj_t *obj, bool from_below);
static void reset_obj_anim_style(lv_obj_t *obj);
static void anim_eq_height(void *var, int32_t v);
static void anim_border_opa(void *var, int32_t v);
static void hide_mini_fab(void);
static void show_mini_fab(bool playing);
static void stop_now_anims(void);
static void start_now_anims(void);
static void stop_mini_pulse(void);
static void hide_mini_fab(void);
static void dismiss_library_and_player(void);

static void close_list_keep_player(void)
{
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    reset_obj_anim_style(s_scrim);
    reset_obj_anim_style(s_card);
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        if (s_now_bar) {
            lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_now_sub) {
            lv_label_set_text(s_now_sub, nodelio_media_is_paused() ? "已暂停 · 点播放继续"
                                                                   : "点此打开列表");
        }
        set_drawer_for_skill(false);
        layout_skill_layers();
        raise_skill_layers();
        ensure_chrome_order();
        return;
    }
    nodelio_skill_result_hide();
}

static void on_close_card(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* X：关列表；在播则留播放条，未播则全关 */
    close_list_keep_player();
}

static void on_scrim_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 检查点击是否在顶栏区域，如果是则不处理（让点击穿透到 status_bar） */
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    if (p.y < NL_UI_STATUS_H) {
        return;
    }
    /* 点遮罩：有内容可恢复则收成悬浮球，否则关掉 */
    if (nodelio_media_is_playing() || nodelio_media_is_paused() ||
        (s_have_env && s_env && s_env->track_n > 0)) {
        minimize_to_fab();
    } else {
        nodelio_skill_result_hide();
    }
}

static void on_minimize(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    minimize_to_fab();
}

static void on_mini_fab(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    expand_from_fab();
}

static void on_reopen_list(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    reopen_list();
}

static void dsh_refresh_list(void)
{
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("未配置服务器");
        return;
    }
    char ws[200] = {0};
    if (s_have_env && s_env) {
        for (int i = 0; i < s_env->filter_n; i++) {
            if (s_env->filters[i].selected && s_env->filters[i].workspace[0]) {
                snprintf(ws, sizeof(ws), "%s", s_env->filters[i].workspace);
                break;
            }
        }
        if (s_env->detail_body) {
            heap_caps_free(s_env->detail_body);
            s_env->detail_body = nullptr;
        }
    }
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    struct refresh_arg {
        nodelio_nvs_config_t cfg;
        char ws[200];
    };
    auto *arg = static_cast<refresh_arg *>(malloc(sizeof(refresh_arg)));
    if (!arg) {
        nl_ov_toast("内存不足");
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->ws, sizeof(arg->ws), "%s", ws);
    nl_ov_toast("刷新…");
    constexpr uint32_t kStackWords = 12288 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<refresh_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/dsh.status/invoke", base,
                         a->cfg.device_id);
                char ws_esc[400];
                json_escape_str(ws_esc, sizeof(ws_esc), a->ws);
                char body[440];
                if (ws_esc[0]) {
                    snprintf(body, sizeof(body), "{\"workspace\":\"%s\"}", ws_esc);
                } else {
                    snprintf(body, sizeof(body), "{}");
                }
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
                    esp_err_t err =
                        nodelio_http_post_json_ms(url, a->cfg.device_key, body, &status, resp, cap - 1, 12000);
                    if (err == ESP_OK && resp[0] && (status == 200 || status == 201)) {
                        nodelio_skill_deliver_json(&a->cfg, resp, false, nullptr, 0);
                    } else {
                        nodelio_panel_ui_toast_defer("刷新失败");
                    }
                    free(resp);
                }
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_dsh_rf", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(arg);
        nl_ov_toast("刷新失败");
    }
}

static void on_reshuffle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* DSH：刷新按钮回到/重拉进度列表（详情页也走这里） */
    if (s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr) {
        dsh_refresh_list();
        return;
    }
    request_reshuffle();
}

static bool skill_ui_is_up(void);

/** 曲库双列紧凑；其它 list 默认单列舒适（density=compact 仍双列） */
static bool list_use_comfortable(void)
{
    if (strcmp(s_env->card_type, "music_list") == 0) {
        return false;
    }
    if (strcmp(s_env->density, "compact") == 0) {
        return false;
    }
    /* comfortable / 空 / 其它 → 单列 */
    return true;
}

static int list_cols(void)
{
    return list_use_comfortable() ? SKILL_LIST_COLS : SKILL_PAGE_COLS;
}

static int list_page_size(void)
{
    if (list_use_comfortable()) {
        /* 单列指滑看全部，不再切页（曲库仍用双列分页） */
        return s_env->track_n > 0 ? s_env->track_n : (SKILL_LIST_COLS * SKILL_LIST_ROWS);
    }
    return SKILL_PAGE_SIZE;
}

static int list_row_h(void)
{
    return list_use_comfortable() ? SKILL_LIST_ROW_H : SKILL_MUSIC_ROW_H;
}

static int list_page_count(void)
{
    if (s_env->track_n <= 0) {
        return 1;
    }
    const int ps = list_page_size();
    return (s_env->track_n + ps - 1) / ps;
}

static void set_hdr_mid_icon(const char *sym)
{
    if (!s_hdr_mid_btn || !sym) {
        return;
    }
    lv_obj_t *ic = lv_obj_get_child(s_hdr_mid_btn, 0);
    if (ic) {
        lv_label_set_text(ic, sym);
    }
}

/** 列表翻页（曲库等）；新闻收成悬浮球。DSH 详情勿用此钮（易与刷新混淆）。 */
static void on_hdr_mid(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_env) {
        return;
    }
    if (strcmp(s_env->card_type, "news_list") == 0) {
        minimize_to_fab();
        return;
    }
    /* DSH 详情：顶栏中间钮已隐藏；若仍点到则回列表 */
    if (s_env->detail_body && s_env->detail_body[0] && strstr(s_env->app_id, "dsh") != nullptr) {
        dsh_refresh_list();
        return;
    }
    const int pages = list_page_count();
    if (pages <= 1) {
        return;
    }
    s_list_page = (s_list_page + 1) % pages;
    fill_list();
}

static void on_dsh_back_list(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    dsh_refresh_list();
}

static void reopen_list(void)
{
    if (s_env->track_n <= 0 || !s_card) {
        nl_ov_toast("暂无曲目列表");
        return;
    }
    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    s_minimized = false;
    hide_mini_fab();
    s_have_env = true;
    const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
    lv_label_set_text(s_card_title, title);
    fill_list();
    set_drawer_for_skill(true);
    if (s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        anim_show_obj(s_scrim, false);
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    anim_show_obj(s_card, true);
    if (s_now_sub) {
        lv_label_set_text(s_now_sub, "正在播放");
    }
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
}

static void reshuffle_worker(void *)
{
    uint8_t kick = 0;
    for (;;) {
        if (xQueueReceive(s_reshuffle_q, &kick, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char base[160];
        nl_url_strip_base(s_skill_cfg.server_url, base, sizeof(base));
        const char *sid = s_env->app_id[0] ? s_env->app_id : "music.search";
        if (strcmp(sid, "music_search") == 0) {
            sid = "music.search";
        }
        char url[360];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, s_skill_cfg.device_id,
                 sid);
        const int cap = nodelio_caps_max_content_bytes() + 512;
        char *resp = (char *)heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!resp) {
            resp = (char *)malloc((size_t)cap);
        }
        if (!resp) {
            nodelio_panel_ui_toast_defer("内存不足");
            continue;
        }
        resp[0] = '\0';
        int status = 0;
        esp_err_t err = nodelio_http_post_json(url, s_skill_cfg.device_key, "{\"keyword\":\"\",\"shuffle\":true}",
                                               &status, resp, cap - 1);
        if (err == ESP_OK && (status == 200 || status == 201) && resp[0] && strlen(resp) < (size_t)(cap - 2)) {
            nodelio_skill_deliver_json(&s_skill_cfg, resp, false, nullptr, 0);
        } else {
            nodelio_panel_ui_toast_defer("换一批失败");
        }
        free(resp);
    }
}

static void ensure_reshuffle_worker(void)
{
    if (s_reshuffle_q) {
        return;
    }
    s_reshuffle_q = xQueueCreate(1, sizeof(uint8_t));
    if (!s_reshuffle_q) {
        return;
    }
    const uint32_t bytes = 16384;
    s_reshuffle_stack = (StackType_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_reshuffle_stack) {
        s_reshuffle_stack = (StackType_t *)malloc(bytes);
    }
    if (!s_reshuffle_stack) {
        vQueueDelete(s_reshuffle_q);
        s_reshuffle_q = nullptr;
        return;
    }
    if (xTaskCreateStatic(reshuffle_worker, "nl_reshuffle", bytes / sizeof(StackType_t), nullptr, 3,
                          s_reshuffle_stack, &s_reshuffle_tcb) == nullptr) {
        free(s_reshuffle_stack);
        s_reshuffle_stack = nullptr;
        vQueueDelete(s_reshuffle_q);
        s_reshuffle_q = nullptr;
    }
}

static void request_reshuffle(void)
{
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("未配置服务器");
        return;
    }
    ensure_reshuffle_worker();
    if (!s_reshuffle_q) {
        nl_ov_toast("换一批失败");
        return;
    }
    uint8_t kick = 1;
    if (xQueueSend(s_reshuffle_q, &kick, 0) != pdTRUE) {
        nl_ov_toast("正在换一批");
        return;
    }
    nl_ov_toast("换一批…");
}

static void play_at(int idx);

/* 播放器关闭时恢复列表序号，避免耳机图标一直占着当前行 */
static void clear_play_mark(void)
{
    if (s_play_idx < 0 || s_play_idx >= 64) {
        s_play_idx = -1;
        return;
    }
    if (s_row_idx_labels[s_play_idx]) {
        char idx_buf[16];
        snprintf(idx_buf, sizeof(idx_buf), "%d", s_play_idx + 1);
        nl_ov_set_font(s_row_idx_labels[s_play_idx]);
        lv_label_set_text(s_row_idx_labels[s_play_idx], idx_buf);
        lv_obj_set_style_text_color(s_row_idx_labels[s_play_idx], UI_ACCENT_SOFT, 0);
    }
    if (s_row_btns[s_play_idx]) {
        lv_obj_t *lbl = lv_obj_get_child(s_row_btns[s_play_idx], 0);
        if (lbl) {
            lv_label_set_text(lbl, MATERIAL_SYMBOLS_PLAY_ARROW);
        }
    }
    s_play_idx = -1;
}

/* 只更新指定行的图标，避免重建整个列表 */
static void update_row_icon(int idx)
{
    if (idx < 0 || idx >= 64) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    /* 更新序号胶囊：当前播放显示耳机 */
    if (s_row_idx_labels[idx]) {
        lv_obj_set_style_text_font(s_row_idx_labels[idx], &font_material_symbols_20_4, 0);
        lv_label_set_text(s_row_idx_labels[idx], MATERIAL_SYMBOLS_HEADPHONES);
        lv_obj_set_style_text_color(s_row_idx_labels[idx], UI_ACCENT, 0);
    }
    /* 更新播放按钮：改为暂停图标 */
    if (s_row_btns[idx]) {
        lv_obj_t *lbl = lv_obj_get_child(s_row_btns[idx], 0);
        if (lbl) {
            lv_label_set_text(lbl, MATERIAL_SYMBOLS_PAUSE);
        }
    }
}

static void play_at(int idx)
{
    if (!nodelio_caps_has_media()) {
        nl_ov_toast("本机暂不支持播放");
        return;
    }
    if (idx < 0 || idx >= s_env->track_n || !s_env->tracks[idx].playback_url[0]) {
        return;
    }
    /* 恢复上一首的图标 */
    if (s_play_idx >= 0 && s_play_idx < 64) {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            if (s_row_idx_labels[s_play_idx]) {
                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%d", s_play_idx + 1);
                nl_ov_set_font(s_row_idx_labels[s_play_idx]);
                lv_label_set_text(s_row_idx_labels[s_play_idx], idx_buf);
                lv_obj_set_style_text_color(s_row_idx_labels[s_play_idx], UI_ACCENT_SOFT, 0);
            }
            if (s_row_btns[s_play_idx]) {
                lv_obj_t *lbl = lv_obj_get_child(s_row_btns[s_play_idx], 0);
                if (lbl) {
                    lv_label_set_text(lbl, MATERIAL_SYMBOLS_PLAY_ARROW);
                }
            }
        }
    }
    s_play_idx = idx;
    (void)nodelio_media_play_track(&s_skill_cfg, &s_env->tracks[idx]);
    /* 只更新当前行的图标，避免重建整个列表 */
    update_row_icon(idx);
}

static int neighbor_playable(int from, int dir)
{
    if (s_env->track_n <= 0 || (dir != 1 && dir != -1)) {
        return -1;
    }
    /* from < 0：下一首从 0 起，上一首从末尾起（换一批后当前曲可能不在新列表里） */
    int start = from;
    if (start < 0 || start >= s_env->track_n) {
        start = (dir > 0) ? -1 : 0;
    }
    for (int step = 1; step <= s_env->track_n; step++) {
        int idx = start + dir * step;
        idx %= s_env->track_n;
        if (idx < 0) {
            idx += s_env->track_n;
        }
        if (s_env->tracks[idx].playback_url[0]) {
            return idx;
        }
    }
    return -1;
}

static void dsh_fetch_session_detail(int track_idx);
static void fill_dsh_detail(void);

static void on_play_row(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* DSH 列表：点行看末轮全文；有播放 URL 的仍走点播 */
    if (s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr &&
        strcmp(s_env->card_type, "music_list") != 0) {
        if (idx >= 0 && idx < s_env->track_n && !s_env->tracks[idx].playback_url[0]) {
            dsh_fetch_session_detail(idx);
            return;
        }
    }
    play_at(idx);
}

/** JSON 字符串转义（路径反斜杠）；out 须大于 in*2。 */
static void json_escape_str(char *out, size_t out_len, const char *in)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t o = 0;
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (const char *p = in; *p && o + 2 < out_len; ++p) {
        if (*p == '\\' || *p == '"') {
            out[o++] = '\\';
        }
        if ((unsigned char)*p < 0x20) {
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/** 拉 dsh.status 会话详情（含 body）。silent：定时刷新，无 toast、失败不打扰。 */
static void dsh_fetch_detail(const char *ws, const char *sess, bool silent)
{
    if (!sess || !sess[0] || !s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        if (!silent) {
            nl_ov_toast("无法打开详情");
        }
        return;
    }
    if (s_dsh_detail_busy) {
        return;
    }
    struct detail_arg {
        nodelio_nvs_config_t cfg;
        char ws[200];
        char sess[80];
        bool silent;
    };
    auto *arg = static_cast<detail_arg *>(malloc(sizeof(detail_arg)));
    if (!arg) {
        if (!silent) {
            nl_ov_toast("内存不足");
        }
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->ws, sizeof(arg->ws), "%s", ws ? ws : "");
    snprintf(arg->sess, sizeof(arg->sess), "%s", sess);
    arg->silent = silent;
    snprintf(s_dsh_detail_ws, sizeof(s_dsh_detail_ws), "%s", arg->ws);
    snprintf(s_dsh_detail_sess, sizeof(s_dsh_detail_sess), "%s", arg->sess);
    s_dsh_detail_busy = true;
    if (!silent) {
        nl_ov_toast("加载回复…");
    }
    constexpr uint32_t kStackWords = 12288 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<detail_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[480];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/dsh.status/invoke", base,
                         a->cfg.device_id);
                /* ws≤199 转义≤398；sess≤79 转义≤158；外壳约 34 → body≥591 */
                char ws_esc[400];
                char sess_esc[160];
                json_escape_str(ws_esc, sizeof(ws_esc), a->ws);
                json_escape_str(sess_esc, sizeof(sess_esc), a->sess);
                char body[640];
                if (ws_esc[0]) {
                    snprintf(body, sizeof(body), "{\"workspace\":\"%s\",\"session\":\"%s\"}", ws_esc,
                             sess_esc);
                } else {
                    snprintf(body, sizeof(body), "{\"session\":\"%s\"}", sess_esc);
                }
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp = static_cast<char *>(
                    heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                if (!resp) {
                    if (!a->silent) {
                        nodelio_panel_ui_toast_defer("内存不足");
                    }
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err =
                        nodelio_http_post_json_ms(url, a->cfg.device_key, body, &status, resp, cap - 1, 12000);
                    if (err == ESP_OK && resp[0] && (status == 200 || status == 201)) {
                        /* 静默轮询：用户已回列表/收起则丢弃，避免把详情又顶回来 */
                        const bool still =
                            !a->silent ||
                            (s_dsh_detail_sess[0] && strcmp(s_dsh_detail_sess, a->sess) == 0 &&
                             !s_minimized && s_have_env && s_env && s_env->detail_body &&
                             s_env->detail_body[0]);
                        if (still) {
                            char spoken[NL_ENV_SPOKEN];
                            spoken[0] = '\0';
                            nodelio_skill_deliver_json(&a->cfg, resp, false, spoken, sizeof(spoken));
                            /* 首次打开详情播报；定时刷新不念 */
                            if (!a->silent && spoken[0]) {
                                nodelio_panel_ui_announce_spoken(spoken);
                            }
                        }
                    } else if (!a->silent) {
                        nodelio_panel_ui_toast_defer("加载失败");
                    }
                    free(resp);
                }
                s_dsh_detail_busy = false;
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_dsh_det", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_dsh_detail_busy = false;
        free(arg);
        if (!silent) {
            nl_ov_toast("无法打开详情");
        }
    }
}

/** 从列表行打开详情。 */
static void dsh_fetch_session_detail(int track_idx)
{
    if (!s_have_env || !s_env || track_idx < 0 || track_idx >= s_env->track_n) {
        return;
    }
    char ws[200] = {0};
    char sess[80] = {0};
    dsh_extract_arm(&s_env->tracks[track_idx], ws, sizeof(ws), sess, sizeof(sess));
    dsh_fetch_detail(ws, sess, false);
}

/** user_data = (track_idx << 8) | action_idx；走 Host invoke，禁止直连桥。 */
static void run_invoke_action(const nl_env_invoke_action_t *ia)
{
    if (!ia) {
        return;
    }
    /* DSH 派活/续聊：本地武装，不 HTTP（等语音「让它…」） */
    if (ia->params_json[0] && strstr(ia->params_json, "\"arm\"")) {
        char ws_buf[200] = {0};
        char sess_buf[80] = {0};
        cJSON *root = cJSON_Parse(ia->params_json);
        if (root) {
            const cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "workspace");
            const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "session");
            if (cJSON_IsString(jw) && jw->valuestring) {
                snprintf(ws_buf, sizeof(ws_buf), "%s", jw->valuestring);
            }
            if (cJSON_IsString(js) && js->valuestring) {
                snprintf(sess_buf, sizeof(sess_buf), "%s", js->valuestring);
            }
            cJSON_Delete(root);
        }
        nodelio_panel_ui_arm_dsh(ws_buf[0] ? ws_buf : nullptr, sess_buf[0] ? sess_buf : nullptr);
        /* 续聊：只 toast + 开麦。禁止 announce_spoken——纯文字会 EndVoiceSession，立刻把刚开的麦掐掉 */
        const bool card_open =
            s_skill_ui_active && s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (sess_buf[0]) {
            nl_ov_toast(card_open ? "请直接说要做的事" : "已续聊，请直接说要做的事");
            dsh_enter_armed(ws_buf, sess_buf);
        } else if (ws_buf[0]) {
            nl_ov_toast("已选工作区，请直接说");
            dsh_enter_armed(ws_buf, nullptr);
        } else {
            nl_ov_toast("请直接说要做的事");
        }
        /* 卡已开：留在详情/列表开麦。minimize 会清 skill_ui_active，再 show_chat 会弹 AI 抽屉成双窗 */
        if (!card_open) {
            minimize_to_fab();
        }
        nodelio_overlay_show_chat();
        return;
    }
    const char *skill = ia->instance_id[0] ? ia->instance_id : ia->app_id;
    if (!skill[0] || !s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("无法执行");
        return;
    }
    struct invoke_arg {
        nodelio_nvs_config_t cfg;
        char skill_id[NL_ENV_STR];
        char body[NL_ENV_PARAMS];
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        nl_ov_toast("内存不足");
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->skill_id, sizeof(arg->skill_id), "%s", skill);
    snprintf(arg->body, sizeof(arg->body), "%s", ia->params_json[0] ? ia->params_json : "{}");
    nl_ov_toast(ia->title[0] ? ia->title : "执行中…");
    constexpr uint32_t kStackWords = 12288 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<invoke_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, a->cfg.device_id,
                         a->skill_id);
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
                    esp_err_t err =
                        nodelio_http_post_json_ms(url, a->cfg.device_key, a->body, &status, resp, cap - 1, 10000);
                    if (err == ESP_OK && resp[0] &&
                        (status == 200 || status == 201 ||
                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                        char spoken[NL_ENV_SPOKEN];
                        spoken[0] = '\0';
                        nodelio_skill_deliver_json(&a->cfg, resp, false, spoken, sizeof(spoken));
                        if (strstr(a->skill_id, "dsh") != nullptr && spoken[0]) {
                            nodelio_panel_ui_announce_spoken(spoken);
                        }
                    } else {
                        nodelio_panel_ui_toast_defer("执行失败");
                    }
                    free(resp);
                }
                free(a);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_inv_act", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(arg);
        nl_ov_toast("无法执行");
    }
}

static void on_invoke_action(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    const int track_idx = (int)(packed >> 8);
    const int act_idx = (int)(packed & 0xff);
    if (track_idx < 0 || track_idx >= s_env->track_n || act_idx < 0 ||
        act_idx >= s_env->tracks[track_idx].invoke_n) {
        return;
    }
    run_invoke_action(&s_env->tracks[track_idx].invokes[act_idx]);
}

/** 「第一个 / 第2个 / 选三」→ 1-based；失败返回 0 */
static int parse_ordinal_zh(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return 0;
    }
    static const char *const kCn[] = {"一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十"};
    static const int kVal[] = {1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const char *p = strstr(phrase, "第");
    if (p) {
        p += 3; /* UTF-8 第 */
        if (p[0] >= '1' && p[0] <= '9') {
            int n = p[0] - '0';
            if (p[1] >= '0' && p[1] <= '9') {
                n = n * 10 + (p[1] - '0');
            }
            return n;
        }
        for (size_t i = 0; i < sizeof(kCn) / sizeof(kCn[0]); i++) {
            if (strncmp(p, kCn[i], strlen(kCn[i])) == 0) {
                return kVal[i];
            }
        }
    }
    /* 选一 / 要第二个 */
    for (size_t i = 0; i < sizeof(kCn) / sizeof(kCn[0]); i++) {
        if (strstr(phrase, kCn[i]) &&
            (strstr(phrase, "选") || strstr(phrase, "要") || strstr(phrase, "打开") ||
             strstr(phrase, "进"))) {
            return kVal[i];
        }
    }
    return 0;
}

static void invoke_filter_workspace(const nl_env_filter_t *f)
{
    if (!f || !f->workspace[0] || !s_env) {
        return;
    }
    nodelio_panel_ui_arm_dsh(f->workspace, nullptr);
    nl_env_invoke_action_t ia;
    memset(&ia, 0, sizeof(ia));
    snprintf(ia.id, sizeof(ia.id), "filter");
    snprintf(ia.title, sizeof(ia.title), "%s", f->title[0] ? f->title : "工作区");
    snprintf(ia.app_id, sizeof(ia.app_id), "%s",
             s_env->app_id[0] ? s_env->app_id : "dsh.status");
    /* 路径转义后须装进 params_json(256)：外壳 {"workspace":""} = 16+NUL → esc≤239 */
    char esc[240];
    size_t eo = 0;
    for (const char *p = f->workspace; *p && eo + 2 < sizeof(esc); ++p) {
        if (*p == '\\' || *p == '"') {
            esc[eo++] = '\\';
        }
        esc[eo++] = *p;
    }
    esc[eo] = '\0';
    snprintf(ia.params_json, sizeof(ia.params_json), "{\"workspace\":\"%s\"}", esc);
    run_invoke_action(&ia);
}

static void on_filter_chip(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_env) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_env->filter_n) {
        return;
    }
    const nl_env_filter_t *f = &s_env->filters[idx];
    if (f->selected) {
        return;
    }
    invoke_filter_workspace(f);
}

static void fill_filters(void)
{
    if (!s_filter_bar) {
        return;
    }
    lv_obj_clean(s_filter_bar);
    if (!s_env || s_env->filter_n <= 0) {
        lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_filter_bar, 0);
        return;
    }
    lv_obj_clear_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_filter_bar, 40);
    for (int i = 0; i < s_env->filter_n; i++) {
        const nl_env_filter_t *f = &s_env->filters[i];
        lv_obj_t *chip = lv_obj_create(s_filter_bar);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, 36);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 14, 0);
        lv_obj_set_style_radius(chip, 18, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (f->selected) {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_ON, 0);
            lv_obj_set_style_border_color(chip, UI_ACCENT_SOFT, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_80, 0);
        } else {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_border_color(chip, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_60, 0);
        }
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, 8);
        lv_obj_add_event_cb(chip, on_filter_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(chip);
        nl_ov_set_font(lbl);
        lv_label_set_text(lbl, f->title[0] ? f->title : "?");
        lv_obj_set_style_text_color(lbl, f->selected ? UI_TEXT : UI_TEXT_SUB, 0);
    }
    /* 滚到选中项 */
    for (int i = 0; i < s_env->filter_n; i++) {
        if (s_env->filters[i].selected) {
            lv_obj_t *ch = lv_obj_get_child(s_filter_bar, i);
            if (ch) {
                lv_obj_scroll_to_view(ch, LV_ANIM_OFF);
            }
            break;
        }
    }
}

bool nodelio_skill_try_pick_phrase(const char *text)
{
    if (!text || !text[0] || !skill_ui_is_up() || !s_env) {
        return false;
    }
    if (strstr(s_env->app_id, "dsh") == nullptr) {
        return false;
    }
    const int ord = parse_ordinal_zh(text);
    if (ord <= 0) {
        return false;
    }
    /* 「第 N 个工作区」→ 顶栏 filters */
    if (s_env->filter_n > 0 && strstr(text, "工作区") != nullptr) {
        if (ord > s_env->filter_n) {
            nl_ov_toast("超出范围");
            nodelio_panel_ui_announce_spoken("没有这个工作区序号，请再说。");
            return true;
        }
        const nl_env_filter_t *f = &s_env->filters[ord - 1];
        ESP_LOGI(TAG, "dsh pick workspace ord=%d title=%s", ord, f->title);
        invoke_filter_workspace(f);
        return true;
    }
    /* 会话列表按播报序号 */
    int selectable[24];
    int n = 0;
    for (int i = 0; i < s_env->track_n && n < 24; i++) {
        if (strcmp(s_env->tracks[i].id, "__workspaces__") == 0) {
            continue;
        }
        selectable[n++] = i;
    }
    if (n <= 0) {
        return false;
    }
    if (ord > n) {
        nl_ov_toast("超出范围");
        nodelio_panel_ui_announce_spoken("没有这个序号，请再说第几个。");
        return true;
    }
    const int ti = selectable[ord - 1];
    const nl_env_track_t *tr = &s_env->tracks[ti];
    if (tr->invoke_n <= 0) {
        nl_ov_toast("该项不可选");
        return true;
    }
    /* 会话优先「续聊」(arm)；否则第一个 action */
    int prefer = 0;
    for (int ai = 0; ai < tr->invoke_n; ai++) {
        if (strstr(tr->invokes[ai].title, "续聊") != nullptr) {
            prefer = ai;
            break;
        }
    }
    ESP_LOGI(TAG, "dsh pick ord=%d track=%d action=%s", ord, ti, tr->invokes[prefer].title);
    run_invoke_action(&tr->invokes[prefer]);
    return true;
}

static lv_obj_t *make_text_action_btn(lv_obj_t *parent, const char *title, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 10);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    nl_ov_set_font(lbl);
    lv_label_set_text(lbl, title && title[0] ? title : "执行");
    lv_obj_set_style_text_color(lbl, UI_BG, 0);
    lv_obj_center(lbl);
    return btn;
}

static void on_stop_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    nodelio_media_stop();
    /* 播放条和曲库一起退下，动画结束后释放对象 */
    dismiss_library_and_player();
}

static void on_pause_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (nodelio_media_is_paused()) {
        nodelio_media_resume();
    } else {
        nodelio_media_pause();
    }
}

static void on_prev_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!nodelio_skill_media_prev()) {
        nl_ov_toast("没有上一首");
    }
}

static void on_next_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!nodelio_skill_media_next()) {
        nl_ov_toast("没有下一首");
    }
}

static void set_pause_btn_icon(bool playing)
{
    if (!s_now_pause_btn) {
        return;
    }
    lv_obj_t *ic = lv_obj_get_child(s_now_pause_btn, 0);
    if (ic) {
        lv_label_set_text(ic, playing ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW);
    }
}

bool nodelio_skill_media_prev(void)
{
    int idx = neighbor_playable(s_play_idx, -1);
    if (idx < 0) {
        return false;
    }
    play_at(idx);
    return true;
}

bool nodelio_skill_media_next(void)
{
    int idx = neighbor_playable(s_play_idx, +1);
    if (idx < 0) {
        return false;
    }
    play_at(idx);
    return true;
}

bool nodelio_skill_media_play_any(char *title_out, size_t title_out_len)
{
    if (title_out && title_out_len > 0) {
        title_out[0] = '\0';
    }
    if (!s_have_env || s_env->track_n <= 0) {
        return false;
    }
    int idxs[NL_ENV_TRACK_MAX];
    int n = 0;
    for (int i = 0; i < s_env->track_n && n < NL_ENV_TRACK_MAX; i++) {
        if (s_env->tracks[i].playback_url[0]) {
            idxs[n++] = i;
        }
    }
    if (n <= 0) {
        return false;
    }
    int slot = (int)(esp_random() % (uint32_t)n);
    if (n > 1 && idxs[slot] == s_play_idx) {
        slot = (slot + 1) % n;
    }
    const int pick = idxs[slot];
    play_at(pick);
    if (title_out && title_out_len > 0 && pick >= 0 && pick < s_env->track_n) {
        snprintf(title_out, title_out_len, "%s", s_env->tracks[pick].title);
    }
    return true;
}

/** 新结果仅 1 首且可在旧列表中找到 → 保留旧列表只切播，避免「随便」再搜把歌单冲成 1 首。 */
static int find_track_in_env(const nl_content_envelope_t *hay, const nl_env_track_t *needle)
{
    if (!hay || !needle || hay->track_n <= 0) {
        return -1;
    }
    if (needle->playback_url[0]) {
        for (int i = 0; i < hay->track_n; i++) {
            if (hay->tracks[i].playback_url[0] &&
                strcmp(hay->tracks[i].playback_url, needle->playback_url) == 0) {
                return i;
            }
        }
    }
    if (needle->title[0]) {
        for (int i = 0; i < hay->track_n; i++) {
            if (strcmp(hay->tracks[i].title, needle->title) != 0) {
                continue;
            }
            if (needle->artist[0] && hay->tracks[i].artist[0] &&
                strcmp(hay->tracks[i].artist, needle->artist) != 0) {
                continue;
            }
            return i;
        }
    }
    return -1;
}

/** Material Symbols 图标钮（勿用 nl_ov_set_font：普惠字库无符号字形） */
static lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym,
                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, SKILL_CTRL_SZ, SKILL_CTRL_SZ);
    lv_obj_set_style_radius(btn, SKILL_CTRL_SZ / 2, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, fg, 0);
    lv_obj_center(ic);
    return btn;
}

/** 播放控制专用大图标钮（提高点击命中率） */
static lv_obj_t *make_play_ctrl_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym,
                                    lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 12);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, fg, 0);
    lv_obj_center(ic);
    return btn;
}

static void anim_eq_height(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_border_opa(void *var, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_obj_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_obj_ty(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

/** from_below=true：自下方滑入（禁止负 translate 盖住顶栏）。 */
static void anim_show_obj(lv_obj_t *obj, bool from_below)
{
    if (!obj) {
        return;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    if (from_below) {
        lv_obj_set_style_translate_y(obj, SKILL_ANIM_SLIDE, 0);
    } else {
        lv_obj_set_style_translate_y(obj, 0, 0);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_obj_opa);
    lv_anim_start(&a);

    if (from_below) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, obj);
        lv_anim_set_values(&b, SKILL_ANIM_SLIDE, 0);
        lv_anim_set_time(&b, SKILL_ANIM_MS);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, anim_obj_ty);
        lv_anim_start(&b);
    }
}

static void null_skill_widget_ptrs(void)
{
    s_scrim = nullptr;
    s_card = nullptr;
    s_filter_bar = nullptr;
    s_dsh_status = nullptr;
    s_dsh_status_lbl = nullptr;
    for (int i = 0; i < 3; i++) {
        s_dsh_eq[i] = nullptr;
    }
    s_dsh_detail_bar = nullptr;
    s_card_list = nullptr;
    s_card_title = nullptr;
    s_card_sub = nullptr;
    s_now_bar = nullptr;
    s_now_eq_wrap = nullptr;
    for (int i = 0; i < SKILL_EQ_N; i++) {
        s_now_eq[i] = nullptr;
    }
    s_now_lbl = nullptr;
    s_now_sub = nullptr;
    s_now_btns = nullptr;
    s_now_pause_btn = nullptr;
    s_mini_fab = nullptr;
    s_mini_ic = nullptr;
    s_news_body_lbl = nullptr;
    s_dsh_focus_row = nullptr;
    s_dsh_eq_anim_on = false;
    s_dsh_row_pulse_on = false;
    memset(s_row_idx_labels, 0, sizeof(s_row_idx_labels));
    memset(s_row_btns, 0, sizeof(s_row_btns));
}

static void free_skill_widgets(void)
{
    stop_dsh_status_anims();
    stop_now_anims();
    stop_mini_pulse();
    lv_obj_t *scrim = s_scrim;
    lv_obj_t *card = s_card;
    lv_obj_t *bar = s_now_bar;
    lv_obj_t *fab = s_mini_fab;
    if (scrim) {
        lv_anim_delete(scrim, anim_obj_opa);
        lv_anim_delete(scrim, anim_obj_ty);
    }
    if (card) {
        lv_anim_delete(card, anim_obj_opa);
        lv_anim_delete(card, anim_obj_ty);
    }
    if (bar) {
        lv_anim_delete(bar, anim_obj_opa);
        lv_anim_delete(bar, anim_obj_ty);
        lv_anim_delete(bar, anim_border_opa);
    }
    if (fab) {
        lv_anim_delete(fab, anim_obj_opa);
        lv_anim_delete(fab, anim_border_opa);
    }
    null_skill_widget_ptrs();
    s_ui_closing = false;
    s_have_env = false;
    memset(&s_env, 0, sizeof(s_env));
    s_play_idx = -1;
    s_minimized = false;
    s_news_detail = -1;
    s_news_gen += 1;
    if (s_news_body_heap) {
        heap_caps_free(s_news_body_heap);
        s_news_body_heap = nullptr;
    }
    if (scrim) {
        lv_obj_delete(scrim);
    }
    if (card) {
        lv_obj_delete(card);
    }
    if (bar) {
        lv_obj_delete(bar);
    }
    if (fab) {
        lv_obj_delete(fab);
    }
    set_drawer_for_skill(false);
    ensure_chrome_order();
}

static void release_async(void *p)
{
    uint32_t gen = (uint32_t)(uintptr_t)p;
    if (gen != s_close_gen || !s_ui_closing) {
        return;
    }
    free_skill_widgets();
}

static void on_dismiss_done(lv_anim_t *a)
{
    if (!a) {
        return;
    }
    /* 完成回调里不能删正在做动画的对象，丢到下一拍 */
    lv_async_call(release_async, lv_anim_get_user_data(a));
}

static bool slide_out_obj(lv_obj_t *obj, bool with_done)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, obj);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade, anim_obj_opa);
    lv_anim_start(&fade);

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, obj);
    lv_anim_set_values(&slide, 0, 72);
    lv_anim_set_time(&slide, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide, anim_obj_ty);
    if (with_done) {
        lv_anim_set_completed_cb(&slide, on_dismiss_done);
        lv_anim_set_user_data(&slide, (void *)(uintptr_t)s_close_gen);
    }
    lv_anim_start(&slide);
    return true;
}

static void dismiss_library_and_player(void)
{
    s_close_gen += 1;
    s_ui_closing = true;
    s_have_env = false;
    memset(&s_env, 0, sizeof(s_env));
    s_play_idx = -1;
    s_minimized = false;
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    stop_now_anims();
    hide_mini_fab();
    set_drawer_for_skill(false);

    lv_obj_t *done_on = nullptr;
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_card;
    } else if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_now_bar;
    } else if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_scrim;
    }
    bool started = false;
    started = slide_out_obj(s_scrim, s_scrim && s_scrim == done_on) || started;
    started = slide_out_obj(s_now_bar, s_now_bar && s_now_bar == done_on) || started;
    started = slide_out_obj(s_card, s_card && s_card == done_on) || started;
    if (!started) {
        free_skill_widgets();
    }
}

static void reset_obj_anim_style(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

static void stop_mini_pulse(void)
{
    if (s_mini_pulse_on && s_mini_fab) {
        lv_anim_delete(s_mini_fab, anim_border_opa);
        s_mini_pulse_on = false;
        lv_obj_set_style_border_opa(s_mini_fab, SKILL_BORDER_OPA, 0);
    }
}

static void start_mini_pulse(void)
{
    if (!s_mini_fab) {
        return;
    }
    stop_mini_pulse();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mini_fab);
    lv_anim_set_values(&a, 90, 240);
    lv_anim_set_time(&a, 900);
    lv_anim_set_playback_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, anim_border_opa);
    lv_anim_start(&a);
    s_mini_pulse_on = true;
}

static void hide_mini_fab(void)
{
    stop_mini_pulse();
    if (s_mini_fab) {
        reset_obj_anim_style(s_mini_fab);
        lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_mini_fab(bool playing)
{
    if (!s_mini_fab) {
        return;
    }
    lv_obj_clear_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    if (s_mini_ic) {
        lv_label_set_text(s_mini_ic, playing ? MATERIAL_SYMBOLS_MUSIC_NOTE : MATERIAL_SYMBOLS_HEADPHONES);
    }
    anim_show_obj(s_mini_fab, false);
    if (playing) {
        start_mini_pulse();
    } else {
        stop_mini_pulse();
    }
    layout_skill_layers();
    raise_skill_layers();
}

static void minimize_to_fab(void)
{
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    s_minimized = true;
    reset_obj_anim_style(s_scrim);
    reset_obj_anim_style(s_card);
    reset_obj_anim_style(s_now_bar);
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_bar) {
        stop_now_anims();
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    }
    set_drawer_for_skill(false);
    show_mini_fab(nodelio_media_is_playing());
    ensure_chrome_order();
}

static void expand_from_fab(void)
{
    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }
    s_minimized = false;
    hide_mini_fab();

    const bool playing = nodelio_media_is_playing();
    const bool paused = nodelio_media_is_paused();
    const bool have_list = s_have_env && s_env->track_n > 0 && s_card;

    if (have_list) {
        set_drawer_for_skill(true);
        if (s_scrim) {
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            anim_show_obj(s_scrim, false);
        }
        if (s_card_title) {
            const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
            lv_label_set_text(s_card_title, title);
        }
        fill_list();
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        anim_show_obj(s_card, true);
    } else {
        set_drawer_for_skill(false);
    }

    if (s_now_bar && (playing || paused)) {
        if (s_now_sub) {
            if (paused) {
                lv_label_set_text(s_now_sub, "已暂停 · 点播放继续");
            } else {
                lv_label_set_text(s_now_sub, have_list ? "正在播放" : "点此打开列表");
            }
        }
        set_pause_btn_icon(playing);
        lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        if (playing) {
            start_now_anims();
        } else {
            stop_now_anims();
        }
        anim_show_obj(s_now_bar, true);
    }

    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
}

static void stop_now_anims(void)
{
    if (s_now_border_anim_on && s_now_bar) {
        lv_anim_delete(s_now_bar, anim_border_opa);
        s_now_border_anim_on = false;
        lv_obj_set_style_border_opa(s_now_bar, SKILL_BORDER_OPA, 0);
    }
    for (int i = 0; i < SKILL_EQ_N; i++) {
        if (s_now_eq[i]) {
            lv_anim_delete(s_now_eq[i], anim_eq_height);
            lv_obj_set_height(s_now_eq[i], 8 + (i % 3) * 4);
        }
    }
}

static void start_now_anims(void)
{
    if (!s_now_bar) {
        return;
    }
    stop_now_anims();

    lv_anim_init(&s_now_border_anim);
    lv_anim_set_var(&s_now_border_anim, s_now_bar);
    lv_anim_set_values(&s_now_border_anim, 110, 240);
    lv_anim_set_time(&s_now_border_anim, 900);
    lv_anim_set_playback_time(&s_now_border_anim, 900);
    lv_anim_set_repeat_count(&s_now_border_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_now_border_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&s_now_border_anim, anim_border_opa);
    lv_anim_start(&s_now_border_anim);
    s_now_border_anim_on = true;

    static const int32_t k_lo[SKILL_EQ_N] = {6, 10, 8, 12};
    static const int32_t k_hi[SKILL_EQ_N] = {18, 24, 20, 22};
    static const uint32_t k_ms[SKILL_EQ_N] = {380, 460, 420, 500};
    for (int i = 0; i < SKILL_EQ_N; i++) {
        if (!s_now_eq[i]) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_now_eq[i]);
        lv_anim_set_values(&a, k_lo[i], k_hi[i]);
        lv_anim_set_time(&a, k_ms[i]);
        lv_anim_set_playback_time(&a, k_ms[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_eq_height);
        lv_anim_set_delay(&a, (uint32_t)(i * 70));
        lv_anim_start(&a);
    }
}

static void ensure_card(void)
{
    if (s_card) {
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    if (!parent) {
        return;
    }

    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_HOR_RES, LV_VER_RES - NL_UI_STATUS_H);
    lv_obj_align(s_scrim, LV_ALIGN_TOP_LEFT, 0, NL_UI_STATUS_H);
    lv_obj_set_style_bg_color(s_scrim, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_scrim, SKILL_SCRIM_OPA, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, on_scrim_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    s_card = lv_obj_create(parent);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, SKILL_CARD_W, SKILL_CARD_H_MAX);
    lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, NL_UI_STATUS_H + SKILL_GAP);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 18, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, UI_ACCENT_SOFT, 0);
    lv_obj_set_style_border_opa(s_card, SKILL_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(s_card, 18, 0);
    lv_obj_set_style_shadow_opa(s_card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_card, UI_BG, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_set_style_pad_row(s_card, 8, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hdr = lv_obj_create(s_card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hdr_txt = lv_obj_create(hdr);
    lv_obj_remove_style_all(hdr_txt);
    lv_obj_set_flex_grow(hdr_txt, 1);
    lv_obj_set_height(hdr_txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr_txt, 2, 0);

    s_card_title = lv_label_create(hdr_txt);
    nl_ov_set_font(s_card_title);
    lv_label_set_text(s_card_title, "曲库");
    lv_obj_set_style_text_color(s_card_title, UI_TEXT, 0);

    s_card_sub = lv_label_create(hdr_txt);
    nl_ov_set_font(s_card_sub);
    lv_label_set_text(s_card_sub, "");
    lv_obj_set_style_text_color(s_card_sub, UI_TEXT_MUTED, 0);

    lv_obj_t *hdr_btns = lv_obj_create(hdr);
    lv_obj_remove_style_all(hdr_btns);
    lv_obj_set_size(hdr_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(hdr_btns, 6, 0);
    lv_obj_clear_flag(hdr_btns, LV_OBJ_FLAG_CLICKABLE);

    make_play_ctrl_btn(hdr_btns, UI_CAPSULE_OFF, UI_ACCENT_SOFT, MATERIAL_SYMBOLS_REFRESH, on_reshuffle);
    /* 曲库为下一页；新闻在 fill_news 里改回下箭头收起 */
    s_hdr_mid_btn = make_play_ctrl_btn(hdr_btns, UI_CAPSULE_OFF, UI_TEXT_SUB, MATERIAL_SYMBOLS_SKIP_NEXT,
                                       on_hdr_mid);

    lv_obj_t *xbtn = lv_obj_create(hdr_btns);
    lv_obj_remove_style_all(xbtn);
    lv_obj_set_size(xbtn, 44, 44);
    lv_obj_set_style_radius(xbtn, 22, 0);
    lv_obj_set_style_bg_color(xbtn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(xbtn, LV_OPA_COVER, 0);
    lv_obj_add_flag(xbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(xbtn, 12);
    lv_obj_add_event_cb(xbtn, on_close_card, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_obj_set_style_text_font(xl, &font_material_symbols_20_4, 0);
    lv_label_set_text(xl, MATERIAL_SYMBOLS_CLOSE);
    lv_obj_set_style_text_color(xl, UI_TEXT_SUB, 0);
    lv_obj_center(xl);

    /* display.filters：横滑 chip（DSH 工作区等） */
    s_filter_bar = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_filter_bar);
    lv_obj_set_width(s_filter_bar, lv_pct(100));
    lv_obj_set_height(s_filter_bar, 0);
    lv_obj_set_flex_flow(s_filter_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_filter_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_filter_bar, 8, 0);
    lv_obj_set_style_pad_hor(s_filter_bar, 2, 0);
    lv_obj_set_scroll_dir(s_filter_bar, LV_DIR_HOR);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_filter_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);

    /* 续聊状态条：开麦/投递时展开，带 EQ 动画 */
    s_dsh_status = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_dsh_status);
    lv_obj_set_width(s_dsh_status, lv_pct(100));
    lv_obj_set_height(s_dsh_status, 0);
    lv_obj_set_style_radius(s_dsh_status, 10, 0);
    lv_obj_set_style_bg_color(s_dsh_status, UI_TILE_SWITCH, 0);
    lv_obj_set_style_bg_opa(s_dsh_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_dsh_status, 10, 0);
    lv_obj_set_style_pad_column(s_dsh_status, 8, 0);
    lv_obj_set_flex_flow(s_dsh_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dsh_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *eq_wrap = lv_obj_create(s_dsh_status);
    lv_obj_remove_style_all(eq_wrap);
    lv_obj_set_size(eq_wrap, 22, 22);
    lv_obj_set_flex_flow(eq_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_wrap, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(eq_wrap, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        s_dsh_eq[i] = lv_obj_create(eq_wrap);
        lv_obj_remove_style_all(s_dsh_eq[i]);
        lv_obj_set_size(s_dsh_eq[i], 3, 6 + i * 3);
        lv_obj_set_style_radius(s_dsh_eq[i], 2, 0);
        lv_obj_set_style_bg_color(s_dsh_eq[i], UI_ACCENT_SOFT, 0);
        lv_obj_set_style_bg_opa(s_dsh_eq[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dsh_eq[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_dsh_status_lbl = lv_label_create(s_dsh_status);
    nl_ov_set_font(s_dsh_status_lbl);
    lv_label_set_text(s_dsh_status_lbl, "");
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    lv_obj_set_flex_grow(s_dsh_status_lbl, 1);
    lv_label_set_long_mode(s_dsh_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_dsh_status_lbl, lv_pct(100));

    s_card_list = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_card_list);
    lv_obj_set_width(s_card_list, lv_pct(100));
    lv_obj_set_flex_grow(s_card_list, 1);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_card_list, 4, 0);
    lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);

    /* 详情底栏：续聊固定在窗口底部，不进滚动区 */
    s_dsh_detail_bar = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_dsh_detail_bar);
    lv_obj_set_width(s_dsh_detail_bar, lv_pct(100));
    lv_obj_set_height(s_dsh_detail_bar, 0);
    lv_obj_set_style_pad_top(s_dsh_detail_bar, 6, 0);
    lv_obj_set_style_pad_bottom(s_dsh_detail_bar, 2, 0);
    lv_obj_set_flex_flow(s_dsh_detail_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dsh_detail_bar, 8, 0);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);

    if (nodelio_caps_has_surface("now_playing")) {
        s_now_bar = lv_obj_create(parent);
        lv_obj_remove_style_all(s_now_bar);
        lv_obj_set_size(s_now_bar, SKILL_CARD_W, SKILL_NOW_H);
        lv_obj_align(s_now_bar, LV_ALIGN_TOP_MID, 0, NL_UI_STATUS_H + SKILL_GAP);
        lv_obj_set_style_bg_color(s_now_bar, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(s_now_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_now_bar, 16, 0);
        lv_obj_set_style_border_width(s_now_bar, 2, 0);
        lv_obj_set_style_border_color(s_now_bar, UI_ACCENT, 0);
        lv_obj_set_style_border_opa(s_now_bar, SKILL_BORDER_OPA, 0);
        lv_obj_set_style_shadow_width(s_now_bar, 16, 0);
        lv_obj_set_style_shadow_opa(s_now_bar, LV_OPA_30, 0);
        lv_obj_set_style_shadow_color(s_now_bar, UI_ACCENT, 0);
        lv_obj_set_style_pad_hor(s_now_bar, 10, 0);
        lv_obj_set_style_pad_column(s_now_bar, 8, 0);
        lv_obj_set_flex_flow(s_now_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);

        s_now_eq_wrap = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(s_now_eq_wrap);
        lv_obj_set_size(s_now_eq_wrap, 28, 28);
        lv_obj_set_style_radius(s_now_eq_wrap, 8, 0);
        lv_obj_set_style_bg_color(s_now_eq_wrap, UI_TILE_SWITCH, 0);
        lv_obj_set_style_bg_opa(s_now_eq_wrap, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(s_now_eq_wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_eq_wrap, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(s_now_eq_wrap, 4, 0);
        lv_obj_set_style_pad_bottom(s_now_eq_wrap, 4, 0);
        lv_obj_add_flag(s_now_eq_wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_eq_wrap, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        for (int i = 0; i < SKILL_EQ_N; i++) {
            s_now_eq[i] = lv_obj_create(s_now_eq_wrap);
            lv_obj_remove_style_all(s_now_eq[i]);
            lv_obj_set_size(s_now_eq[i], 3, 8 + (i % 3) * 4);
            lv_obj_set_style_radius(s_now_eq[i], 2, 0);
            lv_obj_set_style_bg_color(s_now_eq[i], UI_ACCENT_SOFT, 0);
            lv_obj_set_style_bg_opa(s_now_eq[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(s_now_eq[i], LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *meta = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(meta);
        lv_obj_set_flex_grow(meta, 1);
        lv_obj_set_height(meta, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(meta, 1, 0);
        lv_obj_add_flag(meta, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(meta, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_sub = lv_label_create(meta);
        nl_ov_set_font(s_now_sub);
        lv_label_set_text(s_now_sub, "点此打开列表");
        lv_obj_set_style_text_color(s_now_sub, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(s_now_sub, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_sub, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_lbl = lv_label_create(meta);
        nl_ov_set_font(s_now_lbl);
        lv_label_set_long_mode(s_now_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(s_now_lbl, SKILL_CARD_W - 200);
        lv_obj_set_style_text_color(s_now_lbl, UI_TEXT, 0);
        lv_obj_add_flag(s_now_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_lbl, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_btns = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(s_now_btns);
        lv_obj_set_size(s_now_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_now_btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_now_btns, 4, 0);
        lv_obj_clear_flag(s_now_btns, LV_OBJ_FLAG_CLICKABLE);

        make_play_ctrl_btn(s_now_btns, UI_CAPSULE_OFF, UI_TEXT, MATERIAL_SYMBOLS_SKIP_PREVIOUS, on_prev_now);
        s_now_pause_btn =
            make_play_ctrl_btn(s_now_btns, UI_CAPSULE_OFF, UI_TEXT, MATERIAL_SYMBOLS_PAUSE, on_pause_now);
        make_play_ctrl_btn(s_now_btns, UI_DANGER_BG, UI_DANGER_FG, MATERIAL_SYMBOLS_STOP, on_stop_now);
        make_play_ctrl_btn(s_now_btns, UI_CAPSULE_OFF, UI_TEXT, MATERIAL_SYMBOLS_SKIP_NEXT, on_next_now);
        make_icon_btn(s_now_btns, UI_CAPSULE_OFF, UI_TEXT_SUB, MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN,
                      on_minimize);
    }

    s_mini_fab = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mini_fab);
    lv_obj_set_size(s_mini_fab, SKILL_MINI_SZ, SKILL_MINI_SZ);
    lv_obj_align(s_mini_fab, LV_ALIGN_BOTTOM_RIGHT, -14, -(NL_UI_TAB_H + 14));
    lv_obj_set_style_radius(s_mini_fab, SKILL_MINI_SZ / 2, 0);
    lv_obj_set_style_bg_color(s_mini_fab, UI_TILE_SWITCH, 0);
    lv_obj_set_style_bg_opa(s_mini_fab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mini_fab, 2, 0);
    lv_obj_set_style_border_color(s_mini_fab, UI_ACCENT, 0);
    lv_obj_set_style_border_opa(s_mini_fab, SKILL_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(s_mini_fab, 14, 0);
    lv_obj_set_style_shadow_opa(s_mini_fab, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_mini_fab, UI_ACCENT, 0);
    lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mini_fab, 10);
    lv_obj_add_event_cb(s_mini_fab, on_mini_fab, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_mini_fab, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(s_mini_fab, 1, LV_STATE_PRESSED);

    s_mini_ic = lv_label_create(s_mini_fab);
    lv_obj_set_style_text_font(s_mini_ic, &font_material_symbols_20_4, 0);
    lv_label_set_text(s_mini_ic, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_set_style_text_color(s_mini_ic, UI_ACCENT_SOFT, 0);
    lv_obj_center(s_mini_ic);

    ensure_chrome_order();
}

#include "news_list_ui.inc"

static void apply_card_chrome(void)
{
    const bool music = strcmp(s_env->card_type, "music_list") == 0;
    const bool news = strcmp(s_env->card_type, "news_list") == 0;
    const bool dsh_detail =
        s_env->detail_body && s_env->detail_body[0] && strstr(s_env->app_id, "dsh") != nullptr;
    const bool paged = music || news || list_page_count() > 1;
    /* 有顶栏 chip 时勿 compact：否则列表 SIZE_CONTENT 被 max_h 裁切且难滚，第 4 行露不全 */
    const bool compact = !paged && !dsh_detail && s_env->filter_n == 0 && s_env->track_n <= 4;
    if (s_hdr_mid_btn) {
        /* DSH 详情：中间钮与左侧刷新易混，改用底栏「返回列表」；仅曲库/新闻分页显示 */
        if (dsh_detail) {
            lv_obj_add_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        } else if (paged) {
            lv_obj_clear_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_card) {
        return;
    }
    const int hmax = skill_card_h_max();
    if (compact) {
        lv_obj_set_height(s_card, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(s_card, hmax, 0);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 0);
            lv_obj_set_height(s_card_list, LV_SIZE_CONTENT);
        }
    } else {
        lv_obj_set_height(s_card, hmax);
        lv_obj_set_style_max_height(s_card, hmax, 0);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 1);
        }
    }
}

static bool dsh_subtitle_is_running(const char *sub)
{
    if (!sub || !sub[0]) {
        return false;
    }
    return strstr(sub, "正在用") != nullptr || strstr(sub, "正在思考") != nullptr ||
           strstr(sub, "执行中") != nullptr;
}

/** 详情页：执行中 → 顶状态条 EQ；空闲则按续聊 phase 画 */
static void dsh_detail_paint_running(const char *subtitle)
{
    const bool running =
        s_dsh_phase == DSH_UI_DISPATCHED || s_dsh_phase == DSH_UI_ARMED || dsh_subtitle_is_running(subtitle);
    if (!running) {
        if (s_dsh_phase == DSH_UI_IDLE) {
            stop_dsh_status_anims();
            if (s_dsh_status) {
                lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(s_dsh_status, 0);
            }
        } else {
            dsh_paint_status_strip();
        }
        return;
    }
    if (!s_dsh_status || !s_dsh_status_lbl) {
        return;
    }
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_dsh_status, 32);
    uint32_t pill = UI_C_TILE_PWM;
    uint32_t bar = UI_C_INFO;
    const char *txt = "执行中…";
    if (s_dsh_phase == DSH_UI_ARMED) {
        pill = UI_C_TILE_SWITCH;
        bar = UI_C_ACCENT_SOFT;
        txt = "续聊中 · 请直接说指令";
    } else if (s_dsh_phase == DSH_UI_DISPATCHED && s_dsh_last_instr[0]) {
        static char buf[120];
        snprintf(buf, sizeof(buf), "已投递 · %s", s_dsh_last_instr);
        txt = buf;
    } else if (subtitle && subtitle[0] && dsh_subtitle_is_running(subtitle)) {
        txt = subtitle;
    }
    lv_obj_set_style_bg_color(s_dsh_status, lv_color_hex(pill), 0);
    for (int i = 0; i < 3; i++) {
        if (s_dsh_eq[i]) {
            lv_obj_set_style_bg_color(s_dsh_eq[i], lv_color_hex(bar), 0);
        }
    }
    lv_label_set_text(s_dsh_status_lbl, txt);
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    if (s_card_sub) {
        lv_label_set_text(s_card_sub, s_dsh_phase == DSH_UI_ARMED ? "续聊中 · 直接说 · 底栏可返回"
                                                                  : "执行中 · 底栏可返回列表");
        lv_obj_set_style_text_color(s_card_sub, lv_color_hex(bar), 0);
    }
    start_dsh_status_anims();
}

/** DSH 会话详情：滚动纯文本；续聊固定底栏；执行中顶条 EQ */
static void fill_dsh_detail(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    /* 定时刷新重建时保留阅读位置，避免跳回顶 */
    const lv_coord_t keep_y =
        (s_news_body_lbl && s_card_list) ? lv_obj_get_scroll_y(s_card_list) : 0;
    lv_obj_clean(s_card_list);
    s_news_body_lbl = nullptr;
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(s_card_list, 8, 0);

    const char *artist =
        (s_env->track_n > 0 && s_env->tracks[0].artist[0]) ? s_env->tracks[0].artist : "";
    const bool running = dsh_subtitle_is_running(artist) || s_dsh_phase != DSH_UI_IDLE;

    if (s_card_sub) {
        lv_label_set_text(s_card_sub, running ? "执行中 · 底栏可返回列表"
                                              : "上滑阅读 · 底栏返回列表 / 续聊");
        lv_obj_set_style_text_color(s_card_sub, running ? UI_INFO : UI_TEXT_MUTED, 0);
    }

    if (s_env->track_n > 0 && s_env->tracks[0].title[0]) {
        lv_obj_t *ttl = lv_label_create(s_card_list);
        nl_ov_set_font(ttl);
        lv_label_set_text(ttl, s_env->tracks[0].title);
        lv_obj_set_width(ttl, lv_pct(100));
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(ttl, UI_TEXT, 0);
    }
    if (artist[0] && !dsh_subtitle_is_running(artist)) {
        /* 空闲字幕仍放正文上；执行中改走顶状态条，避免重复 */
        lv_obj_t *sub = lv_label_create(s_card_list);
        nl_ov_set_font(sub);
        lv_label_set_text(sub, artist);
        lv_obj_set_width(sub, lv_pct(100));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
    }

    s_news_body_lbl = lv_label_create(s_card_list);
    nl_ov_set_font(s_news_body_lbl);
    const char *body =
        (s_env->detail_body && s_env->detail_body[0]) ? s_env->detail_body : "（无正文）";
    lv_label_set_text(s_news_body_lbl, body);
    lv_obj_set_width(s_news_body_lbl, lv_pct(100));
    lv_label_set_long_mode(s_news_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_news_body_lbl, UI_TEXT, 0);
    lv_obj_set_style_pad_top(s_news_body_lbl, 8, 0);

    /* 底栏：返回列表 + 续聊（固定不滚） */
    if (s_dsh_detail_bar) {
        lv_obj_clean(s_dsh_detail_bar);
        lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t *back = make_text_action_btn(s_dsh_detail_bar, "返回列表", on_dsh_back_list, nullptr);
        if (back) {
            lv_obj_set_style_bg_color(back, UI_CAPSULE_OFF, 0);
            lv_obj_t *bl = lv_obj_get_child(back, 0);
            if (bl) {
                lv_obj_set_style_text_color(bl, UI_TEXT_SUB, 0);
            }
            lv_obj_set_size(back, LV_SIZE_CONTENT, 40);
            lv_obj_set_style_pad_hor(back, 14, 0);
        }
        if (s_env->track_n > 0) {
            for (int ai = 0; ai < s_env->tracks[0].invoke_n && ai < NL_ENV_ACTION_MAX; ai++) {
                const intptr_t packed = ((intptr_t)0 << 8) | (intptr_t)ai;
                const char *btn_title = s_env->tracks[0].invokes[ai].title;
                lv_color_t btn_bg = UI_ACCENT;
                if (strstr(btn_title, "续聊") != nullptr) {
                    if (s_dsh_phase == DSH_UI_ARMED) {
                        btn_title = "聆听中";
                        btn_bg = UI_WARN;
                    } else if (s_dsh_phase == DSH_UI_DISPATCHED || dsh_subtitle_is_running(artist)) {
                        btn_title = "执行中";
                        btn_bg = UI_INFO;
                    }
                }
                lv_obj_t *ab =
                    make_text_action_btn(s_dsh_detail_bar, btn_title, on_invoke_action, (void *)packed);
                if (ab) {
                    lv_obj_set_style_bg_color(ab, btn_bg, 0);
                    lv_obj_set_size(ab, LV_SIZE_CONTENT, 40);
                    lv_obj_set_style_pad_hor(ab, 16, 0);
                }
            }
        }
        lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 48);
    }

    dsh_detail_paint_running(artist);

    if (keep_y > 0) {
        lv_obj_update_layout(s_card_list);
        lv_obj_scroll_to_y(s_card_list, keep_y, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);
    }
    apply_card_chrome();
    start_dsh_detail_poll();
}

static void fill_list(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    fill_filters(); /* news 等无 filters 时收起横条 */
    if (strcmp(s_env->card_type, "news_list") == 0) {
        stop_dsh_detail_poll();
        if (s_dsh_detail_bar) {
            lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_detail_bar, 0);
        }
        fill_news();
        apply_card_chrome();
        return;
    }
    /* 带 body 的 DSH 详情卡 */
    if (s_env->detail_body && s_env->detail_body[0] && strstr(s_env->app_id, "dsh") != nullptr) {
        fill_dsh_detail();
        return;
    }
    stop_dsh_detail_poll();
    stop_dsh_status_anims();
    s_dsh_focus_row = nullptr;
    if (s_dsh_detail_bar) {
        lv_obj_clean(s_dsh_detail_bar);
        lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 0);
    }
    lv_obj_clean(s_card_list);
    /* 清空控件引用数组 */
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    const bool can_play = nodelio_caps_has_media();
    const bool comfortable = list_use_comfortable();
    /* 舒适单列允许指滑；紧凑双列仍分页、禁滚以免误触 */
    if (comfortable) {
        lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    } else {
        lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    }

    set_hdr_mid_icon(MATERIAL_SYMBOLS_SKIP_NEXT);

    const bool music_card = strcmp(s_env->card_type, "music_list") == 0;

    if (s_card_sub) {
        if (s_env->track_n > 0) {
            char sub[72];
            const int pages = list_page_count();
            if (s_list_page >= pages) {
                s_list_page = 0;
            }
            if (music_card) {
                if (pages > 1) {
                    snprintf(sub, sizeof(sub), "共 %d 首 · %d/%d", s_env->track_n, s_list_page + 1, pages);
                } else {
                    snprintf(sub, sizeof(sub), "共 %d 首 · 点播或说歌名", s_env->track_n);
                }
            } else if (pages > 1) {
                snprintf(sub, sizeof(sub), "共 %d 条 · %d/%d · 语音说指令派活", s_env->track_n,
                         s_list_page + 1, pages);
            } else if (strstr(s_env->app_id, "dsh") != nullptr) {
                if (s_dsh_phase == DSH_UI_ARMED) {
                    snprintf(sub, sizeof(sub), "续聊中 · 直接说指令");
                } else if (s_dsh_phase == DSH_UI_DISPATCHED) {
                    snprintf(sub, sizeof(sub), "已投递 · 点刷新看进度");
                } else {
                    snprintf(sub, sizeof(sub), "共 %d 条 · 点卡片看回复 · 「续聊」开麦", s_env->track_n);
                }
            } else {
                snprintf(sub, sizeof(sub), "共 %d 条", s_env->track_n);
            }
            lv_label_set_text(s_card_sub, sub);
        } else if (s_env->filter_n > 0 && strstr(s_env->app_id, "dsh") != nullptr) {
            lv_label_set_text(s_card_sub, "横滑选工作区 · 再说「让它…」");
        } else {
            lv_label_set_text(s_card_sub, "点关闭或点遮罩收起");
        }
    }

    if (strcmp(s_env->status, "ok") != 0) {
        lv_obj_t *err = lv_label_create(s_card_list);
        nl_ov_set_font(err);
        lv_label_set_text(err, s_env->error_message[0] ? s_env->error_message : "技能失败");
        lv_obj_set_width(err, lv_pct(100));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(err, UI_DANGER_FG, 0);
        apply_card_chrome();
        return;
    }

    if (s_env->track_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_card_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, s_env->spoken[0] ? s_env->spoken : "无结果");
        lv_obj_set_style_text_color(empty, UI_TEXT, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        apply_card_chrome();
        return;
    }

    const int pages = list_page_count();
    if (s_list_page < 0 || s_list_page >= pages) {
        s_list_page = 0;
    }
    const int cols = list_cols();
    const int page_sz = list_page_size();
    const int row_h = list_row_h();
    const int start = s_list_page * page_sz;
    int end = start + page_sz;
    if (end > s_env->track_n) {
        end = s_env->track_n;
    }
    for (int base = start; base < end; base += cols) {
        lv_obj_t *line = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, row_h);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, 6, 0);
        lv_obj_set_style_pad_row(line, 6, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        const int ncell = (base + cols - 1 < end) ? cols : (end - base);
        for (int c = 0; c < ncell; c++) {
            const int i = base + c;
            lv_obj_t *row = lv_obj_create(line);
            lv_obj_remove_style_all(row);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_height(row, row_h);
            lv_obj_set_style_min_width(row, 0, 0);
            lv_obj_set_style_bg_color(row, UI_FIELD, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 12, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
            lv_obj_set_style_pad_hor(row, comfortable ? 10 : 6, 0);
            lv_obj_set_style_pad_ver(row, comfortable ? 8 : 0, 0);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_set_style_clip_corner(row, true, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            /* 曲库：有 URL 点播；DSH：点行拉末轮全文/最新进度（无 playback_url 也要可点） */
            const bool dsh_detail_row =
                strstr(s_env->app_id, "dsh") != nullptr && !music_card &&
                !s_env->tracks[i].playback_url[0];
            if ((can_play && s_env->tracks[i].playback_url[0]) || dsh_detail_row) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_set_style_bg_color(row, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
            }

            lv_obj_t *idx_pod = lv_obj_create(row);
            lv_obj_remove_style_all(idx_pod);
            lv_obj_set_size(idx_pod, comfortable ? 26 : 22, comfortable ? 26 : 22);
            lv_obj_set_style_radius(idx_pod, 6, 0);
            lv_obj_set_style_bg_color(idx_pod, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(idx_pod, LV_OPA_COVER, 0);
            lv_obj_clear_flag(idx_pod, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *idx_l = lv_label_create(idx_pod);
            nl_ov_set_font(idx_l);
            if (i == s_play_idx) {
                lv_obj_set_style_text_font(idx_l, &font_material_symbols_20_4, 0);
                lv_label_set_text(idx_l, MATERIAL_SYMBOLS_HEADPHONES);
                lv_obj_set_style_text_color(idx_l, UI_ACCENT, 0);
            } else {
                char idx_buf[16];
                snprintf(idx_buf, sizeof(idx_buf), "%d", i + 1);
                lv_label_set_text(idx_l, idx_buf);
                lv_obj_set_style_text_color(idx_l, UI_ACCENT_SOFT, 0);
            }
            lv_obj_center(idx_l);
            if (i < 64) {
                s_row_idx_labels[i] = idx_l;
            }

            lv_obj_t *txt = lv_obj_create(row);
            lv_obj_remove_style_all(txt);
            lv_obj_set_flex_grow(txt, 1);
            lv_obj_set_height(txt, LV_SIZE_CONTENT);
            lv_obj_set_style_min_width(txt, 0, 0);
            lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(txt, comfortable ? 3 : 1, 0);
            lv_obj_set_style_clip_corner(txt, true, 0);
            lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *t = lv_label_create(txt);
            nl_ov_set_font(t);
            lv_label_set_text(t, s_env->tracks[i].title);
            lv_obj_set_style_text_color(t, UI_TEXT, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, lv_pct(100));
            const lv_font_t *row_font = lv_obj_get_style_text_font(t, LV_PART_MAIN);
            const int32_t line_h = row_font ? lv_font_get_line_height(row_font) : 16;
            lv_obj_set_height(t, line_h);

            lv_obj_t *a = lv_label_create(txt);
            nl_ov_set_font(a);
            if (s_env->tracks[i].artist[0]) {
                lv_label_set_text(a, s_env->tracks[i].artist);
            } else if (music_card) {
                lv_label_set_text(a, "未知艺人");
            } else {
                lv_label_set_text(a, " ");
            }
            lv_obj_set_style_text_color(a, UI_TEXT_MUTED, 0);
            lv_obj_set_width(a, lv_pct(100));
            if (comfortable) {
                lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
                lv_obj_set_height(a, line_h * 2);
            } else {
                lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
                lv_obj_set_height(a, line_h);
            }

            if (can_play && s_env->tracks[i].playback_url[0]) {
                const char *icon = (i == s_play_idx) ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW;
                lv_obj_t *btn = make_icon_btn(row, UI_ACCENT, UI_BG, icon, nullptr);
                lv_obj_add_event_cb(btn, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                if (i < 64) {
                    s_row_btns[i] = btn;
                }
            }
            /* Protocol run:invoke（如 DSH 允许/拒绝）；最多 2 个 */
            for (int ai = 0; ai < s_env->tracks[i].invoke_n && ai < NL_ENV_ACTION_MAX; ai++) {
                const intptr_t packed = ((intptr_t)i << 8) | (intptr_t)ai;
                const char *btn_title = s_env->tracks[i].invokes[ai].title;
                bool restyle = false;
                lv_color_t btn_bg = UI_ACCENT;
                if (dsh_track_is_focus(i) && strstr(btn_title, "续聊") != nullptr) {
                    if (s_dsh_phase == DSH_UI_ARMED) {
                        btn_title = "聆听中";
                        btn_bg = UI_WARN;
                        restyle = true;
                    } else if (s_dsh_phase == DSH_UI_DISPATCHED) {
                        btn_title = "已投递";
                        btn_bg = UI_INFO;
                        restyle = true;
                    }
                }
                lv_obj_t *ab = make_text_action_btn(row, btn_title, on_invoke_action, (void *)packed);
                if (ab && restyle) {
                    lv_obj_set_style_bg_color(ab, btn_bg, 0);
                }
            }
            dsh_style_focus_row(row, i, idx_pod, a);
        }
        if (ncell < cols) {
            lv_obj_t *pad = lv_obj_create(line);
            lv_obj_remove_style_all(pad);
            lv_obj_set_flex_grow(pad, 1);
            lv_obj_set_height(pad, row_h);
            lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (strstr(s_env->app_id, "dsh") != nullptr) {
        dsh_paint_status_strip();
    } else if (s_dsh_phase != DSH_UI_IDLE) {
        dsh_clear_ui_phase();
    }
    apply_card_chrome();
}

static bool skill_ui_is_up(void)
{
    if (s_have_env) {
        return true;
    }
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        return true;
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    return false;
}

bool nodelio_skill_result_is_visible(void)
{
    return skill_ui_is_up();
}

void nodelio_skill_close_all(void)
{
    nodelio_media_stop();
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    dismiss_library_and_player();
}

bool nodelio_skill_try_dismiss_phrase(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    const bool close = strstr(text, "关闭") || strstr(text, "关掉") || strstr(text, "关上") ||
                       strstr(text, "退出") || strstr(text, "收起");
    if (!close) {
        return false;
    }
    /* 控家对象留给离线控家口令 */
    if (strstr(text, "灯") || strstr(text, "继电器") || strstr(text, "插座") ||
        strstr(text, "窗帘") || strstr(text, "卷帘") || strstr(text, "情景") ||
        strstr(text, "控家") || strstr(text, "小智") || strstr(text, "省电")) {
        return false;
    }
    /* 点名关播放器/曲库：即使弹窗已收成播放条也关 */
    const bool named =
        strstr(text, "播放器") || strstr(text, "曲库") || strstr(text, "音乐") ||
        strstr(text, "电台") || strstr(text, "播客") || strstr(text, "新闻") ||
        strstr(text, "技能") || strstr(text, "应用");
    if (!named && !skill_ui_is_up()) {
        return false;
    }
    if (named && !skill_ui_is_up() && !nodelio_media_is_playing() && !nodelio_media_is_paused()) {
        /* 无弹窗也无在播：不当作已处理，留给其它口令 */
        return false;
    }
    /* 弹窗/播放条已在屏上：关闭电台/关掉/收起…一律停播并退下 */
    ESP_LOGI(TAG, "voice dismiss skill ui: %s", text);
    nodelio_skill_close_all();
    return true;
}

void nodelio_skill_result_hide(void)
{
    s_news_gen += 1;
    s_news_detail = -1;
    if (s_news_body_heap) {
        heap_caps_free(s_news_body_heap);
        s_news_body_heap = nullptr;
    }
    if (s_env && s_env->detail_body) {
        heap_caps_free(s_env->detail_body);
        s_env->detail_body = nullptr;
    }
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        s_dsh_phase = DSH_UI_IDLE;
        s_dsh_focus_sess[0] = '\0';
        s_dsh_focus_ws[0] = '\0';
        s_dsh_last_instr[0] = '\0';
        s_dsh_focus_row = nullptr;
        return;
    }
    dsh_clear_ui_phase();
    s_minimized = false;
    stop_now_anims();
    hide_mini_fab();
    reset_obj_anim_style(s_scrim);
    reset_obj_anim_style(s_card);
    reset_obj_anim_style(s_now_bar);
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_sub && s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(s_now_sub, "点此打开列表");
    }
    set_drawer_for_skill(false);
    /* 保留歌单：hide 只收起卡片；点播放条仍可 reopen_list */
    if (s_env->track_n <= 0) {
        s_have_env = false;
    }
    ensure_chrome_order();
}

void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, const nl_content_envelope_t *env,
                               bool auto_play_first)
{
    if (!env) {
        return;
    }
    /* 弹卡即算用户活动，避免随后 goodbye 息屏把列表盖掉 */
    Board::GetInstance().NoteUserActivity();
    if (cfg) {
        s_skill_cfg = *cfg;
    }
    /* 列表已开且新包只有 1 首要播：若该曲已在旧列表 → 不替换歌单，只 play_at */
    if (auto_play_first && env->auto_play && env->track_n == 1 && s_have_env && s_env &&
        s_env->track_n > 1 && strcmp(s_env->card_type, "music_list") == 0 &&
        (strcmp(env->card_type, "music_list") == 0 || env->card_type[0] == '\0')) {
        int hit = find_track_in_env(s_env, &env->tracks[0]);
        if (hit >= 0 && env->tracks[0].playback_url[0]) {
            ESP_LOGI(TAG, "keep list n=%d play hit=%d title=%s", s_env->track_n, hit,
                     env->tracks[0].title);
            {
                const int ps = list_page_size();
                if (ps > 0) {
                    s_list_page = hit / ps;
                }
            }
            play_at(hit);
            DisplayLockGuard lock(nl_ov_disp());
            if (lock && s_ready && s_card) {
                fill_list();
                layout_skill_layers();
                raise_skill_layers();
            }
            return;
        }
    }
    char keep_url[NL_ENV_URL];
    keep_url[0] = '\0';
    if (s_play_idx >= 0 && s_play_idx < s_env->track_n && s_env->tracks[s_play_idx].playback_url[0]) {
        snprintf(keep_url, sizeof(keep_url), "%s", s_env->tracks[s_play_idx].playback_url);
    }
    if (!ensure_skill_env()) {
        nl_ov_toast("内存不足");
        return;
    }
    /* detail_body 深拷：调用方随后会释放解析侧指针 */
    char *owned_detail = nullptr;
    if (env->detail_body && env->detail_body[0]) {
        const size_t n = strnlen(env->detail_body, 6000);
        owned_detail = static_cast<char *>(heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!owned_detail) {
            owned_detail = static_cast<char *>(malloc(n + 1));
        }
        if (owned_detail) {
            memcpy(owned_detail, env->detail_body, n);
            owned_detail[n] = '\0';
        }
    }
    if (s_env->detail_body) {
        heap_caps_free(s_env->detail_body);
        s_env->detail_body = nullptr;
    }
    *s_env = *env;
    s_env->detail_body = owned_detail;
    s_have_env = true;
    /* 新 Envelope 覆盖续聊乐观态（LVGL 清动画放进锁内） */
    const bool clear_dsh_ui = strstr(s_env->app_id, "dsh") != nullptr;
    if (clear_dsh_ui) {
        s_dsh_phase = DSH_UI_IDLE;
        s_dsh_focus_sess[0] = '\0';
        s_dsh_focus_ws[0] = '\0';
        s_dsh_last_instr[0] = '\0';
        s_dsh_focus_row = nullptr;
    }
    s_list_page = 0;
    s_play_idx = -1;
    if (keep_url[0]) {
        for (int i = 0; i < s_env->track_n; i++) {
            if (strcmp(s_env->tracks[i].playback_url, keep_url) == 0) {
                s_play_idx = i;
                {
                    const int ps = list_page_size();
                    if (ps > 0) {
                        s_list_page = i / ps;
                    }
                }
                break;
            }
        }
    }

    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }

    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    /* 卡已开：刷新/派活回包只换内容，禁止再跑入场淡入（否则像关了又弹） */
    const bool card_already_open =
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_minimized &&
        s_skill_ui_active && !s_ui_closing;

    if (s_ui_closing) {
        s_close_gen += 1;
        s_ui_closing = false;
        reset_obj_anim_style(s_scrim);
        reset_obj_anim_style(s_card);
        reset_obj_anim_style(s_now_bar);
    }
    ensure_card();
    if (!s_card) {
        return;
    }
    if (clear_dsh_ui) {
        stop_dsh_status_anims();
        if (s_dsh_status) {
            lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_status, 0);
        }
    }

    ensure_chrome_order();

    if (!s_news_screen_hook) {
        nodelio_panel_ui_set_screen_cb(news_on_screen);
        s_news_screen_hook = true;
    }
    s_news_detail = -1;
    const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
    if (strcmp(s_env->card_type, "news_list") == 0 && !s_env->display_title[0]) {
        title = "最新新闻";
    }
    lv_label_set_text(s_card_title, title);
    fill_list();

    s_minimized = false;
    hide_mini_fab();
    set_drawer_for_skill(true);

    if (s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        if (card_already_open) {
            reset_obj_anim_style(s_scrim);
        } else {
            anim_show_obj(s_scrim, false);
        }
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    if (card_already_open) {
        reset_obj_anim_style(s_card);
    } else {
        anim_show_obj(s_card, true);
    }
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();

    clear_ttl();
    if (s_env->ttl_sec > 0) {
        s_ttl_tmr = lv_timer_create(on_ttl, (uint32_t)s_env->ttl_sec * 1000, nullptr);
        if (s_ttl_tmr) {
            lv_timer_set_repeat_count(s_ttl_tmr, 1);
        }
    }

    if (auto_play_first && nodelio_caps_has_media() && s_env->track_n > 0 &&
        s_env->tracks[0].playback_url[0]) {
        ESP_LOGI(TAG, "auto play #0 title=%s", s_env->tracks[0].title);
        play_at(0);
    } else if (auto_play_first) {
        ESP_LOGW(TAG, "auto play skipped caps=%d tracks=%d url0=%d",
                 nodelio_caps_has_media() ? 1 : 0, s_env->track_n,
                 (s_env->track_n > 0 && s_env->tracks[0].playback_url[0]) ? 1 : 0);
    }

    ESP_LOGI(TAG, "show card_type=%s tracks=%d news=%d auto_play=%d", s_env->card_type, s_env->track_n,
             s_env->news_n, auto_play_first ? 1 : 0);
}

void nodelio_panel_ui_media_now_playing(const char *title, bool playing)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    if (s_ui_closing && (!title || !title[0])) {
        return;
    }
    if (s_ui_closing) {
        s_close_gen += 1;
        s_ui_closing = false;
        reset_obj_anim_style(s_scrim);
        reset_obj_anim_style(s_card);
        reset_obj_anim_style(s_now_bar);
    }
    if (!nodelio_caps_has_surface("now_playing") || !s_now_bar) {
        return;
    }
    /* 无标题 → 关闭播放器 */
    if (!title || !title[0]) {
        stop_now_anims();
        set_pause_btn_icon(true);
        clear_play_mark();
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_minimized) {
            if (s_have_env && s_env->track_n > 0) {
                show_mini_fab(false);
            } else {
                s_minimized = false;
                hide_mini_fab();
            }
        }
        layout_skill_layers();
        ensure_chrome_order();
        return;
    }

    lv_label_set_text(s_now_lbl, title);
    set_pause_btn_icon(playing);

    if (s_minimized) {
        show_mini_fab(playing);
        layout_skill_layers();
        ensure_chrome_order();
        return;
    }

    if (s_now_sub) {
        const bool list_open = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (playing) {
            lv_label_set_text(s_now_sub, list_open ? "正在播放" : "点此打开列表");
        } else {
            lv_label_set_text(s_now_sub, "已暂停 · 点播放继续");
        }
    }
    const bool was_hidden = lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    if (playing) {
        start_now_anims();
    } else {
        stop_now_anims();
    }
    if (was_hidden) {
        anim_show_obj(s_now_bar, true);
    }
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_scrim);
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_card);
    }
    lv_obj_move_foreground(s_now_bar);
    layout_skill_layers();
    ensure_chrome_order();
}

void nodelio_skill_dsh_on_dispatched(const char *instruction)
{
    if (s_dsh_phase == DSH_UI_IDLE && !s_dsh_focus_sess[0]) {
        char ws[200] = {0};
        char sess[80] = {0};
        nodelio_panel_ui_get_dsh_arm(ws, sizeof(ws), sess, sizeof(sess));
        if (sess[0]) {
            snprintf(s_dsh_focus_sess, sizeof(s_dsh_focus_sess), "%s", sess);
        }
        if (ws[0]) {
            snprintf(s_dsh_focus_ws, sizeof(s_dsh_focus_ws), "%s", ws);
        }
    }
    /* 无续聊焦点时仍 toast，避免「说了没反应」 */
    s_dsh_phase = DSH_UI_DISPATCHED;
    s_dsh_last_instr[0] = '\0';
    if (instruction && instruction[0]) {
        snprintf(s_dsh_last_instr, sizeof(s_dsh_last_instr), "%s", instruction);
        const size_t n = strlen(s_dsh_last_instr);
        if (n > 36) {
            s_dsh_last_instr[33] = '.';
            s_dsh_last_instr[34] = '.';
            s_dsh_last_instr[35] = '.';
            s_dsh_last_instr[36] = '\0';
        }
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_toast("已投递，执行中…");
    const bool card_open =
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && s_skill_ui_active;
    if (card_open && s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr && s_card_list) {
        fill_list(); /* 列表或详情都会重画底栏/状态条 */
    } else if (card_open) {
        dsh_paint_status_strip();
    }
}
