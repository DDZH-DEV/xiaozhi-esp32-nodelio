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
#define SKILL_CARD_H_MAX 360
#define SKILL_NOW_H 56
#define SKILL_GAP 10
#define SKILL_BORDER_OPA 200
#define SKILL_SCRIM_OPA 170
#define SKILL_EQ_N 4
#define SKILL_CTRL_SZ 34
#define SKILL_MINI_SZ 52
#define SKILL_ANIM_MS 240
#define SKILL_ANIM_SLIDE 22
#define SKILL_PAGE_COLS 2
#define SKILL_PAGE_ROWS 5
#define SKILL_PAGE_SIZE (SKILL_PAGE_COLS * SKILL_PAGE_ROWS)
#define NEWS_ARTICLE_CAP (20 * 1024)
/* MQTT MCP 回包总长易被对端掐断；约 900 汉字一段，多数文章 1～2 段。 */
#define NEWS_MCP_CHUNK 2800
/* 云端回源拉正文最多约 8s，默认 HTTP 读超时 2.5s 会在状态行到达前断开 */
#define NEWS_HTTP_IO_MS 12000
#define NEWS_TASK_STACK 10240

static lv_obj_t *s_scrim;
static lv_obj_t *s_card;
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
static lv_anim_t s_now_border_anim;
static bool s_now_border_anim_on;
static bool s_mini_pulse_on;
static bool s_minimized;
static bool s_ui_closing;
static uint32_t s_close_gen;
static nodelio_nvs_config_t s_skill_cfg;
static nl_content_envelope_t s_env;
static bool s_have_env;
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

static void layout_skill_layers(void)
{
    int y = NL_UI_STATUS_H + SKILL_GAP;
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align(s_now_bar, LV_ALIGN_TOP_MID, 0, y);
        y += SKILL_NOW_H + SKILL_GAP;
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
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

static void clear_ttl(void)
{
    if (s_ttl_tmr) {
        lv_timer_del(s_ttl_tmr);
    }
    s_ttl_tmr = nullptr;
}

static void on_ttl(lv_timer_t *t)
{
    (void)t;
    nodelio_skill_result_hide();
}

static void fill_list(void);
static void apply_card_chrome(void);
static void reopen_list(void);
static void request_reshuffle(void);
static void minimize_to_fab(void);
static void expand_from_fab(void);
static void anim_show_obj(lv_obj_t *obj, bool from_above);
static void reset_obj_anim_style(lv_obj_t *obj);
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
        (s_have_env && s_env.track_n > 0)) {
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

static void on_reshuffle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    request_reshuffle();
}

static int list_page_count(void)
{
    if (s_env.track_n <= 0) {
        return 1;
    }
    return (s_env.track_n + SKILL_PAGE_SIZE - 1) / SKILL_PAGE_SIZE;
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

/** 曲库翻页；新闻收成悬浮球。正文卡没有下一页，不提示曲目。 */
static void on_hdr_mid(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (strcmp(s_env.card_type, "news_list") == 0) {
        minimize_to_fab();
        return;
    }
    const bool music = strcmp(s_env.card_type, "music_list") == 0;
    const int pages = list_page_count();
    if (!music || pages <= 1) {
        return;
    }
    s_list_page = (s_list_page + 1) % pages;
    fill_list();
}

static void reopen_list(void)
{
    if (s_env.track_n <= 0 || !s_card) {
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
    const char *title = s_env.display_title[0] ? s_env.display_title : "搜索结果";
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
        const char *sid = s_env.app_id[0] ? s_env.app_id : "music.search";
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
    if (idx < 0 || idx >= s_env.track_n || !s_env.tracks[idx].playback_url[0]) {
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
    (void)nodelio_media_play_track(&s_skill_cfg, &s_env.tracks[idx]);
    /* 只更新当前行的图标，避免重建整个列表 */
    update_row_icon(idx);
}

static int neighbor_playable(int from, int dir)
{
    if (s_env.track_n <= 0 || (dir != 1 && dir != -1)) {
        return -1;
    }
    /* from < 0：下一首从 0 起，上一首从末尾起（换一批后当前曲可能不在新列表里） */
    int start = from;
    if (start < 0 || start >= s_env.track_n) {
        start = (dir > 0) ? -1 : 0;
    }
    for (int step = 1; step <= s_env.track_n; step++) {
        int idx = start + dir * step;
        idx %= s_env.track_n;
        if (idx < 0) {
            idx += s_env.track_n;
        }
        if (s_env.tracks[idx].playback_url[0]) {
            return idx;
        }
    }
    return -1;
}

static void on_play_row(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    play_at((int)(intptr_t)lv_event_get_user_data(e));
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
    if (!s_have_env || s_env.track_n <= 0) {
        return false;
    }
    int idxs[NL_ENV_TRACK_MAX];
    int n = 0;
    for (int i = 0; i < s_env.track_n && n < NL_ENV_TRACK_MAX; i++) {
        if (s_env.tracks[i].playback_url[0]) {
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
    if (title_out && title_out_len > 0 && pick >= 0 && pick < s_env.track_n) {
        snprintf(title_out, title_out_len, "%s", s_env.tracks[pick].title);
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

static void anim_show_obj(lv_obj_t *obj, bool from_above)
{
    if (!obj) {
        return;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    if (from_above) {
        lv_obj_set_style_translate_y(obj, -SKILL_ANIM_SLIDE, 0);
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

    if (from_above) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, obj);
        lv_anim_set_values(&b, -SKILL_ANIM_SLIDE, 0);
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
    memset(s_row_idx_labels, 0, sizeof(s_row_idx_labels));
    memset(s_row_btns, 0, sizeof(s_row_btns));
}

static void free_skill_widgets(void)
{
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
    const bool have_list = s_have_env && s_env.track_n > 0 && s_card;

    if (have_list) {
        set_drawer_for_skill(true);
        if (s_scrim) {
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            anim_show_obj(s_scrim, false);
        }
        if (s_card_title) {
            const char *title = s_env.display_title[0] ? s_env.display_title : "搜索结果";
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

    s_card_list = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_card_list);
    lv_obj_set_width(s_card_list, lv_pct(100));
    lv_obj_set_flex_grow(s_card_list, 1);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_card_list, 4, 0);
    lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);

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
    const bool music = strcmp(s_env.card_type, "music_list") == 0;
    const bool news = strcmp(s_env.card_type, "news_list") == 0;
    const bool paged = music || news || list_page_count() > 1;
    const bool compact = !paged && s_env.track_n <= 4;
    if (s_hdr_mid_btn) {
        if (paged) {
            lv_obj_clear_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_card) {
        return;
    }
    if (compact) {
        lv_obj_set_height(s_card, LV_SIZE_CONTENT);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 0);
            lv_obj_set_height(s_card_list, LV_SIZE_CONTENT);
        }
    } else {
        lv_obj_set_height(s_card, SKILL_CARD_H_MAX);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 1);
        }
    }
}

static void fill_list(void)
{
    if (!s_card_list) {
        return;
    }
    if (strcmp(s_env.card_type, "news_list") == 0) {
        fill_news();
        apply_card_chrome();
        return;
    }
    lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clean(s_card_list);
    /* 清空控件引用数组 */
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    const bool can_play = nodelio_caps_has_media();

    set_hdr_mid_icon(MATERIAL_SYMBOLS_SKIP_NEXT);

    const bool music_card = strcmp(s_env.card_type, "music_list") == 0;

    if (s_card_sub) {
        if (s_env.track_n > 0) {
            char sub[48];
            const int pages = list_page_count();
            if (s_list_page >= pages) {
                s_list_page = 0;
            }
            if (music_card) {
                if (pages > 1) {
                    snprintf(sub, sizeof(sub), "共 %d 首 · %d/%d", s_env.track_n, s_list_page + 1, pages);
                } else {
                    snprintf(sub, sizeof(sub), "共 %d 首 · 点播或说歌名", s_env.track_n);
                }
            } else if (pages > 1) {
                snprintf(sub, sizeof(sub), "共 %d 条 · %d/%d", s_env.track_n, s_list_page + 1, pages);
            } else {
                snprintf(sub, sizeof(sub), "共 %d 条", s_env.track_n);
            }
            lv_label_set_text(s_card_sub, sub);
        } else {
            lv_label_set_text(s_card_sub, "点关闭或点遮罩收起");
        }
    }

    if (strcmp(s_env.status, "ok") != 0) {
        lv_obj_t *err = lv_label_create(s_card_list);
        nl_ov_set_font(err);
        lv_label_set_text(err, s_env.error_message[0] ? s_env.error_message : "技能失败");
        lv_obj_set_width(err, lv_pct(100));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(err, UI_DANGER_FG, 0);
        apply_card_chrome();
        return;
    }

    if (s_env.track_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_card_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, s_env.spoken[0] ? s_env.spoken : "无结果");
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
    const int start = s_list_page * SKILL_PAGE_SIZE;
    int end = start + SKILL_PAGE_SIZE;
    if (end > s_env.track_n) {
        end = s_env.track_n;
    }
    for (int base = start; base < end; base += SKILL_PAGE_COLS) {
        lv_obj_t *line = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, 50);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, 6, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        const int ncell = (base + 1 < end) ? 2 : 1;
        for (int c = 0; c < ncell; c++) {
            const int i = base + c;
            lv_obj_t *row = lv_obj_create(line);
            lv_obj_remove_style_all(row);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_height(row, 50);
            lv_obj_set_style_min_width(row, 0, 0);
            lv_obj_set_style_bg_color(row, UI_FIELD, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 12, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
            lv_obj_set_style_pad_hor(row, 6, 0);
            lv_obj_set_style_pad_column(row, 4, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            if (can_play && s_env.tracks[i].playback_url[0]) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_set_style_bg_color(row, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
            }

            lv_obj_t *idx_pod = lv_obj_create(row);
            lv_obj_remove_style_all(idx_pod);
            lv_obj_set_size(idx_pod, 22, 22);
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
            lv_obj_set_style_pad_row(txt, 1, 0);
            lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *t = lv_label_create(txt);
            nl_ov_set_font(t);
            lv_label_set_text(t, s_env.tracks[i].title);
            lv_obj_set_style_text_color(t, UI_TEXT, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, lv_pct(100));
            {
                const lv_font_t *font = lv_obj_get_style_text_font(t, LV_PART_MAIN);
                lv_obj_set_height(t, font ? lv_font_get_line_height(font) : 16);
            }

            lv_obj_t *a = lv_label_create(txt);
            nl_ov_set_font(a);
            if (s_env.tracks[i].artist[0]) {
                lv_label_set_text(a, s_env.tracks[i].artist);
            } else if (music_card) {
                lv_label_set_text(a, "未知艺人");
            } else {
                lv_label_set_text(a, " ");
            }
            lv_obj_set_style_text_color(a, UI_TEXT_MUTED, 0);
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_set_width(a, lv_pct(100));

            if (can_play && s_env.tracks[i].playback_url[0]) {
                const char *icon = (i == s_play_idx) ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW;
                lv_obj_t *btn = make_icon_btn(row, UI_ACCENT, UI_BG, icon, nullptr);
                lv_obj_add_event_cb(btn, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                if (i < 64) {
                    s_row_btns[i] = btn;
                }
            }
        }
        if (ncell == 1) {
            lv_obj_t *pad = lv_obj_create(line);
            lv_obj_remove_style_all(pad);
            lv_obj_set_flex_grow(pad, 1);
            lv_obj_set_height(pad, 50);
            lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        }
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
    if (!skill_ui_is_up()) {
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
    clear_ttl();
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
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
    s_have_env = false;
    ensure_chrome_order();
}

void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, const nl_content_envelope_t *env,
                               bool auto_play_first)
{
    if (!env) {
        return;
    }
    if (cfg) {
        s_skill_cfg = *cfg;
    }
    /* 列表已开且新包只有 1 首要播：若该曲已在旧列表 → 不替换歌单，只 play_at */
    if (auto_play_first && env->auto_play && env->track_n == 1 && s_have_env && s_env.track_n > 1 &&
        strcmp(s_env.card_type, "music_list") == 0 &&
        (strcmp(env->card_type, "music_list") == 0 || env->card_type[0] == '\0')) {
        int hit = find_track_in_env(&s_env, &env->tracks[0]);
        if (hit >= 0 && env->tracks[0].playback_url[0]) {
            ESP_LOGI(TAG, "keep list n=%d play hit=%d title=%s", s_env.track_n, hit,
                     env->tracks[0].title);
            if (SKILL_PAGE_SIZE > 0) {
                s_list_page = hit / SKILL_PAGE_SIZE;
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
    if (s_play_idx >= 0 && s_play_idx < s_env.track_n && s_env.tracks[s_play_idx].playback_url[0]) {
        snprintf(keep_url, sizeof(keep_url), "%s", s_env.tracks[s_play_idx].playback_url);
    }
    s_env = *env;
    s_have_env = true;
    s_list_page = 0;
    s_play_idx = -1;
    if (keep_url[0]) {
        for (int i = 0; i < s_env.track_n; i++) {
            if (strcmp(s_env.tracks[i].playback_url, keep_url) == 0) {
                s_play_idx = i;
                if (SKILL_PAGE_SIZE > 0) {
                    s_list_page = i / SKILL_PAGE_SIZE;
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

    ensure_chrome_order();

    if (!s_news_screen_hook) {
        nodelio_panel_ui_set_screen_cb(news_on_screen);
        s_news_screen_hook = true;
    }
    s_news_detail = -1;
    const char *title = s_env.display_title[0] ? s_env.display_title : "搜索结果";
    if (strcmp(s_env.card_type, "news_list") == 0 && !s_env.display_title[0]) {
        title = "最新新闻";
    }
    lv_label_set_text(s_card_title, title);
    fill_list();

    s_minimized = false;
    hide_mini_fab();
    set_drawer_for_skill(true);

    if (s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        anim_show_obj(s_scrim, false);
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    anim_show_obj(s_card, true);
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();

    clear_ttl();
    if (s_env.ttl_sec > 0) {
        s_ttl_tmr = lv_timer_create(on_ttl, (uint32_t)s_env.ttl_sec * 1000, nullptr);
        if (s_ttl_tmr) {
            lv_timer_set_repeat_count(s_ttl_tmr, 1);
        }
    }

    if (auto_play_first && nodelio_caps_has_media() && s_env.track_n > 0 &&
        s_env.tracks[0].playback_url[0]) {
        /* LLM 常连打 music.search+auto_play；同曲已在播则只刷新列表，不掐 HTTP */
        play_at(0);
    }

    ESP_LOGI(TAG, "show card_type=%s tracks=%d news=%d", s_env.card_type, s_env.track_n, s_env.news_n);
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
            if (s_have_env && s_env.track_n > 0) {
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
