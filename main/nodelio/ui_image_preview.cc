#include "ui_image_preview.h"

#include "board.h"
#include "board_info.h"
#include "display.h"
#include "jpg/jpeg_to_image.h"
#include "overlay_priv.h"
#include "panel_jobs.h"
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
constexpr size_t kDefaultMaxRgb = 512 * 1024;

struct State {
    lv_obj_t *root = nullptr;
    lv_obj_t *viewport = nullptr;
    lv_obj_t *side_left = nullptr;
    lv_obj_t *side_right = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *hint = nullptr;
    lv_obj_t *zoom_lbl = nullptr;
    uint8_t *rgb = nullptr;
    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    size_t max_rgb_bytes = kDefaultMaxRgb;
    lv_image_dsc_t dsc{};
    uint32_t gen = 1;
    int zoom = LV_SCALE_NONE;
    int view_w = 0;
    int view_h = 0;
    /** 100% 时 fit 后的显示尺寸；缩放相对它，不是相对整屏视口。 */
    int base_w = 0;
    int base_h = 0;
    bool allow_zoom = false;
    bool redecode_busy = false;
};

State s{};

void free_jpeg()
{
    if (s.jpeg) {
        heap_caps_free(s.jpeg);
        s.jpeg = nullptr;
    }
    s.jpeg_len = 0;
}

void free_rgb()
{
    if (s.rgb) {
        heap_caps_free(s.rgb);
        s.rgb = nullptr;
    }
    memset(&s.dsc, 0, sizeof(s.dsc));
}

void update_zoom_label()
{
    if (s.zoom_lbl && lv_obj_is_valid(s.zoom_lbl)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (s.zoom * 100 + 128) / 256);
        lv_label_set_text(s.zoom_lbl, buf);
    }
}

void place_image_1x1(uint32_t w, uint32_t h)
{
    if (!s.img || !lv_obj_is_valid(s.img) || !s.dsc.data) {
        return;
    }
    lv_image_set_scale(s.img, LV_SCALE_NONE);
    lv_image_set_inner_align(s.img, LV_IMAGE_ALIGN_TOP_LEFT);
    const int dw = static_cast<int>(w > 0 ? w : 1);
    const int dh = static_cast<int>(h > 0 ? h : 1);
    lv_obj_set_size(s.img, dw, dh);
    int x = 0;
    int y = 0;
    if (dw < s.view_w) {
        x = (s.view_w - dw) / 2;
    }
    if (dh < s.view_h) {
        y = (s.view_h - dh) / 2;
    }
    lv_obj_set_pos(s.img, x, y);
    update_zoom_label();
    if (s.viewport && lv_obj_is_valid(s.viewport)) {
        lv_obj_scroll_to(s.viewport, 0, 0, LV_ANIM_OFF);
        lv_obj_update_layout(s.viewport);
    }
}

bool apply_rgb_locked(uint8_t *rgb, uint32_t w, uint32_t h, uint32_t stride, size_t data_size)
{
    if (!rgb || w < 1 || h < 1 || !s.root || !s.img || !lv_obj_is_valid(s.img)) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return false;
    }
    free_rgb();
    s.rgb = rgb;
    s.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s.dsc.header.w = w;
    s.dsc.header.h = h;
    s.dsc.header.stride = stride ? stride : (w * 2);
    s.dsc.data_size = static_cast<uint32_t>(data_size);
    s.dsc.data = rgb;
    lv_image_set_src(s.img, &s.dsc);
    lv_obj_clear_flag(s.img, LV_OBJ_FLAG_HIDDEN);
    place_image_1x1(w, h);
    if (s.hint && lv_obj_is_valid(s.hint)) {
        lv_obj_add_flag(s.hint, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

void decode_target_size(int *max_w, int *max_h)
{
    /* 有 base 时相对图本身缩放；尚未上图时用视口（初次 fit）。 */
    const int bw = s.base_w > 0 ? s.base_w : s.view_w;
    const int bh = s.base_h > 0 ? s.base_h : s.view_h;
    int zw = (bw * s.zoom + 128) / 256;
    int zh = (bh * s.zoom + 128) / 256;
    if (zw < 8) {
        zw = 8;
    }
    if (zh < 8) {
        zh = 8;
    }
    *max_w = zw;
    *max_h = zh;
}

/** Nearest-neighbor upscale RGB565 once (not per-frame STRETCH). */
uint8_t *scale_rgb565_nn(const uint8_t *src, size_t src_w, size_t src_h, size_t src_stride,
                         size_t dst_w, size_t dst_h, size_t *out_len)
{
    if (!src || src_w < 1 || src_h < 1 || dst_w < 1 || dst_h < 1 || !out_len) {
        return nullptr;
    }
    if (src_stride < src_w * 2) {
        src_stride = src_w * 2;
    }
    const size_t bytes = dst_w * dst_h * 2;
    uint8_t *dst = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dst) {
        dst = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT));
    }
    if (!dst) {
        return nullptr;
    }
    for (size_t y = 0; y < dst_h; ++y) {
        const size_t sy = (y * src_h) / dst_h;
        const uint8_t *row = src + sy * src_stride;
        uint8_t *out = dst + y * dst_w * 2;
        for (size_t x = 0; x < dst_w; ++x) {
            const size_t sx = (x * src_w) / dst_w;
            const uint8_t *px = row + sx * 2;
            out[x * 2] = px[0];
            out[x * 2 + 1] = px[1];
        }
    }
    *out_len = bytes;
    return dst;
}

/**
 * Decode JPEG fitted to max_w/max_h; if still smaller than target, one-shot upscale
 * so zoom-in past native/fit size still has visible effect.
 */
bool decode_jpeg_for_zoom(const uint8_t *jpeg, size_t jpeg_len, int target_w, int target_h,
                          size_t max_rgb_bytes, uint8_t **out_rgb, size_t *out_len, size_t *out_w,
                          size_t *out_h, size_t *out_stride)
{
    if (!jpeg || jpeg_len == 0 || !out_rgb || !out_len || !out_w || !out_h || !out_stride) {
        return false;
    }
    if (target_w < 8) {
        target_w = 8;
    }
    if (target_h < 8) {
        target_h = 8;
    }
    uint8_t *rgb = nullptr;
    size_t rgb_len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image_fit(jpeg, jpeg_len, target_w, target_h, max_rgb_bytes, &rgb, &rgb_len, &w, &h,
                          &stride) != ESP_OK ||
        !rgb) {
        return false;
    }
    const size_t tw = static_cast<size_t>(target_w);
    const size_t th = static_cast<size_t>(target_h);
    /* 源图不够大时一次性放大到目标；目标已是 base×zoom，勿再撑满视口。 */
    if (w < tw || h < th) {
        size_t up_len = 0;
        uint8_t *up = scale_rgb565_nn(rgb, w, h, stride, tw, th, &up_len);
        heap_caps_free(rgb);
        if (!up) {
            return false;
        }
        rgb = up;
        rgb_len = up_len;
        w = tw;
        h = th;
        stride = tw * 2;
    }
    *out_rgb = rgb;
    *out_len = rgb_len;
    *out_w = w;
    *out_h = h;
    *out_stride = stride;
    return true;
}

struct RedecodeJob {
    uint32_t gen = 0;
    int zoom = LV_SCALE_NONE;
    int max_w = 8;
    int max_h = 8;
    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    size_t max_rgb_bytes = kDefaultMaxRgb;
};

void redecode_job_run(void *p)
{
    auto *job = static_cast<RedecodeJob *>(p);
    uint8_t *rgb = nullptr;
    size_t rgb_len = 0, w = 0, h = 0, stride = 0;
    if (job->jpeg && job->jpeg_len > 0) {
        (void)decode_jpeg_for_zoom(job->jpeg, job->jpeg_len, job->max_w, job->max_h,
                                   job->max_rgb_bytes, &rgb, &rgb_len, &w, &h, &stride);
    }

    DisplayLockGuard lock(nl_ov_disp());
    s.redecode_busy = false;
    if (!lock || job->gen != s.gen) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        if (job->jpeg) {
            heap_caps_free(job->jpeg);
        }
        delete job;
        return;
    }
    s.zoom = job->zoom;
    if (rgb) {
        (void)apply_rgb_locked(rgb, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                               static_cast<uint32_t>(stride), rgb_len);
    }
    update_zoom_label();
    if (job->jpeg) {
        heap_caps_free(job->jpeg);
    }
    delete job;
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

void zoom_by(int delta)
{
    Board::GetInstance().NoteUserActivity();
    if (!s.allow_zoom || s.redecode_busy) {
        return;
    }
    if (!s.jpeg || s.jpeg_len == 0) {
        nl_ov_toast("无法缩放");
        return;
    }
    int next = s.zoom + delta;
    if (next < kZoomMin) {
        next = kZoomMin;
    }
    if (next > kZoomMax) {
        next = kZoomMax;
    }
    if (next == s.zoom) {
        return;
    }

    const int prev_zoom = s.zoom;
    s.zoom = next;
    int max_w = 8;
    int max_h = 8;
    decode_target_size(&max_w, &max_h);
    s.zoom = prev_zoom;

    auto *job = new RedecodeJob();
    job->gen = s.gen;
    job->zoom = next;
    job->max_w = max_w;
    job->max_h = max_h;
    job->max_rgb_bytes = s.max_rgb_bytes;
    job->jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(s.jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job->jpeg) {
        delete job;
        return;
    }
    memcpy(job->jpeg, s.jpeg, s.jpeg_len);
    job->jpeg_len = s.jpeg_len;
    s.redecode_busy = true;
    s.zoom = next;
    update_zoom_label();
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Interactive, redecode_job_run, job,
                                 [](void *p) {
                                     auto *j = static_cast<RedecodeJob *>(p);
                                     if (j->jpeg) {
                                         heap_caps_free(j->jpeg);
                                     }
                                     delete j;
                                     s.redecode_busy = false;
                                 },
                                 0, "preview_zoom")) {
        s.redecode_busy = false;
        s.zoom = prev_zoom;
        update_zoom_label();
        heap_caps_free(job->jpeg);
        delete job;
    }
}

}  // namespace

extern "C" void nl_ui_image_preview_hide(void)
{
    s.gen += 1;
    s.zoom = LV_SCALE_NONE;
    s.view_w = 0;
    s.view_h = 0;
    s.base_w = 0;
    s.base_h = 0;
    s.allow_zoom = false;
    s.redecode_busy = false;
    s.max_rgb_bytes = kDefaultMaxRgb;
    free_rgb();
    free_jpeg();
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
    s.zoom = LV_SCALE_NONE;

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
    if (!s.root || gen != s.gen) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return false;
    }
    free_jpeg();
    const bool ok = apply_rgb_locked(rgb, w, h, stride, data_size);
    if (ok) {
        raise_controls();
    }
    return ok;
}

extern "C" bool nl_ui_image_preview_set_jpeg(uint32_t gen, uint8_t *jpeg, size_t jpeg_len,
                                             size_t max_rgb_bytes)
{
    if (!jpeg || jpeg_len == 0) {
        if (jpeg) {
            heap_caps_free(jpeg);
        }
        return false;
    }

    int max_w = 8;
    int max_h = 8;
    size_t cap = max_rgb_bytes > 64 ? max_rgb_bytes : kDefaultMaxRgb;
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock || !s.root || gen != s.gen) {
            heap_caps_free(jpeg);
            return false;
        }
        s.zoom = LV_SCALE_NONE;
        decode_target_size(&max_w, &max_h);
        cap = max_rgb_bytes > 64 ? max_rgb_bytes : kDefaultMaxRgb;
    }

    /* 初次 100%：只 fit 不放大；放大按钮走 decode_jpeg_for_zoom 一次性放大 */
    uint8_t *rgb = nullptr;
    size_t rgb_len = 0, w = 0, h = 0, stride = 0;
    if (jpeg_to_image_fit(jpeg, jpeg_len, max_w, max_h, cap, &rgb, &rgb_len, &w, &h, &stride) !=
            ESP_OK ||
        !rgb) {
        heap_caps_free(jpeg);
        return false;
    }

    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s.root || gen != s.gen) {
        heap_caps_free(rgb);
        heap_caps_free(jpeg);
        return false;
    }
    free_jpeg();
    s.jpeg = jpeg;
    s.jpeg_len = jpeg_len;
    s.max_rgb_bytes = cap;
    s.zoom = LV_SCALE_NONE;
    const bool ok = apply_rgb_locked(rgb, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                     static_cast<uint32_t>(stride), rgb_len);
    if (ok) {
        s.base_w = static_cast<int>(w);
        s.base_h = static_cast<int>(h);
        raise_controls();
    } else {
        s.base_w = 0;
        s.base_h = 0;
        free_jpeg();
    }
    return ok;
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
