#include "sd_photo_ui.h"

#include "board.h"
#include "display.h"
#include "overlay_priv.h"
#include "panel_jobs.h"
#include "panel_ui.h"
#include "sd_library.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include "jpg/jpeg_to_image.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#define TAG "nl_sd_photo"

namespace {

constexpr size_t kJpegCap = 512 * 1024;

struct PhotoUi {
    lv_obj_t *root = nullptr;
    lv_obj_t *img = nullptr;
    lv_obj_t *caption = nullptr;
    lv_image_dsc_t dsc{};
    uint8_t *rgb = nullptr;
    int index = 0;
    bool busy = false;
};

PhotoUi s_ui;

void free_rgb()
{
    if (s_ui.rgb) {
        heap_caps_free(s_ui.rgb);
        s_ui.rgb = nullptr;
    }
    memset(&s_ui.dsc, 0, sizeof(s_ui.dsc));
}

void hide_locked()
{
    free_rgb();
    if (s_ui.root) {
        lv_obj_delete(s_ui.root);
        s_ui.root = nullptr;
        s_ui.img = nullptr;
        s_ui.caption = nullptr;
    }
    s_ui.index = 0;
}

void show_caption(const char *text)
{
    if (!s_ui.caption) {
        return;
    }
    lv_label_set_text(s_ui.caption, text ? text : "");
}

void apply_rgb(uint8_t *rgb, size_t rgb_len, size_t w, size_t h, size_t stride, int index,
               const char *title)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !s_ui.root || !s_ui.img) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return;
    }
    free_rgb();
    s_ui.rgb = rgb;
    s_ui.index = index;
    if (!rgb || w == 0 || h == 0) {
        show_caption(title && title[0] ? title : "无法显示");
        return;
    }
    s_ui.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_ui.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_ui.dsc.header.w = static_cast<uint32_t>(w);
    s_ui.dsc.header.h = static_cast<uint32_t>(h);
    s_ui.dsc.header.stride = static_cast<uint32_t>(stride ? stride : w * 2);
    s_ui.dsc.data_size = static_cast<uint32_t>(rgb_len);
    s_ui.dsc.data = rgb;
    lv_image_set_src(s_ui.img, &s_ui.dsc);
    char cap[128];
    snprintf(cap, sizeof(cap), "%d / %d  %s", index + 1, nodelio_sd_photo_count(),
             title ? title : "");
    show_caption(cap);
}

void load_index_worker(void *p)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(p));
    char path[160];
    if (!nodelio_sd_photo_path(index, path, sizeof(path))) {
        s_ui.busy = false;
        nodelio_panel_ui_toast_defer("没有这张图");
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        s_ui.busy = false;
        nodelio_panel_ui_toast_defer("读取失败");
        return;
    }
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpeg) {
        fclose(f);
        s_ui.busy = false;
        nodelio_panel_ui_toast_defer("内存不足");
        return;
    }
    const size_t jpeg_n = fread(jpeg, 1, kJpegCap, f);
    fclose(f);
    uint8_t *rgb = nullptr;
    size_t rgb_len = 0, w = 0, h = 0, stride = 0;
    esp_err_t err = ESP_FAIL;
    if (jpeg_n > 0) {
        err = jpeg_to_image(jpeg, jpeg_n, &rgb, &rgb_len, &w, &h, &stride);
    }
    heap_caps_free(jpeg);
    char title[96];
    const char *base = strrchr(path, '/');
    const char *name = (base && base[1]) ? base + 1 : path;
    size_t name_n = 0;
    while (name[name_n] && name_n + 1 < sizeof(title)) {
        title[name_n] = name[name_n];
        name_n++;
    }
    title[name_n] = '\0';
    char *dot = strrchr(title, '.');
    if (dot && dot != title) {
        *dot = '\0';
    }
    s_ui.busy = false;
    if (err != ESP_OK || !rgb) {
        apply_rgb(nullptr, 0, 0, 0, 0, index, "解码失败");
        return;
    }
    apply_rgb(rgb, rgb_len, w, h, stride, index, title);
}

void request_load(int index)
{
    const int n = nodelio_sd_photo_count();
    if (n <= 0) {
        nl_ov_toast("内存卡里没有照片");
        return;
    }
    if (index < 0) {
        index = n - 1;
    } else if (index >= n) {
        index = 0;
    }
    if (s_ui.busy) {
        return;
    }
    s_ui.busy = true;
    show_caption("加载中…");
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Interactive, load_index_worker,
                                 reinterpret_cast<void *>(static_cast<intptr_t>(index)),
                                 [](void *) { s_ui.busy = false; }, 0, "sd_photo")) {
        s_ui.busy = false;
        nl_ov_toast("加载失败");
    }
}

void ensure_shell()
{
    if (s_ui.root) {
        return;
    }
    Display *disp = Board::GetInstance().GetDisplay();
    if (!disp) {
        return;
    }
    DisplayLockGuard lock(disp);
    if (!lock) {
        return;
    }
    lv_obj_t *layer = lv_layer_top();
    s_ui.root = lv_obj_create(layer);
    lv_obj_set_size(s_ui.root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.root, 0, 0);
    lv_obj_set_style_pad_all(s_ui.root, 0, 0);
    lv_obj_clear_flag(s_ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.root, LV_OBJ_FLAG_CLICKABLE);

    s_ui.img = lv_image_create(s_ui.root);
    lv_obj_center(s_ui.img);

    s_ui.caption = lv_label_create(s_ui.root);
    nl_ov_set_font(s_ui.caption);
    lv_obj_set_style_text_color(s_ui.caption, lv_color_white(), 0);
    lv_obj_align(s_ui.caption, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_label_set_text(s_ui.caption, "");

    lv_obj_t *close_btn = ui_make_text_btn(s_ui.root, "关闭", 1, [](lv_event_t *) {
        nodelio_skill_sd_photo_hide();
    });
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -12, 12);

    lv_obj_t *prev_btn = ui_make_text_btn(s_ui.root, "上一张", 1, [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        request_load(s_ui.index - 1);
    });
    lv_obj_align(prev_btn, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *next_btn = ui_make_text_btn(s_ui.root, "下一张", 1, [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        request_load(s_ui.index + 1);
    });
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(
        s_ui.root,
        [](lv_event_t *e) {
            if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
                return;
            }
            lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
            Board::GetInstance().NoteUserActivity();
            if (dir == LV_DIR_LEFT) {
                request_load(s_ui.index + 1);
            } else if (dir == LV_DIR_RIGHT) {
                request_load(s_ui.index - 1);
            }
        },
        LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(s_ui.root, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

}  // namespace

extern "C" void nodelio_skill_sd_photo_show(int index)
{
    if (nodelio_sd_photo_count() <= 0) {
        nl_ov_toast("内存卡里没有 JPG 照片");
        return;
    }
    ensure_shell();
    request_load(index);
}

extern "C" bool nodelio_skill_sd_photo_visible(void)
{
    return s_ui.root != nullptr;
}

extern "C" void nodelio_skill_sd_photo_hide(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        hide_locked();
        return;
    }
    hide_locked();
}
