#include "skill_immersive_player.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "media_player.h"
#include "nvs_config.h"
#include "overlay_priv.h"
#include "skill_lyrics.h"
#include "skill_result_ui.h"
#include "skill_ui_priv.h"
#include "ui_theme.h"

#include <cstdio>
#include <cstring>

#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_material_symbols_20_4);

namespace {

constexpr int kIdleEnterMs = 2500;
constexpr int kTickMs = 200;
constexpr int kVinylSzWide = 200;
constexpr int kVinylSzSquare = 140;
constexpr int kLyricVis = 7;

struct ImmersiveUi {
    lv_obj_t *root = nullptr;
    lv_obj_t *left = nullptr;
    lv_obj_t *vinyl = nullptr;
    lv_obj_t *vinyl_hole = nullptr;
    lv_obj_t *vinyl_glyph = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *artist = nullptr;
    lv_obj_t *btn_row = nullptr;
    lv_obj_t *pause_btn = nullptr;
    lv_obj_t *fav_btn = nullptr;
    lv_obj_t *slider = nullptr;
    lv_obj_t *time_lbl = nullptr;
    lv_obj_t *right = nullptr;
    lv_obj_t *lyric_rows[kLyricVis]{};
    lv_obj_t *no_lyric = nullptr;
    lv_timer_t *idle_tmr = nullptr;
    lv_timer_t *tick_tmr = nullptr;
    bool visible = false;
    bool playing = false;
    bool favorited = false;
    bool slider_drag = false;
    int vinyl_angle = 0;
    char title_buf[NL_ENV_TITLE]{};
};

ImmersiveUi s_im;
nodelio_immersive_fav_cb_t s_fav_cb = nullptr;

void show_locked();
void hide_locked();
void refresh_chrome_locked();
void paint_vinyl_color();

uint32_t hash_title(const char *t)
{
    uint32_t h = 2166136261u;
    if (!t) {
        return 0x34c759;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(t); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return 0x202020u | (h & 0x3f3f3fu);
}

void format_mmss(uint32_t ms, char *out, size_t n)
{
    const uint32_t sec = ms / 1000u;
    snprintf(out, n, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

void on_bg_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 子控件冒泡：仅背景本身点击才退出 */
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
    hide_locked();
    if (s_im.playing && s_im.idle_tmr) {
        lv_timer_reset(s_im.idle_tmr);
        lv_timer_resume(s_im.idle_tmr);
    }
}

void on_ctrl_stop(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    nodelio_media_stop();
    hide_locked();
}

void on_ctrl_prev(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    if (!nodelio_skill_media_prev()) {
        nl_ov_toast("没有上一首");
    }
}

void on_ctrl_next(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    if (!nodelio_skill_media_next()) {
        nl_ov_toast("没有下一首");
    }
}

void on_ctrl_pause(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    if (nodelio_media_is_paused()) {
        nodelio_media_resume();
    } else {
        nodelio_media_pause();
    }
    refresh_chrome_locked();
}

void on_ctrl_fav(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    if (s_fav_cb) {
        s_fav_cb();
    } else {
        nl_ov_toast("收藏未就绪");
    }
}

void on_lyric_long(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    nodelio_media_now_title(title, sizeof(title));
    nodelio_media_now_artist(artist, sizeof(artist));
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_cfg_for_http(&s_skill_cfg);
    }
    nodelio_lyrics_request(&s_skill_cfg, title, artist, true);
    nl_ov_toast("正在换歌词…");
}

void on_slider_changed(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_im.slider_drag = true;
        Board::GetInstance().NoteUserActivity();
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_im.slider_drag = false;
        if (!s_im.slider) {
            return;
        }
        const int32_t v = lv_slider_get_value(s_im.slider);
        const uint32_t dur = nodelio_media_duration_ms();
        if (dur == 0) {
            nl_ov_toast("暂不支持跳转");
            return;
        }
        const uint32_t ms = static_cast<uint32_t>((static_cast<uint64_t>(v) * dur) / 1000ull);
        if (!nodelio_media_seek_ms(ms)) {
            nl_ov_toast("暂不支持跳转");
        }
    }
}

lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, int sz)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, sz / 2, 0);
    lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(btn, 10);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, sz >= 52 ? &font_material_symbols_30_4 : &font_material_symbols_20_4,
                               0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, UI_TEXT, 0);
    lv_obj_center(ic);
    return btn;
}

void refresh_lyrics_locked()
{
    if (!s_im.right && !s_im.no_lyric) {
        return;
    }
    const uint32_t pos = nodelio_media_position_ms();
    const int cur = nodelio_lyrics_current_index(pos);
    const int n = nodelio_lyrics_line_count();
    const bool ready = nodelio_lyrics_ready();
    if (s_im.no_lyric) {
        if (ready && s_im.right) {
            lv_obj_add_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
        } else if (!ready) {
            lv_obj_clear_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_im.right) {
        return;
    }
    const int half = kLyricVis / 2;
    for (int i = 0; i < kLyricVis; i++) {
        lv_obj_t *row = s_im.lyric_rows[i];
        if (!row) {
            continue;
        }
        if (!ready || cur < 0) {
            lv_label_set_text(row, "");
            continue;
        }
        const int idx = cur - half + i;
        char line[96];
        if (idx < 0 || idx >= n || !nodelio_lyrics_line_text(idx, line, sizeof(line))) {
            lv_label_set_text(row, "");
            continue;
        }
        lv_label_set_text(row, line);
        const bool on = (idx == cur);
        lv_obj_set_style_text_color(row, on ? UI_ACCENT : UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_opa(row, on ? LV_OPA_COVER : LV_OPA_70, 0);
    }
}

void refresh_chrome_locked()
{
    if (!s_im.visible) {
        return;
    }
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    nodelio_media_now_title(title, sizeof(title));
    nodelio_media_now_artist(artist, sizeof(artist));
    if (s_im.title) {
        lv_label_set_text(s_im.title, title[0] ? title : "播放中");
    }
    if (s_im.artist) {
        lv_label_set_text(s_im.artist, artist[0] ? artist : " ");
    }
    if (s_im.pause_btn) {
        lv_obj_t *ic = lv_obj_get_child(s_im.pause_btn, 0);
        if (ic) {
            const bool playing = nodelio_media_is_playing();
            lv_label_set_text(ic, playing ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW);
        }
    }
    if (s_im.fav_btn) {
        lv_obj_t *ic = lv_obj_get_child(s_im.fav_btn, 0);
        if (ic) {
            lv_obj_set_style_text_color(ic, s_im.favorited ? UI_ACCENT : UI_TEXT_MUTED, 0);
        }
    }
    const uint32_t pos = nodelio_media_position_ms();
    const uint32_t dur = nodelio_media_duration_ms();
    if (s_im.slider && !s_im.slider_drag) {
        if (dur > 0) {
            lv_obj_clear_state(s_im.slider, LV_STATE_DISABLED);
            const int32_t v = static_cast<int32_t>((static_cast<uint64_t>(pos) * 1000ull) / dur);
            lv_slider_set_value(s_im.slider, v > 1000 ? 1000 : v, LV_ANIM_OFF);
        } else {
            lv_obj_add_state(s_im.slider, LV_STATE_DISABLED);
            lv_slider_set_value(s_im.slider, 0, LV_ANIM_OFF);
        }
    }
    if (s_im.time_lbl) {
        char a[16], b[16], line[40];
        format_mmss(pos, a, sizeof(a));
        if (dur > 0) {
            format_mmss(dur, b, sizeof(b));
            snprintf(line, sizeof(line), "%s / %s", a, b);
        } else {
            snprintf(line, sizeof(line), "%s", a);
        }
        lv_label_set_text(s_im.time_lbl, line);
    }
    if (s_im.vinyl && nodelio_media_is_playing()) {
        s_im.vinyl_angle = (s_im.vinyl_angle + 30) % 3600;
        lv_obj_set_style_transform_rotation(s_im.vinyl, s_im.vinyl_angle, 0);
    }
    refresh_lyrics_locked();
}

void tick_cb(lv_timer_t *)
{
    if (!s_im.visible) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        refresh_chrome_locked();
    }
}

void idle_enter_cb(lv_timer_t *)
{
    if (s_im.idle_tmr) {
        lv_timer_pause(s_im.idle_tmr);
    }
    if (!s_im.playing || s_im.visible) {
        return;
    }
    if (nodelio_skill_is_live_radio()) {
        return;
    }
    if (!nodelio_media_is_playing() && !nodelio_media_is_paused()) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        show_locked();
    }
}

void paint_vinyl_color()
{
    if (!s_im.vinyl) {
        return;
    }
    const uint32_t c = hash_title(s_im.title_buf);
    lv_obj_set_style_bg_color(s_im.vinyl, lv_color_hex(c), 0);
}

void hide_locked()
{
    if (s_im.tick_tmr) {
        lv_timer_pause(s_im.tick_tmr);
    }
    if (s_im.root) {
        lv_obj_delete(s_im.root);
        s_im.root = nullptr;
    }
    s_im.left = nullptr;
    s_im.vinyl = nullptr;
    s_im.vinyl_hole = nullptr;
    s_im.vinyl_glyph = nullptr;
    s_im.title = nullptr;
    s_im.artist = nullptr;
    s_im.btn_row = nullptr;
    s_im.pause_btn = nullptr;
    s_im.fav_btn = nullptr;
    s_im.slider = nullptr;
    s_im.time_lbl = nullptr;
    s_im.right = nullptr;
    s_im.no_lyric = nullptr;
    for (int i = 0; i < kLyricVis; i++) {
        s_im.lyric_rows[i] = nullptr;
    }
    s_im.visible = false;
    s_im.slider_drag = false;
}

void show_locked()
{
    if (s_im.visible) {
        refresh_chrome_locked();
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    const auto *p = nodelio_display_profile();
    if (!parent || !p) {
        return;
    }
    const bool wide = p->nav_width != 0;
    const int W = static_cast<int>(p->width);
    const int H = static_cast<int>(p->height);
    const int left_w = wide ? (W * 42 / 100) : W;
    const int pad = 12;

    s_im.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_im.root);
    lv_obj_set_size(s_im.root, W, H);
    lv_obj_align(s_im.root, LV_ALIGN_TOP_LEFT, 0, 0); /* 贴顶消黑边 */
    lv_obj_set_style_bg_color(s_im.root, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_im.root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_im.root, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_im.root, pad, 0);
    lv_obj_set_style_pad_column(s_im.root, 16, 0);
    lv_obj_set_style_pad_row(s_im.root, 8, 0);
    lv_obj_clear_flag(s_im.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_im.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_im.root, on_bg_click, LV_EVENT_CLICKED, nullptr);

    s_im.left = lv_obj_create(s_im.root);
    lv_obj_remove_style_all(s_im.left);
    lv_obj_set_width(s_im.left, left_w - pad);
    lv_obj_set_flex_grow(s_im.left, wide ? 0 : 1);
    lv_obj_set_height(s_im.left, wide ? lv_pct(100) : LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_im.left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_im.left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_im.left, 8, 0);
    lv_obj_clear_flag(s_im.left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_im.left, LV_OBJ_FLAG_EVENT_BUBBLE);

    const int vinyl_sz = wide ? kVinylSzWide : kVinylSzSquare;
    s_im.vinyl = lv_obj_create(s_im.left);
    lv_obj_remove_style_all(s_im.vinyl);
    lv_obj_set_size(s_im.vinyl, vinyl_sz, vinyl_sz);
    lv_obj_set_style_radius(s_im.vinyl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_im.vinyl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_im.vinyl, 3, 0);
    lv_obj_set_style_border_color(s_im.vinyl, UI_BORDER, 0);
    lv_obj_set_style_transform_pivot_x(s_im.vinyl, vinyl_sz / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_im.vinyl, vinyl_sz / 2, 0);
    lv_obj_clear_flag(s_im.vinyl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_im.vinyl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_im.vinyl, on_bg_click, LV_EVENT_CLICKED, nullptr);
    paint_vinyl_color();

    s_im.vinyl_hole = lv_obj_create(s_im.vinyl);
    lv_obj_remove_style_all(s_im.vinyl_hole);
    lv_obj_set_size(s_im.vinyl_hole, vinyl_sz / 5, vinyl_sz / 5);
    lv_obj_set_style_radius(s_im.vinyl_hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_im.vinyl_hole, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_im.vinyl_hole, LV_OPA_COVER, 0);
    lv_obj_center(s_im.vinyl_hole);
    lv_obj_clear_flag(s_im.vinyl_hole, LV_OBJ_FLAG_CLICKABLE);

    s_im.vinyl_glyph = lv_label_create(s_im.vinyl);
    lv_obj_set_style_text_font(s_im.vinyl_glyph, &font_material_symbols_30_4, 0);
    lv_label_set_text(s_im.vinyl_glyph, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_set_style_text_color(s_im.vinyl_glyph, lv_color_white(), 0);
    lv_obj_center(s_im.vinyl_glyph);

    s_im.title = lv_label_create(s_im.left);
    nl_ov_set_font(s_im.title);
    lv_label_set_long_mode(s_im.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_im.title, left_w - 2 * pad);
    lv_obj_set_style_text_color(s_im.title, UI_TEXT, 0);
    lv_obj_set_style_text_align(s_im.title, LV_TEXT_ALIGN_CENTER, 0);

    s_im.artist = lv_label_create(s_im.left);
    nl_ov_set_font(s_im.artist);
    lv_label_set_long_mode(s_im.artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_im.artist, left_w - 2 * pad);
    lv_obj_set_style_text_color(s_im.artist, UI_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_im.artist, LV_TEXT_ALIGN_CENTER, 0);

    s_im.btn_row = lv_obj_create(s_im.left);
    lv_obj_remove_style_all(s_im.btn_row);
    lv_obj_set_width(s_im.btn_row, LV_SIZE_CONTENT);
    lv_obj_set_height(s_im.btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_im.btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_im.btn_row, 10, 0);
    lv_obj_clear_flag(s_im.btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_im.btn_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_SKIP_PREVIOUS, on_ctrl_prev, 44);
    s_im.pause_btn = make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_PAUSE, on_ctrl_pause, 56);
    lv_obj_set_style_bg_color(s_im.pause_btn, UI_ACCENT, 0);
    {
        lv_obj_t *ic = lv_obj_get_child(s_im.pause_btn, 0);
        if (ic) {
            lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
        }
    }
    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_SKIP_NEXT, on_ctrl_next, 44);
    s_im.fav_btn = make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_FAVORITE, on_ctrl_fav, 44);
    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_STOP, on_ctrl_stop, 44);

    s_im.slider = lv_slider_create(s_im.left);
    lv_obj_set_width(s_im.slider, left_w - 2 * pad);
    lv_obj_set_height(s_im.slider, 12);
    lv_slider_set_range(s_im.slider, 0, 1000);
    lv_obj_set_style_bg_color(s_im.slider, UI_CAPSULE_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_KNOB);
    lv_obj_clear_flag(s_im.slider, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_im.slider, on_slider_changed, LV_EVENT_ALL, nullptr);

    s_im.time_lbl = lv_label_create(s_im.left);
    nl_ov_set_font(s_im.time_lbl);
    lv_obj_set_style_text_color(s_im.time_lbl, UI_TEXT_MUTED, 0);

    if (wide) {
        s_im.right = lv_obj_create(s_im.root);
        lv_obj_remove_style_all(s_im.right);
        lv_obj_set_flex_grow(s_im.right, 1);
        lv_obj_set_height(s_im.right, lv_pct(100));
        lv_obj_set_flex_flow(s_im.right, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_im.right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(s_im.right, 4, 0);
        lv_obj_clear_flag(s_im.right, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_im.right, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_im.right, on_lyric_long, LV_EVENT_LONG_PRESSED, nullptr);
        lv_obj_add_event_cb(s_im.right, on_bg_click, LV_EVENT_CLICKED, nullptr);

        for (int i = 0; i < kLyricVis; i++) {
            s_im.lyric_rows[i] = lv_label_create(s_im.right);
            nl_ov_set_font(s_im.lyric_rows[i]);
            lv_label_set_long_mode(s_im.lyric_rows[i], LV_LABEL_LONG_DOT);
            lv_obj_set_width(s_im.lyric_rows[i], lv_pct(100));
            lv_label_set_text(s_im.lyric_rows[i], "");
        }
        s_im.no_lyric = lv_label_create(s_im.right);
        nl_ov_set_font(s_im.no_lyric);
        lv_label_set_text(s_im.no_lyric, "暂无歌词 · 长按可换");
        lv_obj_set_style_text_color(s_im.no_lyric, UI_TEXT_TIP, 0);
        lv_obj_add_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_im.right = nullptr;
        s_im.no_lyric = lv_label_create(s_im.left);
        nl_ov_set_font(s_im.no_lyric);
        lv_label_set_text(s_im.no_lyric, "暂无歌词 · 长按标题可换");
        lv_obj_set_style_text_color(s_im.no_lyric, UI_TEXT_TIP, 0);
        lv_obj_add_flag(s_im.title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_im.title, on_lyric_long, LV_EVENT_LONG_PRESSED, nullptr);
    }

    s_im.visible = true;
    s_im.vinyl_angle = 0;
    if (!s_im.tick_tmr) {
        s_im.tick_tmr = lv_timer_create(tick_cb, kTickMs, nullptr);
    } else {
        lv_timer_resume(s_im.tick_tmr);
        lv_timer_reset(s_im.tick_tmr);
    }
    lv_obj_move_foreground(s_im.root);
    refresh_chrome_locked();
}

void on_now_playing_core(const char *title, bool playing)
{
    if (!title || !title[0]) {
        s_im.playing = false;
        s_im.title_buf[0] = '\0';
        if (s_im.idle_tmr) {
            lv_timer_pause(s_im.idle_tmr);
        }
        hide_locked();
        return;
    }
    snprintf(s_im.title_buf, sizeof(s_im.title_buf), "%s", title);
    s_im.playing = playing || nodelio_media_is_paused();
    if (nodelio_skill_is_live_radio()) {
        if (s_im.idle_tmr) {
            lv_timer_pause(s_im.idle_tmr);
        }
        hide_locked();
        return;
    }
    if (!s_im.idle_tmr) {
        s_im.idle_tmr = lv_timer_create(idle_enter_cb, kIdleEnterMs, nullptr);
    }
    if (s_im.playing) {
        lv_timer_reset(s_im.idle_tmr);
        lv_timer_resume(s_im.idle_tmr);
    } else if (s_im.idle_tmr) {
        lv_timer_pause(s_im.idle_tmr);
    }
    if (s_im.visible) {
        paint_vinyl_color();
        refresh_chrome_locked();
    }
}

}  // namespace

extern "C" void nodelio_skill_immersive_hide(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        hide_locked();
    }
    if (s_im.playing && s_im.idle_tmr) {
        lv_timer_reset(s_im.idle_tmr);
        lv_timer_resume(s_im.idle_tmr);
    }
}

extern "C" bool nodelio_skill_immersive_visible(void) { return s_im.visible; }

extern "C" void nodelio_skill_immersive_set_fav_cb(nodelio_immersive_fav_cb_t cb) { s_fav_cb = cb; }

extern "C" void nodelio_skill_immersive_set_favorited(bool on)
{
    s_im.favorited = on;
    if (!s_im.visible) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        refresh_chrome_locked();
    }
}

extern "C" void nodelio_skill_immersive_note_activity(void)
{
    /* LVGL 事件线程内调用：勿再抢 DisplayLockGuard */
    if (s_im.visible) {
        hide_locked();
        if (s_im.playing && s_im.idle_tmr) {
            lv_timer_reset(s_im.idle_tmr);
            lv_timer_resume(s_im.idle_tmr);
        }
        return;
    }
    if (s_im.idle_tmr && s_im.playing) {
        lv_timer_reset(s_im.idle_tmr);
        lv_timer_resume(s_im.idle_tmr);
    }
}

/** 须在已持有显示锁时调用（media_now_playing 路径）。 */
extern "C" void nodelio_skill_immersive_on_now_playing(const char *title, bool playing)
{
    on_now_playing_core(title, playing);
}
