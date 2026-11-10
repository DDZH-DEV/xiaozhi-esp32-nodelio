#include "ui_image_preview.h"

#include "board.h"
#include "board_info.h"
#include "display.h"
#include "overlay_priv.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include <esp_heap_caps.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kZoomMin = 128;  /* 50% */
constexpr int kZoomMax = 768;  /* 300% */
constexpr int kZoomStep = 64;
constexpr int kSideBtnW = 72;
constexpr int kSideBtnH = 44;

struct State {
    lv_obj_t *root = nullptr;
    lv_obj_t *viewport = nullptr;
    lv_obj_t *side_left = nullptr;
    lv_obj_t *side_right = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *hint = nullptr;
    lv_obj_t *zoom_lbl = nullptr;
    uint8_t *rgb = nullptr;
    lv_image_dsc_t dsc{};
    uint32_t gen = 1;
    int zoom = LV_SCALE_NONE;
    int view_w = 0;
    int view_h = 0;
    bool allow_zoom = false;
};

State s{};

int fit_zoom()
{
    const int bw = static_cast<int>(s.dsc.header.w);
    const int bh = static_cast<int>(s.dsc.header.h);
    if (bw < 1 || bh < 1 || s.view_w < 8 || s.view_h < 8) {
        return LV_SCALE_NONE;
    }
    const int zx = (s.view_w * 256) / bw;
    const int zy = (s.view_h * 256) / bh;
    int z = zx < zy ? zx : zy;
    if (z < kZoomMin) {
        z = kZoomMin;
    }
    if (z > kZoomMax) {
        z = kZoomMax;
    }
    return z;
}

void apply_zoom()
{
    if (!s.img || !lv_obj_is_valid(s.img) || !s.dsc.data) {
        return;
    }
    if (s.zoom < kZoomMin) {
        s.zoom = kZoomMin;
    }
    if (s.zoom > kZoomMax) {
        s.zoom = kZoomMax;
    }
    const int bw = static_cast<int>(s.dsc.header.w);
    const int bh = static_cast<int>(s.dsc.header.h);
    const int dw = (bw * s.zoom + 128) / 256;
    const int dh = (bh * s.zoom + 128) / 256;
    lv_image_set_scale(s.img, LV_SCALE_NONE);
    lv_image_set_inner_align(s.img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(s.img, dw > 1 ? dw : 1, dh > 1 ? dh : 1);
    int x = 0;
    int y = 0;
    if (dw < s.view_w) {
        x = (s.view_w - dw) / 2;
    }
    if (dh < s.view_h) {
        y = (s.view_h - dh) / 2;
    }
    lv_obj_set_pos(s.img, x, y);
    if (s.zoom_lbl && lv_obj_is_valid(s.zoom_lbl)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (s.zoom * 100 + 128) / 256);
        lv_label_set_text(s.zoom_lbl, buf);
    }
    if (s.viewport && lv_obj_is_valid(s.viewport)) {
        lv_obj_scroll_to(s.viewport, 0, 0, LV_ANIM_OFF);
        lv_obj_update_layout(s.viewport);
    }
}

void zoom_by(int delta)
{
    Board::GetInstance().NoteUserActivity();
    if (!s.allow_zoom || !s.dsc.data) {
        return;
    }
    s.zoom += delta;
    apply_zoom();
}

void raise_controls()
{
    if (s.root && lv_obj_is_valid(s.root)) {
        lv_obj_move_foreground(s.root);
    }
    if (s.side_left && lv_obj_is_valid(s.side_left)) {
        lv_obj_move_foreground(s.side_left);
    }
    if (s.side_right && lv_obj_is_valid(s.side_right)) {
        lv_obj_move_foreground(s.side_right);
    }
    nl_ov_raise_chrome();
}

void style_close_btn(lv_obj_t *btn)
{
    if (!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, UI_DANGER_BG, 0);
    if (lv_obj_t *lb = lv_obj_get_child(btn, 0)) {
        lv_obj_set_style_text_color(lb, UI_DANGER_FG, 0);
    }
}

}  // namespace

extern "C" void nl_ui_image_preview_hide(void)
{
    s.gen += 1;
    s.zoom = LV_SCALE_NONE;
    s.view_w = 0;
    s.view_h = 0;
    s.allow_zoom = false;
    if (s.rgb) {
        heap_caps_free(s.rgb);
        s.rgb = nullptr;
    }
    memset(&s.dsc, 0, sizeof(s.dsc));
    if (s.root) {
        lv_obj_delete(s.root);
        s.root = nullptr;
        s.viewport = nullptr;
        s.side_left = nullptr;
        s.side_right = nullptr;
        s.img = nullptr;
        s.hint = nullptr;
        s.zoom_lbl = nullptr;
    }
}

extern "C" uint32_t nl_ui_image_preview_gen(void)
{
    return s.root ? s.gen : 0;
}

extern "C" bool nl_ui_image_preview_visible(void)
{
    return s.root != nullptr;
}

extern "C" bool nl_ui_image_preview_view_size(int *w, int *h)
{
    if (!s.root || s.view_w < 8 || s.view_h < 8) {
        return false;
    }
    if (w) {
        *w = s.view_w;
    }
    if (h) {
        *h = s.view_h;
    }
    return true;
}

extern "C" void nl_ui_image_preview_raise(void)
{
    raise_controls();
}

extern "C" uint32_t nl_ui_image_preview_open(const nl_ui_image_preview_opts_t *opts)
{
    nl_ui_image_preview_opts_t o{};
    if (opts) {
        o = *opts;
    }

    lv_obj_t *layer = lv_layer_top();
    if (!layer) {
        return 0;
    }

    const auto *profile = nodelio_display_profile();
    int box_w = o.w > 0 ? o.w : (profile ? (int)profile->width : LV_HOR_RES);
    int box_h = o.h > 0 ? o.h : (profile ? (int)profile->height : LV_VER_RES);
    if (box_w < 80) {
        box_w = 80;
    }
    if (box_h < 80) {
        box_h = 80;
    }

    Board::GetInstance().NoteUserActivity();
    nl_ui_image_preview_hide();

    s.allow_zoom = o.allow_zoom;
    s.view_w = box_w;
    s.view_h = box_h;

    s.root = lv_obj_create(layer);
    lv_obj_set_size(s.root, box_w, box_h);
    lv_obj_set_pos(s.root, o.x, o.y);
    lv_obj_set_style_bg_color(s.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.root, 0, 0);
    lv_obj_set_style_pad_all(s.root, 0, 0);
    lv_obj_set_style_radius(s.root, 0, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    if (o.close_on_blank) {
        lv_obj_add_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            s.root,
            [](lv_event_t *e) {
                if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                    return;
                }
                if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
                    return;
                }
                Board::GetInstance().NoteUserActivity();
                nl_ui_image_preview_hide();
            },
            LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_clear_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
    }

    s.viewport = lv_obj_create(s.root);
    lv_obj_set_size(s.viewport, s.view_w, s.view_h);
    lv_obj_align(s.viewport, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s.viewport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.viewport, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.viewport, 0, 0);
    lv_obj_set_style_pad_all(s.viewport, 0, 0);
    lv_obj_set_style_radius(s.viewport, 0, 0);
    lv_obj_set_scroll_dir(s.viewport, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(s.viewport, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s.viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.viewport, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(s.viewport, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s.viewport, LV_OBJ_FLAG_SCROLL_ELASTIC);

    s.hint = lv_label_create(s.viewport);
    nl_ov_set_font(s.hint);
    lv_label_set_text(s.hint, o.loading_text && o.loading_text[0] ? o.loading_text : "加载中…");
    lv_obj_set_style_text_color(s.hint, lv_color_white(), 0);
    lv_obj_center(s.hint);

    s.img = lv_image_create(s.viewport);
    lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s.img, LV_OBJ_FLAG_EVENT_BUBBLE);

    if (s.allow_zoom) {
        s.side_left = lv_obj_create(s.root);
        lv_obj_set_size(s.side_left, kSideBtnW + 16, LV_SIZE_CONTENT);
        lv_obj_align(s.side_left, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_opa(s.side_left, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s.side_left, 0, 0);
        lv_obj_set_style_pad_all(s.side_left, 0, 0);
        lv_obj_set_style_pad_row(s.side_left, 10, 0);
        lv_obj_set_style_radius(s.side_left, 0, 0);
        lv_obj_set_flex_flow(s.side_left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s.side_left, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(s.side_left, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s.side_left, LV_OBJ_FLAG_CLICKABLE);

        (void)ui_make_action_btn(s.side_left, "放大", kSideBtnW, kSideBtnH, [](lv_event_t *) {
            zoom_by(kZoomStep);
        });

        s.zoom_lbl = lv_label_create(s.side_left);
        nl_ov_set_font(s.zoom_lbl);
        lv_label_set_text(s.zoom_lbl, "100%");
        lv_obj_set_style_text_color(s.zoom_lbl, lv_color_white(), 0);

        (void)ui_make_action_btn(s.side_left, "缩小", kSideBtnW, kSideBtnH, [](lv_event_t *) {
            zoom_by(-kZoomStep);
        });

        s.side_right = lv_obj_create(s.root);
        lv_obj_set_size(s.side_right, kSideBtnW + 16, LV_SIZE_CONTENT);
        lv_obj_align(s.side_right, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_set_style_bg_opa(s.side_right, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s.side_right, 0, 0);
        lv_obj_set_style_pad_all(s.side_right, 0, 0);
        lv_obj_set_style_radius(s.side_right, 0, 0);
        lv_obj_clear_flag(s.side_right, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s.side_right, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *close_btn =
            ui_make_action_btn(s.side_right, "关闭", kSideBtnW, kSideBtnH, [](lv_event_t *) {
                Board::GetInstance().NoteUserActivity();
                nl_ui_image_preview_hide();
            });
        style_close_btn(close_btn);
    } else {
        lv_obj_t *close_btn = ui_make_action_btn(s.root, "关闭", 96, 40, [](lv_event_t *) {
            Board::GetInstance().NoteUserActivity();
            nl_ui_image_preview_hide();
        });
        style_close_btn(close_btn);
        lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -12, 12);
    }

    raise_controls();
    return s.gen;
}

extern "C" bool nl_ui_image_preview_set_rgb565(uint32_t gen, uint8_t *rgb, uint32_t w, uint32_t h,
                                               uint32_t stride, size_t data_size)
{
    if (!rgb || w < 1 || h < 1 || !s.root || !s.img || !lv_obj_is_valid(s.img) || gen != s.gen) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return false;
    }
    if (s.rgb) {
        heap_caps_free(s.rgb);
    }
    s.rgb = rgb;
    memset(&s.dsc, 0, sizeof(s.dsc));
    s.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s.dsc.header.w = w;
    s.dsc.header.h = h;
    s.dsc.header.stride = stride ? stride : (w * 2);
    s.dsc.data_size = static_cast<uint32_t>(data_size);
    s.dsc.data = rgb;
    lv_image_set_src(s.img, &s.dsc);
    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    s.zoom = s.allow_zoom ? fit_zoom() : LV_SCALE_NONE;
    apply_zoom();
    if (s.hint && lv_obj_is_valid(s.hint)) {
        lv_obj_add_flag(s.hint, LV_OBJ_FLAG_HIDDEN);
    }
    raise_controls();
    return true;
}

extern "C" void nl_ui_image_preview_fail(uint32_t gen, const char *msg)
{
    if (!s.root || gen != s.gen) {
        return;
    }
    const char *text = msg && msg[0] ? msg : "无法预览";
    if (s.hint && lv_obj_is_valid(s.hint)) {
        lv_label_set_text(s.hint, text);
        lv_obj_clear_flag(s.hint, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_toast(text);
}
