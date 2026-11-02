#include "overlay_priv.h"
#include "panel_jobs.h"

#include "application.h"
#include "board_info.h"
#include "display.h"
#include "jpg/jpeg_to_image.h"
#include "layout_cache.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_provision.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define NL_PHOTO_RT_MAX 8
typedef struct {
    char widget_id[NL_LAYOUT_STR];
    uint8_t idx;
    uint8_t paused;
    uint16_t interval_sec;
    int64_t next_ms;
} photo_rt_t;
static photo_rt_t s_photo_rt[NL_PHOTO_RT_MAX];
static int s_photo_rt_n;
static lv_timer_t *s_photo_timer;

/** 轮播 JPEG 常驻 PSRAM：切张先吃内存，避免每次 HTTP */
#define NL_PHOTO_BLOB_MAX 8
typedef struct {
    char id[NL_PHOTO_ID_MAX];
    uint8_t *data;
    size_t len;
} photo_blob_t;
static photo_blob_t s_photo_blobs[NL_PHOTO_BLOB_MAX];

void nl_ov_photo_reset_rt(void)
{
    s_photo_rt_n = 0;
}

static void photo_blob_free_slot(photo_blob_t *b)
{
    if (!b) {
        return;
    }
    if (b->data) {
        heap_caps_free(b->data);
        b->data = nullptr;
    }
    b->len = 0;
    b->id[0] = '\0';
}

static photo_blob_t *photo_blob_find(const char *id)
{
    if (!id || !id[0]) {
        return nullptr;
    }
    for (int i = 0; i < NL_PHOTO_BLOB_MAX; i++) {
        if (s_photo_blobs[i].data && s_photo_blobs[i].len > 0 && strcmp(s_photo_blobs[i].id, id) == 0) {
            return &s_photo_blobs[i];
        }
    }
    return nullptr;
}

static bool photo_blob_has(const char *id)
{
    return photo_blob_find(id) != nullptr;
}

static void photo_blob_put(const char *id, const uint8_t *data, size_t len)
{
    if (!id || !id[0] || !data || len == 0) {
        return;
    }
    photo_blob_t *slot = photo_blob_find(id);
    if (!slot) {
        for (int i = 0; i < NL_PHOTO_BLOB_MAX; i++) {
            if (!s_photo_blobs[i].data) {
                slot = &s_photo_blobs[i];
                break;
            }
        }
    }
    if (!slot) {
        /* 满则挤掉第 0 槽 */
        slot = &s_photo_blobs[0];
        photo_blob_free_slot(slot);
    }
    uint8_t *copy = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!copy) {
        return;
    }
    memcpy(copy, data, len);
    if (slot->data) {
        heap_caps_free(slot->data);
    }
    slot->data = copy;
    slot->len = len;
    snprintf(slot->id, sizeof(slot->id), "%s", id);
}

static void photo_blob_prune_keep(const char *const *ids, size_t n)
{
    for (int i = 0; i < NL_PHOTO_BLOB_MAX; i++) {
        if (!s_photo_blobs[i].data) {
            continue;
        }
        bool keep = false;
        for (size_t k = 0; k < n; k++) {
            if (ids[k] && strcmp(ids[k], s_photo_blobs[i].id) == 0) {
                keep = true;
                break;
            }
        }
        if (!keep) {
            photo_blob_free_slot(&s_photo_blobs[i]);
        }
    }
}

static void on_photo_img_deleted(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    auto *dsc = static_cast<lv_image_dsc_t *>(lv_event_get_user_data(e));
    if (!dsc) {
        return;
    }
    if (dsc->data) {
        heap_caps_free(const_cast<uint8_t *>(dsc->data));
        dsc->data = nullptr;
    }
    free(dsc);
}

static lv_obj_t *find_home_tile_by_widget_id(const char *widget_id)
{
    if (!s_home || !widget_id || !widget_id[0]) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(s_home, i);
        if (!ch) {
            continue;
        }
        auto *w = static_cast<const nl_widget_t *>(lv_obj_get_user_data(ch));
        if (w && w->type == NL_WIDGET_PHOTO_FRAME && strcmp(w->id, widget_id) == 0) {
            return ch;
        }
    }
    return nullptr;
}

static void photo_nav_raise(lv_obj_t *tile)
{
    if (!tile) {
        return;
    }
    lv_obj_update_layout(tile);
    /* 未出图时 layout 宽高可能仍为 0；回退到 place_grid_obj 写入的 style 尺寸，
     * 否则右钮会落到 x=10（看起来像叠在左上角）。 */
    int32_t tw = lv_obj_get_width(tile);
    int32_t th = lv_obj_get_height(tile);
    if (tw <= 0) {
        tw = lv_obj_get_style_width(tile, LV_PART_MAIN);
    }
    if (th <= 0) {
        th = lv_obj_get_style_height(tile, LV_PART_MAIN);
    }
    const int32_t btn = 48;
    const int32_t y = (th > btn) ? ((th - btn) / 2) : 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(tile); i++) {
        lv_obj_t *ch = lv_obj_get_child(tile, i);
        if (!ch || !lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) {
            continue;
        }
        lv_obj_add_flag(ch, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
        const intptr_t delta = (intptr_t)lv_obj_get_user_data(ch);
        if (delta < 0) {
            lv_obj_set_pos(ch, 10, y);
        } else {
            lv_obj_set_pos(ch, (tw > btn + 10) ? (tw - btn - 10) : 10, y);
        }
        lv_obj_move_foreground(ch);
    }
}

static void photo_apply_rgb(const char *widget_id, uint8_t *rgb, size_t rgb_len, size_t w, size_t h,
                            size_t stride, bool fit_contain, uint32_t gen)
{
    if (gen != s_home_gen || !s_home || s_active_tab != NL_TAB_HOME) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return;
    }
    lv_obj_t *tile = find_home_tile_by_widget_id(widget_id);
    if (!tile || !rgb || w == 0 || h == 0) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        if (tile) {
            lv_obj_t *hint = lv_obj_get_child(tile, 0);
            if (hint && lv_obj_check_type(hint, &lv_label_class)) {
                lv_label_set_text(hint, "加载失败");
                lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
                lv_obj_center(hint);
            }
            photo_nav_raise(tile);
        }
        return;
    }
    /* 清掉占位/旧图，保留 USER_1 导航钮 */
    uint32_t ci = 0;
    while (ci < lv_obj_get_child_count(tile)) {
        lv_obj_t *ch = lv_obj_get_child(tile, ci);
        if (ch && lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) {
            ci++;
            continue;
        }
        lv_obj_del(ch);
    }
    auto *dsc = static_cast<lv_image_dsc_t *>(calloc(1, sizeof(lv_image_dsc_t)));
    if (!dsc) {
        heap_caps_free(rgb);
        lv_obj_t *lb = lv_label_create(tile);
        nl_ov_set_font(lb);
        lv_label_set_text(lb, "内存不足");
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
        lv_obj_center(lb);
        return;
    }
    dsc->data = rgb;
    dsc->data_size = rgb_len;
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (uint32_t)w;
    dsc->header.h = (uint32_t)h;
    dsc->header.stride = (uint32_t)stride;

    lv_obj_t *img = lv_image_create(tile);
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
    lv_obj_center(img);
    lv_obj_set_style_radius(img, UI_RADIUS_MD, 0);
    lv_obj_set_style_clip_corner(img, true, 0);
    lv_image_set_src(img, dsc);
    lv_image_set_inner_align(img, fit_contain ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, on_photo_img_deleted, LV_EVENT_DELETE, dsc);
    photo_nav_raise(tile);
}

static photo_rt_t *photo_rt_find(const char *widget_id)
{
    if (!widget_id || !widget_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_photo_rt_n; i++) {
        if (strcmp(s_photo_rt[i].widget_id, widget_id) == 0) {
            return &s_photo_rt[i];
        }
    }
    return nullptr;
}

static void photo_show_loading(const char *widget_id, const char *text)
{
    lv_obj_t *tile = find_home_tile_by_widget_id(widget_id);
    if (!tile) {
        return;
    }
    uint32_t ci = 0;
    while (ci < lv_obj_get_child_count(tile)) {
        lv_obj_t *ch = lv_obj_get_child(tile, ci);
        if (ch && lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) {
            ci++;
            continue;
        }
        lv_obj_del(ch);
    }
    lv_obj_t *lb = lv_label_create(tile);
    nl_ov_set_font(lb);
    lv_label_set_text(lb, text ? text : "加载中");
    lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    lv_obj_center(lb);
    photo_nav_raise(tile);
}

void nl_ov_photo_cache_prune_layout(void)
{
    if (!s_layout) {
        return;
    }
    constexpr size_t kMaxKeep = 48;
    const char *ids[kMaxKeep];
    size_t n = 0;
    for (int pi = 0; pi < s_layout->page_count && n < kMaxKeep; pi++) {
        const nl_page_t *pg = &s_layout->pages[pi];
        if (pg->system_page[0] != '\0') {
            continue;
        }
        for (int si = 0; si < pg->section_count && n < kMaxKeep; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count && n < kMaxKeep; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->type != NL_WIDGET_PHOTO_FRAME) {
                    continue;
                }
                for (uint8_t i = 0; i < w->photo_n && n < kMaxKeep; i++) {
                    if (w->photo_ids[i][0]) {
                        ids[n++] = w->photo_ids[i];
                    }
                }
            }
        }
    }
    photo_blob_prune_keep(ids, n);
    (void)nodelio_photo_cache_prune(ids, n);
}

static void photo_fetch_async(const nl_widget_t *w, uint8_t idx)
{
    if (!w || w->photo_n == 0 || idx >= w->photo_n || !w->photo_ids[idx][0]) {
        return;
    }
    struct photo_arg {
        nodelio_nvs_config_t cfg;
        char widget_id[NL_LAYOUT_STR];
        char photo_id[NL_PHOTO_ID_MAX];
        uint8_t fit_contain;
        uint32_t gen;
    };
    auto *arg = static_cast<photo_arg *>(malloc(sizeof(photo_arg)));
    if (!arg) {
        return;
    }
    nl_ov_cfg_for_http(&arg->cfg);
    snprintf(arg->widget_id, sizeof(arg->widget_id), "%s", w->id);
    snprintf(arg->photo_id, sizeof(arg->photo_id), "%s", w->photo_ids[idx]);
    arg->fit_contain = w->photo_fit_contain;
    arg->gen = s_home_gen;

    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto *a = static_cast<photo_arg *>(p);
                {
                    DisplayLockGuard lock(nl_ov_disp());
                    if (!lock || a->gen != s_home_gen) {
                        free(a);
                        return;
                    }
                }
                constexpr int kJpegCap = 90 * 1024;
                uint8_t *jpeg = nullptr;
                size_t jpeg_n = 0;
                int status = 0;
                esp_err_t err = ESP_FAIL;
                bool own_jpeg = false;
                bool from_flash = false;

                /* 1) PSRAM 热缓存（拷贝，防并发 put 释放） */
                photo_blob_t *blob = photo_blob_find(a->photo_id);
                if (blob && blob->data && blob->len > 0) {
                    jpeg = static_cast<uint8_t *>(
                        heap_caps_malloc(blob->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (jpeg) {
                        memcpy(jpeg, blob->data, blob->len);
                        jpeg_n = blob->len;
                        own_jpeg = true;
                        err = ESP_OK;
                        status = 200;
                    }
                }

                auto try_http = [&]() {
                    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip() || !a->cfg.server_url[0] ||
                        !a->cfg.device_id[0]) {
                        return;
                    }
                    if (!jpeg || !own_jpeg) {
                        jpeg = static_cast<uint8_t *>(
                            heap_caps_malloc((size_t)kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                        own_jpeg = jpeg != nullptr;
                        if (!jpeg) {
                            return;
                        }
                    }
                    char base[160];
                    nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                    char url[420];
                    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/photos/%s", base, a->cfg.device_id,
                             a->photo_id);
                    err = nodelio_http_get_bin(url, a->cfg.device_key, &status, jpeg, kJpegCap, &jpeg_n);
                    from_flash = false;
                    if (err == ESP_OK && status == 200 && jpeg_n > 0) {
                        photo_blob_put(a->photo_id, jpeg, jpeg_n);
                        (void)nodelio_photo_cache_save(a->photo_id, jpeg, jpeg_n);
                    } else if (status == 404) {
                        (void)nodelio_photo_cache_remove(a->photo_id);
                    }
                };

                /* 2) SPIFFS → 3) HTTP */
                if (!(err == ESP_OK && status == 200 && jpeg_n > 0)) {
                    jpeg = static_cast<uint8_t *>(
                        heap_caps_malloc((size_t)kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    own_jpeg = jpeg != nullptr;
                    if (jpeg) {
                        if (nodelio_photo_cache_load(a->photo_id, jpeg, (size_t)kJpegCap, &jpeg_n) ==
                                ESP_OK &&
                            jpeg_n > 0) {
                            err = ESP_OK;
                            status = 200;
                            from_flash = true;
                            photo_blob_put(a->photo_id, jpeg, jpeg_n);
                        } else {
                            try_http();
                        }
                    }
                }

                uint8_t *rgb = nullptr;
                size_t rgb_len = 0, iw = 0, ih = 0, stride = 0;
                const char *fail_tip = nullptr;
                if (err == ESP_OK && status == 200 && jpeg_n > 0) {
                    if (jpeg_to_image(jpeg, jpeg_n, &rgb, &rgb_len, &iw, &ih, &stride) != ESP_OK) {
                        rgb = nullptr;
                        if (from_flash || photo_blob_find(a->photo_id)) {
                            (void)nodelio_photo_cache_remove(a->photo_id);
                            photo_blob_t *bad = photo_blob_find(a->photo_id);
                            if (bad) {
                                photo_blob_free_slot(bad);
                            }
                            jpeg_n = 0;
                            status = 0;
                            err = ESP_FAIL;
                            try_http();
                            if (err == ESP_OK && status == 200 && jpeg_n > 0) {
                                if (jpeg_to_image(jpeg, jpeg_n, &rgb, &rgb_len, &iw, &ih, &stride) !=
                                    ESP_OK) {
                                    rgb = nullptr;
                                    fail_tip = "解码失败";
                                }
                            }
                        } else {
                            fail_tip = "解码失败";
                        }
                    }
                }
                if (!rgb && !fail_tip) {
                    if (status == 401 || status == 403) {
                        fail_tip = "鉴权失败";
                    } else if (status == 404) {
                        fail_tip = "图已失效";
                    } else if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
                        fail_tip = "无缓存";
                    } else {
                        fail_tip = "加载失败";
                    }
                }
                if (own_jpeg && jpeg) {
                    heap_caps_free(jpeg);
                }
                std::string wid(a->widget_id);
                const bool fit = a->fit_contain != 0;
                const uint32_t gen = a->gen;
                std::string tip(fail_tip ? fail_tip : "");
                free(a);
                Application::GetInstance().Schedule([wid, rgb, rgb_len, iw, ih, stride, fit, gen, tip]() {
                    DisplayLockGuard lock(nl_ov_disp());
                    if (gen != s_home_gen) {
                        if (rgb) {
                            heap_caps_free(rgb);
                        }
                        return;
                    }
                    if (rgb) {
                        photo_apply_rgb(wid.c_str(), rgb, rgb_len, iw, ih, stride, fit, gen);
                    } else if (!tip.empty()) {
                        photo_show_loading(wid.c_str(), tip.c_str());
                    }
                });
            },
            arg, [](void* p) { free(p); }, 0, arg->widget_id)) {
        free(arg);
    }
}

static const nl_widget_t *photo_widget_by_id(const char *widget_id)
{
    if (!s_layout || !widget_id) {
        return nullptr;
    }
    for (int pi = 0; pi < s_layout->page_count; pi++) {
        const nl_page_t *pg = &s_layout->pages[pi];
        if (pg->system_page[0] != '\0') {
            continue;
        }
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->type == NL_WIDGET_PHOTO_FRAME && strcmp(w->id, widget_id) == 0) {
                    return w;
                }
            }
        }
    }
    return nullptr;
}

/** 左右循环切图（到头绕回） */
static void photo_step(photo_rt_t *rt, int delta)
{
    if (!rt || delta == 0) {
        return;
    }
    const nl_widget_t *w = photo_widget_by_id(rt->widget_id);
    if (!w || w->photo_n < 2) {
        return;
    }
    int n = (int)w->photo_n;
    int next = ((int)rt->idx + delta) % n;
    if (next < 0) {
        next += n;
    }
    rt->idx = (uint8_t)next;
    rt->paused = 0;
    /* 热缓存命中不闪「加载中」 */
    if (!photo_blob_has(w->photo_ids[rt->idx])) {
        photo_show_loading(rt->widget_id, "加载中");
    }
    photo_fetch_async(w, rt->idx);
    const int sec = rt->interval_sec >= 3 ? rt->interval_sec : 8;
    rt->next_ms = esp_timer_get_time() / 1000 + (int64_t)sec * 1000;
}

static void photo_advance(photo_rt_t *rt)
{
    photo_step(rt, 1);
}

static void on_photo_timer(lv_timer_t *t)
{
    (void)t;
    if (!s_screen_on || s_active_tab != NL_TAB_HOME || !s_home ||
        lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (nodelio_wifi_is_asleep()) {
        return;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < s_photo_rt_n; i++) {
        photo_rt_t *rt = &s_photo_rt[i];
        if (rt->paused || rt->next_ms <= 0 || now < rt->next_ms) {
            continue;
        }
        photo_advance(rt);
    }
}

void nl_ov_photo_timer_ensure(void)
{
    if (s_photo_timer) {
        return;
    }
    s_photo_timer = lv_timer_create(on_photo_timer, 1000, nullptr);
}

/** 占网格一半及以上视为大图（含 6×6）；多屏时滑动优先翻页 */
static bool photo_frame_is_large(const nl_widget_t *w)
{
    if (!w) {
        return false;
    }
    int cols = nodelio_display_profile()->grid_cols;
    int rows = nodelio_display_profile()->grid_rows;
    if (s_layout) {
        if (s_layout->grid_cols > 0) {
            cols = s_layout->grid_cols;
        }
        if (s_layout->grid_rows > 0) {
            rows = s_layout->grid_rows;
        }
    }
    const int area = (int)w->gw * (int)w->gh;
    const int total = cols * rows;
    return total > 0 && area * 2 >= total;
}

/**
 * 左滑下一张、右滑上一张。
 * 多首页屏时不吞手势，否则大图会挡掉分屏翻页。
 */
static void on_photo_frame_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }
    auto *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || w->type != NL_WIDGET_PHOTO_FRAME || w->photo_n < 2) {
        return;
    }
    if (nl_ov_home_user_page_count() > 1) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) {
        return;
    }
    photo_rt_t *rt = photo_rt_find(w->id);
    if (!rt) {
        return;
    }
    lv_indev_wait_release(indev);
    photo_step(rt, dir == LV_DIR_LEFT ? 1 : -1);
}

/** 小图多屏：点左/右约 1/3 切图（大图不用，易误触翻页） */
static void on_photo_frame_side_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || w->type != NL_WIDGET_PHOTO_FRAME || w->photo_n < 2) {
        return;
    }
    lv_obj_t *tile = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_indev_t *indev = lv_indev_active();
    if (!tile || !indev) {
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_area_t area;
    lv_obj_get_coords(tile, &area);
    const int32_t tw = lv_area_get_width(&area);
    if (tw < 8) {
        return;
    }
    const int32_t x = pt.x - area.x1;
    int delta = 0;
    if (x < tw / 3) {
        delta = -1;
    } else if (x > (tw * 2) / 3) {
        delta = 1;
    } else {
        return;
    }
    photo_rt_t *rt = photo_rt_find(w->id);
    if (!rt) {
        return;
    }
    photo_step(rt, delta);
}

static void on_photo_nav_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    if (!w || !btn) {
        return;
    }
    const intptr_t delta = (intptr_t)lv_obj_get_user_data(btn);
    photo_rt_t *rt = photo_rt_find(w->id);
    if (!rt || (delta != 1 && delta != -1)) {
        return;
    }
    photo_step(rt, (int)delta);
}

static void photo_add_nav_btn(lv_obj_t *tile, nl_widget_t *w, int delta, bool left)
{
    (void)left;
    lv_obj_t *btn = lv_obj_create(tile);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 48, 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, (lv_obj_flag_t)(LV_OBJ_FLAG_USER_1 | LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT));
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(btn, 10);
    lv_obj_set_user_data(btn, (void *)(intptr_t)delta);
    lv_obj_add_event_cb(btn, on_photo_nav_btn, LV_EVENT_CLICKED, w);
    lv_obj_t *lb = lv_label_create(btn);
    nl_ov_set_font(lb);
    lv_label_set_text(lb, delta < 0 ? "<" : ">");
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    lv_obj_set_style_text_opa(lb, LV_OPA_70, 0);
    lv_obj_center(lb);
    lv_obj_clear_flag(lb, LV_OBJ_FLAG_CLICKABLE);
}

static void photo_ensure_nav_btns(lv_obj_t *tile, nl_widget_t *w)
{
    if (!tile || !w || w->photo_n < 2) {
        return;
    }
    bool have = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(tile); i++) {
        lv_obj_t *ch = lv_obj_get_child(tile, i);
        if (ch && lv_obj_has_flag(ch, LV_OBJ_FLAG_USER_1)) {
            have = true;
            break;
        }
    }
    if (!have) {
        photo_add_nav_btn(tile, w, -1, true);
        photo_add_nav_btn(tile, w, 1, false);
    }
    photo_nav_raise(tile);
}

static void photo_rt_register(const nl_widget_t *w)
{
    if (!w || w->photo_n < 2 || s_photo_rt_n >= NL_PHOTO_RT_MAX) {
        return;
    }
    photo_rt_t *rt = &s_photo_rt[s_photo_rt_n++];
    memset(rt, 0, sizeof(*rt));
    snprintf(rt->widget_id, sizeof(rt->widget_id), "%s", w->id);
    rt->idx = 0;
    rt->paused = 0;
    rt->interval_sec = w->photo_interval_sec >= 3 ? w->photo_interval_sec : 8;
    rt->next_ms = esp_timer_get_time() / 1000 + (int64_t)rt->interval_sec * 1000;
    nl_ov_photo_timer_ensure();
}

void nl_ov_fill_photo_frame(nl_widget_t *w, int cell_w, int cell_h)
{
    if (!w) {
        return;
    }
    lv_obj_t *tile = nl_ov_make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
    lv_obj_set_style_bg_color(tile, UI_TILE_OFF, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    /* 图铺满圆角卡：必须 clip，否则四角直角图溢出成黑块 */
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_layout(tile, LV_LAYOUT_NONE);

    lv_obj_t *lb = lv_label_create(tile);
    nl_ov_set_font(lb);
    if (w->photo_n == 0 || !w->photo_ids[0][0]) {
        lv_label_set_text(lb, "去 APP 添加");
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
        lv_obj_center(lb);
        return;
    }
    lv_label_set_text(lb, "加载中");
    lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    lv_obj_center(lb);
    if (w->photo_n >= 2) {
        const bool multi = nl_ov_home_user_page_count() > 1;
        const bool large = photo_frame_is_large(w);
        if (multi && large) {
            /* 大图多屏：整卡不抢点击，滑动直达翻页；左右钮循环切图 */
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            photo_ensure_nav_btns(tile, w);
        } else if (multi) {
            lv_obj_add_event_cb(tile, on_photo_frame_gesture, LV_EVENT_GESTURE, w);
            lv_obj_add_event_cb(tile, on_photo_frame_side_click, LV_EVENT_CLICKED, w);
        } else {
            lv_obj_add_event_cb(tile, on_photo_frame_gesture, LV_EVENT_GESTURE, w);
            lv_obj_add_event_cb(tile, on_photo_frame_side_click, LV_EVENT_CLICKED, w);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
        }
        photo_rt_register(w);
    }
    photo_fetch_async(w, 0);
}
