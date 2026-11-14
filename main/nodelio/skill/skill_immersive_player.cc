#include "skill_immersive_player.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "media_player.h"
#include "nvs_config.h"
#include "overlay_priv.h"
#include "skill_cover.h"
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

constexpr int kTickMs = 400;       /* 曲名/进度/歌词：慢刷即可 */
constexpr int kVinylRevMs = 12000; /* 黑胶一周时长；用 lv_anim 跟真实时间，不绑 400ms 步进 */
constexpr int kVinylSz = 220;
constexpr int kCoverSz = 132; /* 中心缩小封面，外圈留黑胶边 */
constexpr int kVinylLabel = 72; /* 无封面时中心碟标 */
constexpr int kBtnSec = 48;
constexpr int kBtnPri = 56;
constexpr int kLeftW = 300; /* 碟片列加宽，与中栏拉开 */
constexpr int kMidW = 460;
constexpr int kLyricVis = 7;

struct ImmersiveUi {
    lv_obj_t *root = nullptr;
    lv_obj_t *left = nullptr;
    lv_obj_t *mid = nullptr;
    lv_obj_t *vinyl = nullptr;
    lv_obj_t *cover_img = nullptr;
    lv_obj_t *vinyl_ring[2]{};
    lv_obj_t *vinyl_label = nullptr;
    lv_obj_t *vinyl_hole = nullptr;
    lv_obj_t *vinyl_glyph = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *artist = nullptr;
    lv_obj_t *btn_row = nullptr;
    lv_obj_t *pause_btn = nullptr;
    lv_obj_t *fav_btn = nullptr;
    lv_obj_t *collapse_btn = nullptr;
    lv_obj_t *slider = nullptr;
    lv_obj_t *time_lbl = nullptr;
    lv_obj_t *right = nullptr;
    lv_obj_t *lyric_rows[kLyricVis]{};
    lv_obj_t *no_lyric = nullptr;
    lv_timer_t *tick_tmr = nullptr;
    lv_timer_t *cover_tmr = nullptr; /* 延后拉封面，避免与播流抢带宽 */
    lv_image_dsc_t cover_dsc{}; /* 必须常驻：lv_image 只存指针 */
    bool visible = false;
    bool playing = false;
    bool favorited = false;
    bool slider_drag = false;
    bool cover_on = false;
    int vinyl_angle = 0;
    uint32_t ambient = 0; /* 当前背景单色 RGB888（封面取色 / 无封面温馨底） */
    char title_buf[NL_ENV_TITLE]{};
};

/** 沉浸时停掉下层列表/播放条，避免双套 UI 同屏刷 */
struct UnderlayPark {
    bool parked = false;
    bool had_card = false;
    bool had_scrim = false;
    bool had_now = false;
    bool had_mini = false;
};

ImmersiveUi s_im;
UnderlayPark s_park;
nodelio_immersive_fav_cb_t s_fav_cb = nullptr;

void show_locked();
void hide_locked();
void refresh_chrome_locked();
void paint_vinyl_color();
void paint_immersive_ctrl_btn(lv_obj_t *btn, bool primary);
void paint_immersive_fg();
void set_vinyl_cover_mode(bool has_cover);
void paint_ambient_locked();
void apply_ambient(uint32_t rgb);
uint32_t ambient_warm_fallback();
uint32_t ambient_from_cover(const lv_image_dsc_t *dsc);
void cover_mask_circle(lv_image_dsc_t *dsc);
void vinyl_apply_angle(int32_t angle);
void vinyl_spin_exec(void *var, int32_t v);
void vinyl_spin_stop();
void vinyl_spin_sync();

/*
 * 沉浸页氛围底恒偏深（封面取色 / 暖底），与系统浅色/曜夜/沉浸主题解耦：
 * 字与次要钮固定「深底浅字 / 浅胶囊深标」，三套主题打开都可读。
 */
constexpr uint32_t kImFg = 0xf4f7fb;
constexpr uint32_t kImFgMuted = 0xc5d0dc;
constexpr uint32_t kImFgTip = 0x93a0b0;
constexpr uint32_t kVinylBlack = 0x000000u;
constexpr uint32_t kVinylGroove = 0x2e2e2eu; /* 纹路：黑底上极淡亮线，勿用大灰块 */
constexpr uint32_t kVinylLabelBg = 0x1c1c1cu; /* 无封面中心碟标，略亮于胶面 */
constexpr uint32_t kImBtnSecBg = 0xeef2f7u;
constexpr uint32_t kImBtnSecFg = 0x1c2430u;
constexpr uint32_t kImBtnPriFg = 0xffffffu;
constexpr uint32_t kImSliderTrack = 0x3a4555u;

/** 仅宽屏（P4 侧轨）做自动沉浸；sila-86 方屏性能不够，禁入。 */
bool immersive_allowed()
{
    const auto *p = nodelio_display_profile();
    return p && p->nav_width != 0;
}

void park_underlay_locked()
{
    if (s_park.parked) {
        return;
    }
    s_park.had_card = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    s_park.had_scrim = s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    s_park.had_now = s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    s_park.had_mini = s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    stop_now_anims();
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_bar) {
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_mini_fab) {
        lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    }
    s_park.parked = true;
}

void unpark_underlay_locked()
{
    if (!s_park.parked) {
        return;
    }
    if (s_park.had_scrim && s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_park.had_card && s_card) {
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_park.had_now && s_now_bar) {
        lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        if (nodelio_media_is_playing()) {
            start_now_anims();
        }
    }
    if (s_park.had_mini && s_mini_fab) {
        lv_obj_clear_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    }
    s_park = {};
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
}

uint32_t mix_u32(uint32_t a, uint32_t b, int t /*0..256*/)
{
    if (t <= 0) {
        return a;
    }
    if (t >= 256) {
        return b;
    }
    const int ar = (a >> 16) & 0xff, ag = (a >> 8) & 0xff, ab = a & 0xff;
    const int br = (b >> 16) & 0xff, bg = (b >> 8) & 0xff, bb = b & 0xff;
    const int r = ar + ((br - ar) * t) / 256;
    const int g = ag + ((bg - ag) * t) / 256;
    const int bl = ab + ((bb - ab) * t) / 256;
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(bl);
}

/** 封面色压成可衬白字的深单色 */
uint32_t to_ambient(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xff;
    int g = (rgb >> 8) & 0xff;
    int b = rgb & 0xff;
    int mx = r > g ? r : g;
    if (b > mx) {
        mx = b;
    }
    if (mx < 1) {
        mx = 1;
    }
    /* 保色相，峰值压到约 0x5a，再暗化到背景档 */
    r = r * 90 / mx;
    g = g * 90 / mx;
    b = b * 90 / mx;
    r = (r * 38) / 100;
    g = (g * 38) / 100;
    b = (b * 38) / 100;
    const uint32_t tint =
        (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    return mix_u32(UI_C_BG, tint, 200);
}

/** 无封面：深暖棕，不跟曲名哈希跳色 */
uint32_t ambient_warm_fallback()
{
    return 0x2C2118u;
}

uint32_t ambient_from_cover(const lv_image_dsc_t *dsc)
{
    if (!dsc || !dsc->data || dsc->header.w == 0 || dsc->header.h == 0) {
        return ambient_warm_fallback();
    }
    if (dsc->header.cf != LV_COLOR_FORMAT_RGB565) {
        return ambient_warm_fallback();
    }
    const uint32_t w = dsc->header.w;
    const uint32_t h = dsc->header.h;
    const uint32_t stride = dsc->header.stride ? dsc->header.stride : w * 2;
    const uint8_t *base = static_cast<const uint8_t *>(dsc->data);
    uint32_t rs = 0, gs = 0, bs = 0, n = 0;
    /* 稀疏采样中心区域，避免边框黑 */
    const uint32_t x0 = w / 6, x1 = w - w / 6;
    const uint32_t y0 = h / 6, y1 = h - h / 6;
    for (uint32_t y = y0; y < y1; y += 3) {
        const uint16_t *row =
            reinterpret_cast<const uint16_t *>(base + static_cast<size_t>(y) * stride);
        for (uint32_t x = x0; x < x1; x += 3) {
            const uint16_t p = row[x];
            rs += ((p >> 11) & 0x1f) * 255 / 31;
            gs += ((p >> 5) & 0x3f) * 255 / 63;
            bs += (p & 0x1f) * 255 / 31;
            ++n;
        }
    }
    if (n == 0) {
        return ambient_warm_fallback();
    }
    const uint32_t rgb = ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
    return to_ambient(rgb);
}

void paint_ambient_locked()
{
    if (!s_im.root) {
        return;
    }
    const uint32_t c = s_im.ambient ? s_im.ambient : ambient_warm_fallback();
    lv_obj_set_style_bg_color(s_im.root, lv_color_hex(c), 0);
    lv_obj_set_style_bg_grad_dir(s_im.root, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(s_im.root, LV_OPA_COVER, 0);
}

void apply_ambient(uint32_t rgb)
{
    if (!rgb) {
        rgb = ambient_warm_fallback();
    }
    if (rgb == s_im.ambient && s_im.root) {
        return;
    }
    s_im.ambient = rgb;
    paint_ambient_locked();
}

void format_mmss(uint32_t ms, char *out, size_t n)
{
    const uint32_t sec = ms / 1000u;
    snprintf(out, n, "%u:%02u", (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

void exit_immersive_locked()
{
    hide_locked();
}

void on_ctrl_collapse(lv_event_t *)
{
    Board::GetInstance().NoteUserActivity();
    exit_immersive_locked();
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
    if (!s_fav_cb) {
        nl_ov_toast("收藏未就绪");
        return;
    }
    /* 先乐观标红，等服务端回包再 sync 纠正 */
    s_im.favorited = !s_im.favorited;
    if (s_im.fav_btn) {
        lv_obj_t *ic = lv_obj_get_child(s_im.fav_btn, 0);
        if (ic) {
            lv_obj_set_style_text_color(
                ic, s_im.favorited ? UI_DANGER : lv_color_hex(kImBtnSecFg), 0);
        }
    }
    s_fav_cb();
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

lv_obj_t *make_icon_btn(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, int sz, bool primary)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    /* 禁止用主题 CAPSULE_OFF：曜夜/沉浸下是深底，叠深图标会「空心黑球」 */
    lv_obj_set_style_bg_color(btn, primary ? UI_ACCENT : lv_color_hex(kImBtnSecBg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 2, 0); /* 符号视觉重心偏下，略垫底居中 */
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(btn, 8);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, primary ? &font_material_symbols_30_4 : &font_material_symbols_20_4,
                               0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, primary ? lv_color_hex(kImBtnPriFg) : lv_color_hex(kImBtnSecFg),
                                0);
    lv_obj_set_style_translate_y(ic, -1, 0);
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
    const bool ready = nodelio_lyrics_ready() && n > 0;
    if (s_im.no_lyric) {
        if (ready) {
            lv_obj_add_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_im.no_lyric, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_im.right) {
        return;
    }
    /* 未唱到首句前：从第 0 行起展示，不要整栏空白等到开唱 */
    const int focus = (ready && cur < 0) ? 0 : cur;
    const int half = kLyricVis / 2;
    for (int i = 0; i < kLyricVis; i++) {
        lv_obj_t *row = s_im.lyric_rows[i];
        if (!row) {
            continue;
        }
        if (!ready || focus < 0) {
            lv_label_set_text(row, "");
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int idx = focus - half + i;
        char line[96];
        if (idx < 0 || idx >= n || !nodelio_lyrics_line_text(idx, line, sizeof(line))) {
            lv_label_set_text(row, "");
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(row, line);
        const bool on = (cur >= 0 && idx == cur);
        lv_obj_set_style_text_color(row, on ? UI_ACCENT : lv_color_hex(kImFgMuted), 0);
        lv_obj_set_style_text_opa(row, on ? LV_OPA_COVER : LV_OPA_80, 0);
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
            /* 已收藏标红（勿用品牌绿，看不出状态） */
            lv_obj_set_style_text_color(
                ic, s_im.favorited ? UI_DANGER : lv_color_hex(kImBtnSecFg), 0);
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
    refresh_lyrics_locked();
    vinyl_spin_sync();
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

/** RGB565 封面角填黑，旋转时不用 clip_corner（软件圆角+旋转极卡） */
void cover_mask_circle(lv_image_dsc_t *dsc)
{
    if (!dsc || !dsc->data || dsc->header.w == 0 || dsc->header.h == 0) {
        return;
    }
    if (dsc->header.cf != LV_COLOR_FORMAT_RGB565) {
        return;
    }
    const int w = static_cast<int>(dsc->header.w);
    const int h = static_cast<int>(dsc->header.h);
    const int cx = w / 2;
    const int cy = h / 2;
    const int rad = cx < cy ? cx : cy;
    const int rr = rad * rad;
    const uint32_t stride = dsc->header.stride ? dsc->header.stride : static_cast<uint32_t>(w * 2);
    auto *base = const_cast<uint8_t *>(static_cast<const uint8_t *>(dsc->data));
    for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<uint16_t *>(base + static_cast<size_t>(y) * stride);
        const int dy = y - cy;
        for (int x = 0; x < w; ++x) {
            const int dx = x - cx;
            if (dx * dx + dy * dy > rr) {
                row[x] = 0x0000;
            }
        }
    }
}

void vinyl_apply_angle(int32_t angle)
{
    angle %= 3600;
    if (angle < 0) {
        angle += 3600;
    }
    s_im.vinyl_angle = angle;
    /* 有封面：只转 lv_image（专用旋转路径）；黑胶外圈静止——比转整棵 clip 子树轻一个数量级 */
    if (s_im.cover_on && s_im.cover_img) {
        if (s_im.vinyl) {
            lv_obj_set_style_transform_rotation(s_im.vinyl, 0, 0);
        }
        lv_image_set_rotation(s_im.cover_img, angle);
        return;
    }
    if (s_im.cover_img) {
        lv_image_set_rotation(s_im.cover_img, 0);
    }
    if (s_im.vinyl) {
        lv_obj_set_style_transform_rotation(s_im.vinyl, angle, 0);
    }
}

void vinyl_spin_exec(void *, int32_t v)
{
    vinyl_apply_angle(v);
}

void vinyl_spin_stop()
{
    lv_anim_delete(&s_im, vinyl_spin_exec);
}

void vinyl_spin_sync()
{
    if (!s_im.visible || !s_im.vinyl) {
        vinyl_spin_stop();
        return;
    }
    const bool should = nodelio_media_is_playing() && nodelio_vinyl_spin_enabled();
    lv_anim_t *running = lv_anim_get(&s_im, vinyl_spin_exec);
    if (!should) {
        vinyl_spin_stop();
        vinyl_apply_angle(s_im.vinyl_angle);
        return;
    }
    if (running) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_im);
    lv_anim_set_values(&a, s_im.vinyl_angle, s_im.vinyl_angle + 3600);
    lv_anim_set_time(&a, kVinylRevMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, vinyl_spin_exec);
    lv_anim_start(&a);
}

void set_vinyl_cover_mode(bool has_cover)
{
    s_im.cover_on = has_cover;
    /* 纹路环始终显示，形成外圈黑胶边（有封面时外圈不转，纹路静止可接受） */
    for (int i = 0; i < 2; i++) {
        if (s_im.vinyl_ring[i]) {
            lv_obj_clear_flag(s_im.vinyl_ring[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_im.vinyl_label) {
        if (has_cover) {
            lv_obj_add_flag(s_im.vinyl_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_im.vinyl_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_im.cover_img) {
        if (has_cover) {
            lv_obj_clear_flag(s_im.cover_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_im.cover_img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* 切模式时把角度落到当前路径，避免父节点/图片双旋转；并按是否整盘转重算 clip */
    paint_vinyl_color();
    vinyl_apply_angle(s_im.vinyl_angle);
    vinyl_spin_sync();
}

void paint_vinyl_color()
{
    if (!s_im.vinyl) {
        return;
    }
    /* 纯黑胶面；去掉描边（描边在暖色氛围上会发灰边） */
    lv_obj_set_style_bg_color(s_im.vinyl, lv_color_hex(kVinylBlack), 0);
    lv_obj_set_style_bg_opa(s_im.vinyl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_im.vinyl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_im.vinyl, 0, 0);
    lv_obj_set_style_border_opa(s_im.vinyl, LV_OPA_TRANSP, 0);
    for (int i = 0; i < 2; i++) {
        if (s_im.vinyl_ring[i]) {
            lv_obj_set_style_border_color(s_im.vinyl_ring[i], lv_color_hex(kVinylGroove), 0);
            lv_obj_set_style_border_opa(s_im.vinyl_ring[i], LV_OPA_30, 0);
        }
    }
    if (s_im.vinyl_label) {
        if (!s_im.cover_on) {
            lv_obj_set_style_bg_color(s_im.vinyl_label, lv_color_hex(kVinylLabelBg), 0);
            lv_obj_set_style_bg_opa(s_im.vinyl_label, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_im.vinyl_label, 1, 0);
            lv_obj_set_style_border_color(s_im.vinyl_label, lv_color_hex(0x3a3a3a), 0);
            lv_obj_set_style_border_opa(s_im.vinyl_label, LV_OPA_COVER, 0);
        }
    }
    if (s_im.vinyl_hole) {
        lv_obj_set_style_bg_color(s_im.vinyl_hole, lv_color_hex(kVinylBlack), 0);
    }
    if (s_im.vinyl_glyph) {
        lv_obj_set_style_text_color(s_im.vinyl_glyph, lv_color_hex(kImFg), 0);
    }
    /*
     * clip 规则：有封面时只转 image、碟体静止 → 可 clip；
     * 无封面整盘转时禁 clip（圆角蒙版+transform 极卡）；停转可 clip。
     */
    const bool clip_vinyl = s_im.cover_on || !nodelio_vinyl_spin_enabled();
    lv_obj_set_style_clip_corner(s_im.vinyl, clip_vinyl, 0);
    if (s_im.cover_img) {
        lv_obj_set_style_bg_color(s_im.cover_img, lv_color_hex(kVinylBlack), 0);
        lv_obj_set_style_bg_opa(s_im.cover_img, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(s_im.cover_img, 0, 0);
        const bool clip_cover = !nodelio_vinyl_spin_enabled();
        lv_obj_set_style_radius(s_im.cover_img, clip_cover ? LV_RADIUS_CIRCLE : 0, 0);
        lv_obj_set_style_clip_corner(s_im.cover_img, clip_cover, 0);
    }
}

void paint_immersive_ctrl_btn(lv_obj_t *btn, bool primary)
{
    if (!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, primary ? UI_ACCENT : lv_color_hex(kImBtnSecBg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_t *ic = lv_obj_get_child(btn, 0);
    if (!ic) {
        return;
    }
    if (btn == s_im.fav_btn && s_im.favorited) {
        lv_obj_set_style_text_color(ic, UI_DANGER, 0);
        return;
    }
    lv_obj_set_style_text_color(ic, primary ? lv_color_hex(kImBtnPriFg) : lv_color_hex(kImBtnSecFg),
                                0);
}

void paint_immersive_fg()
{
    if (s_im.title) {
        lv_obj_set_style_text_color(s_im.title, lv_color_hex(kImFg), 0);
    }
    if (s_im.artist) {
        lv_obj_set_style_text_color(s_im.artist, lv_color_hex(kImFgMuted), 0);
    }
    if (s_im.time_lbl) {
        lv_obj_set_style_text_color(s_im.time_lbl, lv_color_hex(kImFgMuted), 0);
    }
    if (s_im.no_lyric) {
        lv_obj_set_style_text_color(s_im.no_lyric, lv_color_hex(kImFgTip), 0);
    }
    if (s_im.collapse_btn) {
        paint_immersive_ctrl_btn(s_im.collapse_btn, false);
    }
    if (s_im.btn_row) {
        const uint32_t n = lv_obj_get_child_count(s_im.btn_row);
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *btn = lv_obj_get_child(s_im.btn_row, i);
            paint_immersive_ctrl_btn(btn, btn == s_im.pause_btn);
        }
    }
    if (s_im.slider) {
        lv_obj_set_style_bg_color(s_im.slider, lv_color_hex(kImSliderTrack), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_im.slider, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_KNOB);
    }
}

void on_cover_ready(const lv_image_dsc_t *dsc, void *)
{
    if (!dsc || !dsc->data || !s_im.visible || !s_im.cover_img || !s_im.vinyl) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_im.cover_img) {
        return;
    }
    /* 拷到常驻 dsc：回调参数是栈上副本，lv_image 只保存指针 */
    s_im.cover_dsc = *dsc;
    cover_mask_circle(&s_im.cover_dsc);
    lv_image_set_src(s_im.cover_img, &s_im.cover_dsc);
    lv_obj_set_size(s_im.cover_img, kCoverSz, kCoverSz);
    /* 角已填黑：禁 clip_corner，否则每帧圆角蒙版+旋转打满 CPU */
    lv_obj_set_style_radius(s_im.cover_img, 0, 0);
    lv_obj_set_style_clip_corner(s_im.cover_img, false, 0);
    lv_image_set_pivot(s_im.cover_img, kCoverSz / 2, kCoverSz / 2);
    lv_obj_center(s_im.cover_img);
    /* 提到纹路环之上，仍小于黑胶，外圈黑边可见 */
    lv_obj_move_foreground(s_im.cover_img);
    set_vinyl_cover_mode(true);
    paint_vinyl_color();
    apply_ambient(ambient_from_cover(dsc));
}

void cover_delay_cb(lv_timer_t *t)
{
    if (s_im.cover_tmr == t) {
        s_im.cover_tmr = nullptr;
    }
    lv_timer_delete(t);
    if (!s_im.visible) {
        return;
    }
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    nodelio_media_now_title(title, sizeof(title));
    nodelio_media_now_artist(artist, sizeof(artist));
    if (!title[0]) {
        return;
    }
    nodelio_cover_request(title, artist, on_cover_ready, nullptr);
}

void request_cover_and_lyrics()
{
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    nodelio_media_now_title(title, sizeof(title));
    nodelio_media_now_artist(artist, sizeof(artist));
    if (!title[0]) {
        return;
    }
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_cfg_for_http(&s_skill_cfg);
    }
    /* 歌词轻量；封面 HTTPS 大图延后，避免与 Ogg 播流抢 WiFi 导致 underrun */
    nodelio_lyrics_request(&s_skill_cfg, title, artist, false);
    if (s_im.cover_tmr) {
        lv_timer_delete(s_im.cover_tmr);
        s_im.cover_tmr = nullptr;
    }
    s_im.cover_tmr = lv_timer_create(cover_delay_cb, 2800, nullptr);
    if (s_im.cover_tmr) {
        lv_timer_set_repeat_count(s_im.cover_tmr, 1);
    }
}

void hide_locked()
{
    vinyl_spin_stop();
    if (s_im.tick_tmr) {
        lv_timer_pause(s_im.tick_tmr);
    }
    if (s_im.cover_tmr) {
        lv_timer_delete(s_im.cover_tmr);
        s_im.cover_tmr = nullptr;
    }
    if (s_im.root) {
        lv_obj_delete(s_im.root);
        s_im.root = nullptr;
    }
    s_im.left = nullptr;
    s_im.mid = nullptr;
    s_im.vinyl = nullptr;
    s_im.cover_img = nullptr;
    s_im.vinyl_ring[0] = nullptr;
    s_im.vinyl_ring[1] = nullptr;
    s_im.vinyl_label = nullptr;
    s_im.vinyl_hole = nullptr;
    s_im.vinyl_glyph = nullptr;
    memset(&s_im.cover_dsc, 0, sizeof(s_im.cover_dsc));
    s_im.title = nullptr;
    s_im.artist = nullptr;
    s_im.btn_row = nullptr;
    s_im.pause_btn = nullptr;
    s_im.fav_btn = nullptr;
    s_im.collapse_btn = nullptr;
    s_im.slider = nullptr;
    s_im.time_lbl = nullptr;
    s_im.right = nullptr;
    s_im.no_lyric = nullptr;
    for (int i = 0; i < kLyricVis; i++) {
        s_im.lyric_rows[i] = nullptr;
    }
    s_im.visible = false;
    s_im.slider_drag = false;
    s_im.cover_on = false;
    s_im.ambient = 0;
    unpark_underlay_locked();
}

void show_locked()
{
    if (!immersive_allowed()) {
        return;
    }
    if (s_im.visible) {
        refresh_chrome_locked();
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    const auto *p = nodelio_display_profile();
    if (!parent || !p) {
        return;
    }
    park_underlay_locked();
    const int W = static_cast<int>(p->width);
    const int H = static_cast<int>(p->height);
    const int pad_x = 20;
    const int pad_y = 14;

    s_im.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_im.root);
    lv_obj_set_size(s_im.root, W, H);
    lv_obj_align(s_im.root, LV_ALIGN_TOP_LEFT, 0, 0);
    s_im.ambient = 0;
    apply_ambient(ambient_warm_fallback());
    lv_obj_set_flex_flow(s_im.root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_im.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(s_im.root, pad_x, 0);
    lv_obj_set_style_pad_right(s_im.root, pad_x, 0);
    lv_obj_set_style_pad_top(s_im.root, pad_y, 0);
    lv_obj_set_style_pad_bottom(s_im.root, pad_y, 0);
    lv_obj_set_style_pad_column(s_im.root, 44, 0); /* 碟片与中栏拉开 */
    lv_obj_clear_flag(s_im.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_im.root, LV_OBJ_FLAG_CLICKABLE);

    /* 右上角最小化（不占 flex 行） */
    s_im.collapse_btn =
        make_icon_btn(s_im.root, MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN, on_ctrl_collapse, kBtnSec,
                      false);
    lv_obj_set_style_bg_opa(s_im.collapse_btn, LV_OPA_50, 0);
    lv_obj_add_flag(s_im.collapse_btn,
                    (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_clear_flag(s_im.collapse_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(s_im.collapse_btn, LV_ALIGN_TOP_RIGHT, -8, 8);

    /* 左：仅转圈封面 */
    s_im.left = lv_obj_create(s_im.root);
    lv_obj_remove_style_all(s_im.left);
    lv_obj_set_size(s_im.left, kLeftW, lv_pct(100));
    lv_obj_set_style_bg_opa(s_im.left, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_im.left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_im.left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_im.left, LV_OBJ_FLAG_SCROLLABLE);

    s_im.vinyl = lv_obj_create(s_im.left);
    lv_obj_remove_style_all(s_im.vinyl);
    lv_obj_set_size(s_im.vinyl, kVinylSz, kVinylSz);
    lv_obj_set_style_radius(s_im.vinyl, LV_RADIUS_CIRCLE, 0);
    /* 无封面整盘转时禁 clip_corner：圆角蒙版+子树 transform 是卡顿主因 */
    lv_obj_set_style_clip_corner(s_im.vinyl, false, 0);
    lv_obj_set_style_bg_opa(s_im.vinyl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_im.vinyl, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_im.vinyl, kVinylSz / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_im.vinyl, kVinylSz / 2, 0);
    lv_obj_clear_flag(s_im.vinyl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_im.vinyl, LV_OBJ_FLAG_CLICKABLE);

    /* 黑胶纹路环 */
    static const int kRingSz[2] = {186, 148};
    for (int i = 0; i < 2; i++) {
        s_im.vinyl_ring[i] = lv_obj_create(s_im.vinyl);
        lv_obj_remove_style_all(s_im.vinyl_ring[i]);
        lv_obj_set_size(s_im.vinyl_ring[i], kRingSz[i], kRingSz[i]);
        lv_obj_set_style_radius(s_im.vinyl_ring[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_im.vinyl_ring[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_im.vinyl_ring[i], 1, 0);
        lv_obj_set_style_border_color(s_im.vinyl_ring[i], lv_color_hex(UI_C_BORDER), 0);
        lv_obj_set_style_border_opa(s_im.vinyl_ring[i], LV_OPA_40, 0);
        lv_obj_center(s_im.vinyl_ring[i]);
        lv_obj_clear_flag(s_im.vinyl_ring[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_im.cover_img = lv_image_create(s_im.vinyl);
    lv_obj_remove_style_all(s_im.cover_img);
    lv_obj_set_size(s_im.cover_img, kCoverSz, kCoverSz);
    lv_obj_set_style_bg_color(s_im.cover_img, lv_color_hex(0x121212), 0);
    lv_obj_set_style_bg_opa(s_im.cover_img, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_im.cover_img, 0, 0);
    lv_obj_set_style_clip_corner(s_im.cover_img, false, 0);
    lv_image_set_pivot(s_im.cover_img, kCoverSz / 2, kCoverSz / 2);
    lv_obj_center(s_im.cover_img);
    lv_obj_add_flag(s_im.cover_img, LV_OBJ_FLAG_HIDDEN);

    /* 中心碟标 + 轴孔 */
    s_im.vinyl_label = lv_obj_create(s_im.vinyl);
    lv_obj_remove_style_all(s_im.vinyl_label);
    lv_obj_set_size(s_im.vinyl_label, kVinylLabel, kVinylLabel);
    lv_obj_set_style_radius(s_im.vinyl_label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_im.vinyl_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_im.vinyl_label, 1, 0);
    lv_obj_set_style_border_color(s_im.vinyl_label, lv_color_hex(UI_C_BORDER), 0);
    lv_obj_center(s_im.vinyl_label);
    lv_obj_clear_flag(s_im.vinyl_label, LV_OBJ_FLAG_CLICKABLE);

    s_im.vinyl_hole = lv_obj_create(s_im.vinyl_label);
    lv_obj_remove_style_all(s_im.vinyl_hole);
    lv_obj_set_size(s_im.vinyl_hole, 10, 10);
    lv_obj_set_style_radius(s_im.vinyl_hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_im.vinyl_hole, lv_color_hex(kVinylBlack), 0);
    lv_obj_set_style_bg_opa(s_im.vinyl_hole, LV_OPA_COVER, 0);
    lv_obj_center(s_im.vinyl_hole);
    lv_obj_clear_flag(s_im.vinyl_hole, LV_OBJ_FLAG_CLICKABLE);

    s_im.vinyl_glyph = lv_label_create(s_im.vinyl_label);
    lv_obj_set_style_text_font(s_im.vinyl_glyph, &font_material_symbols_20_4, 0);
    lv_label_set_text(s_im.vinyl_glyph, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_set_style_text_color(s_im.vinyl_glyph, lv_color_hex(kImFg), 0);
    /* 音符略上，轴孔占碟标几何中心 */
    lv_obj_align(s_im.vinyl_glyph, LV_ALIGN_CENTER, 0, -8);

    paint_vinyl_color();

    /* 中：曲名 + 控件条 */
    s_im.mid = lv_obj_create(s_im.root);
    lv_obj_remove_style_all(s_im.mid);
    lv_obj_set_size(s_im.mid, kMidW, lv_pct(100));
    lv_obj_set_style_bg_opa(s_im.mid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_im.mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_im.mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_im.mid, 12, 0);
    lv_obj_clear_flag(s_im.mid, LV_OBJ_FLAG_SCROLLABLE);

    s_im.title = lv_label_create(s_im.mid);
    nl_ov_set_font(s_im.title);
    lv_label_set_long_mode(s_im.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_im.title, kMidW);
    lv_obj_set_style_text_color(s_im.title, lv_color_hex(kImFg), 0);
    lv_obj_set_style_text_align(s_im.title, LV_TEXT_ALIGN_CENTER, 0);

    s_im.artist = lv_label_create(s_im.mid);
    nl_ov_set_font(s_im.artist);
    lv_label_set_long_mode(s_im.artist, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_im.artist, kMidW);
    lv_obj_set_style_text_color(s_im.artist, lv_color_hex(kImFgMuted), 0);
    lv_obj_set_style_text_align(s_im.artist, LV_TEXT_ALIGN_CENTER, 0);

    s_im.btn_row = lv_obj_create(s_im.mid);
    lv_obj_remove_style_all(s_im.btn_row);
    lv_obj_set_width(s_im.btn_row, kMidW - 16);
    lv_obj_set_height(s_im.btn_row, kBtnPri);
    lv_obj_set_style_bg_opa(s_im.btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_im.btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_im.btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_im.btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_im.btn_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_SKIP_PREVIOUS, on_ctrl_prev, kBtnSec, false);
    s_im.pause_btn =
        make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_PAUSE, on_ctrl_pause, kBtnPri, true);
    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_SKIP_NEXT, on_ctrl_next, kBtnSec, false);
    s_im.fav_btn = make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_FAVORITE, on_ctrl_fav, kBtnSec, false);
    (void)make_icon_btn(s_im.btn_row, MATERIAL_SYMBOLS_STOP, on_ctrl_stop, kBtnSec, false);

    s_im.slider = lv_slider_create(s_im.mid);
    lv_obj_set_width(s_im.slider, kMidW - 24); /* 留边，避免旋钮/左端被邻列挡住 */
    lv_obj_set_height(s_im.slider, 14);
    lv_slider_set_range(s_im.slider, 0, 1000);
    lv_obj_set_style_bg_color(s_im.slider, lv_color_hex(kImSliderTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_im.slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_im.slider, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_im.slider, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_im.slider, UI_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_im.slider, 6, LV_PART_KNOB);
    lv_obj_clear_flag(s_im.slider, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_im.slider, on_slider_changed, LV_EVENT_ALL, nullptr);

    s_im.time_lbl = lv_label_create(s_im.mid);
    nl_ov_set_font(s_im.time_lbl);
    lv_obj_set_style_text_color(s_im.time_lbl, lv_color_hex(kImFgMuted), 0);
    lv_obj_set_style_text_align(s_im.time_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* 右：歌词 */
    s_im.right = lv_obj_create(s_im.root);
    lv_obj_remove_style_all(s_im.right);
    lv_obj_set_flex_grow(s_im.right, 1);
    lv_obj_set_height(s_im.right, lv_pct(100));
    lv_obj_set_style_bg_opa(s_im.right, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_im.right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_im.right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_im.right, 6, 0);
    lv_obj_set_style_pad_hor(s_im.right, 8, 0);
    lv_obj_clear_flag(s_im.right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_im.right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_im.right, on_lyric_long, LV_EVENT_LONG_PRESSED, nullptr);

    for (int i = 0; i < kLyricVis; i++) {
        s_im.lyric_rows[i] = lv_label_create(s_im.right);
        nl_ov_set_font(s_im.lyric_rows[i]);
        lv_label_set_long_mode(s_im.lyric_rows[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_im.lyric_rows[i], lv_pct(100));
        lv_obj_set_style_text_color(s_im.lyric_rows[i], lv_color_hex(kImFgMuted), 0);
        lv_obj_set_style_text_align(s_im.lyric_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_im.lyric_rows[i], "");
        lv_obj_add_flag(s_im.lyric_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_im.no_lyric = lv_label_create(s_im.right);
    nl_ov_set_font(s_im.no_lyric);
    lv_label_set_text(s_im.no_lyric, "暂无歌词\n长按此处可换源");
    lv_obj_set_style_text_color(s_im.no_lyric, lv_color_hex(kImFgTip), 0);
    lv_obj_set_style_text_align(s_im.no_lyric, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_im.no_lyric, 6, 0);
    /* 默认无歌词：仅此 label，靠 right 的 flex 垂直居中 */

    s_im.visible = true;
    s_im.vinyl_angle = 0;
    s_im.cover_on = false;
    if (!s_im.tick_tmr) {
        s_im.tick_tmr = lv_timer_create(tick_cb, kTickMs, nullptr);
    } else {
        lv_timer_resume(s_im.tick_tmr);
        lv_timer_reset(s_im.tick_tmr);
    }
    lv_obj_move_foreground(s_im.root);
    if (s_im.collapse_btn) {
        lv_obj_move_foreground(s_im.collapse_btn);
        lv_obj_align(s_im.collapse_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    }
    nl_ov_shot_fab_apply_state();
    paint_immersive_fg();
    paint_vinyl_color();
    refresh_chrome_locked();
    vinyl_spin_sync();
    request_cover_and_lyrics();
}

void on_now_playing_core(const char *title, bool playing)
{
    if (!title || !title[0]) {
        s_im.playing = false;
        s_im.title_buf[0] = '\0';
        hide_locked();
        return;
    }
    const bool title_changed = strcmp(s_im.title_buf, title) != 0;
    snprintf(s_im.title_buf, sizeof(s_im.title_buf), "%s", title);
    s_im.playing = playing || nodelio_media_is_paused();
    if (!immersive_allowed() || nodelio_skill_is_live_radio()) {
        hide_locked();
        return;
    }
    /* 不自动进沉浸；仅已在沉浸页时跟随切歌刷新 */
    if (s_im.visible) {
        if (title_changed) {
            s_im.cover_on = false;
            memset(&s_im.cover_dsc, 0, sizeof(s_im.cover_dsc));
            set_vinyl_cover_mode(false);
            apply_ambient(ambient_warm_fallback());
            paint_vinyl_color();
            request_cover_and_lyrics();
        }
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
}

extern "C" void nodelio_skill_immersive_show(void)
{
    if (!immersive_allowed() || nodelio_skill_is_live_radio()) {
        return;
    }
    if (!nodelio_media_is_playing() && !nodelio_media_is_paused()) {
        return;
    }
    if (s_im.visible) {
        return;
    }
    char title[NL_ENV_TITLE];
    nodelio_media_now_title(title, sizeof(title));
    if (!title[0]) {
        return;
    }
    snprintf(s_im.title_buf, sizeof(s_im.title_buf), "%s", title);
    s_im.playing = true;
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        show_locked();
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
    /* 已取消空闲自动进沉浸；保留符号供播放条旧调用。 */
}

extern "C" void nodelio_skill_immersive_apply_vinyl_pref(void)
{
    if (!s_im.visible) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        paint_vinyl_color();
        vinyl_spin_sync();
    }
}

/** 须在已持有显示锁时调用（media_now_playing 路径）。 */
extern "C" void nodelio_skill_immersive_on_now_playing(const char *title, bool playing)
{
    on_now_playing_core(title, playing);
}
