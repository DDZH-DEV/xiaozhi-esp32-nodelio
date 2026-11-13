/*
 * ui_probe.cc — 段1 渲染实测页（仅 CONFIG_NODELIO_UI_PROBE=y 编入实现）。
 * 三模式各跑 10s 自动轮换：
 *   0 classic : 现状（实心底 + 实心卡片），作基线
 *   1 alpha   : 壁纸 + 运行时 70% 黑半透明圆角卡（CPU 混合）
 *   2 baked   : 壁纸卡片区预先 blur+压暗，卡片透明只画 1px 边
 * 列表自动滚动制造连续重绘；按模式统计 LVGL 刷新耗时并打日志。点顶部统计条退出。
 * 版式矩形为实测临时值，段2 迁入 display_profile。
 */
#include "ui_probe.h"

#include "sdkconfig.h"

#if CONFIG_NODELIO_UI_PROBE

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "board_info.h"
#include "panel_ui.h"
#include "ui_theme.h"

void nl_ov_set_font(lv_obj_t *obj);

namespace {

constexpr const char *TAG = "nl_probe";
constexpr int kModeClassic = 0;
constexpr int kModeAlpha = 1;
constexpr int kModeBaked = 2;
constexpr int kModeCount = 3;
constexpr const char *kModeName[kModeCount] = {"classic", "alpha", "baked"};
constexpr uint32_t kModeMs = 10000;
constexpr int64_t kWarmupUs = 1000000; /* 切模式后的整屏重绘不计入 */
constexpr int kCardRadius = 20;
constexpr int kBlurRadius = 10;
constexpr int kBlurPasses = 2;
constexpr int kDarkenPct = 30; /* 保留 30% 亮度 ≈ 叠 70% 黑 */
constexpr int kRowCount = 30;
constexpr int kRowH = 52;
constexpr int kScrollStep = 6;
constexpr uint32_t kScrollMs = 16;
constexpr int kMaxHidden = 32;

struct Rect {
    int x, y, w, h;
};
struct Geometry {
    Rect dock, hero, list;
};

Geometry geometry_for(int w, int h)
{
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    if (prof && prof->dock_rect.w > 0 && prof->hero_rect.w > 0 && prof->list_rect.w > 0) {
        return {{prof->dock_rect.x, prof->dock_rect.y, prof->dock_rect.w, prof->dock_rect.h},
                {prof->hero_rect.x, prof->hero_rect.y, prof->hero_rect.w, prof->hero_rect.h},
                {prof->list_rect.x, prof->list_rect.y, prof->list_rect.w, prof->list_rect.h}};
    }
    if (w > h) { /* 62: 1280x452 */
        return {{16, 16, 76, 420}, {108, 16, 468, 420}, {592, 16, 672, 420}};
    }
    /* 508: 720x720 */
    return {{12, 16, 68, 688}, {92, 16, 612, 310}, {92, 338, 612, 366}};
}

struct Result {
    bool valid;
    uint32_t frames;
    int64_t avg_us, max_us;
    int fps10; /* fps×10，避免依赖浮点 printf */
};

struct Probe {
    lv_display_t *disp;
    int w, h;
    lv_color_format_t cf;
    Geometry g;
    lv_obj_t *root, *wall, *dock, *hero, *list_card, *list, *stat;
    lv_timer_t *scroll_tmr, *mode_tmr;
    lv_image_dsc_t img[2]; /* 0 原壁纸 1 预烘焙 */
    uint8_t *buf[2];
    bool have_wall;
    lv_obj_t *hidden[kMaxHidden];
    int hidden_n;
    int mode, dir, scroll_ticks;
    int64_t mode_started_us, refr_start_us;
    bool drew;
    uint32_t frames;
    int64_t total_us, max_us;
    int64_t gen_us, bake_us;
    Result res[kModeCount];
};

Probe *s_p;

/* ---------- 壁纸生成与烘焙（工作缓冲为 R,G,B 字节序） ---------- */

void gen_wallpaper(uint8_t *rgb, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        uint8_t *px = rgb + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x, px += 3) {
            const int cell = ((x / 40) + (y / 40)) & 1;
            const int grain = ((x * 7 + y * 13) ^ (x * y)) & 15;
            px[0] = (uint8_t)std::min(255, 20 + x * 60 / w + cell * 30 + grain);
            px[1] = (uint8_t)std::min(255, 70 + y * 110 / h + cell * 40 + grain);
            px[2] = (uint8_t)std::min(255, 40 + (w - x) * 50 / w + cell * 20);
        }
    }
}

void blur_h(uint8_t *rgb, int w, const Rect &r, uint8_t *tmp)
{
    const int win = kBlurRadius * 2 + 1;
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint8_t *row = rgb + (size_t)y * w * 3;
        int s[3] = {0, 0, 0};
        for (int k = -kBlurRadius; k <= kBlurRadius; ++k) {
            const int sx = std::clamp(r.x + k, 0, w - 1);
            for (int c = 0; c < 3; ++c) s[c] += row[sx * 3 + c];
        }
        for (int x = r.x; x < r.x + r.w; ++x) {
            for (int c = 0; c < 3; ++c) tmp[(x - r.x) * 3 + c] = (uint8_t)(s[c] / win);
            const int out = std::clamp(x - kBlurRadius, 0, w - 1);
            const int in = std::clamp(x + kBlurRadius + 1, 0, w - 1);
            for (int c = 0; c < 3; ++c) s[c] += row[in * 3 + c] - row[out * 3 + c];
        }
        memcpy(row + r.x * 3, tmp, (size_t)r.w * 3);
    }
}

void blur_v(uint8_t *rgb, int w, int h, const Rect &r, uint8_t *tmp)
{
    const int win = kBlurRadius * 2 + 1;
    for (int x = r.x; x < r.x + r.w; ++x) {
        int s[3] = {0, 0, 0};
        for (int k = -kBlurRadius; k <= kBlurRadius; ++k) {
            const int sy = std::clamp(r.y + k, 0, h - 1);
            for (int c = 0; c < 3; ++c) s[c] += rgb[((size_t)sy * w + x) * 3 + c];
        }
        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int c = 0; c < 3; ++c) tmp[(y - r.y) * 3 + c] = (uint8_t)(s[c] / win);
            const int out = std::clamp(y - kBlurRadius, 0, h - 1);
            const int in = std::clamp(y + kBlurRadius + 1, 0, h - 1);
            for (int c = 0; c < 3; ++c) {
                s[c] += rgb[((size_t)in * w + x) * 3 + c] - rgb[((size_t)out * w + x) * 3 + c];
            }
        }
        for (int y = r.y; y < r.y + r.h; ++y) {
            memcpy(rgb + ((size_t)y * w + x) * 3, tmp + (y - r.y) * 3, 3);
        }
    }
}

bool in_round_rect(int x, int y, const Rect &r, int rad)
{
    const int lx = x - r.x, ly = y - r.y;
    const int dx = lx < rad ? rad - lx : (lx >= r.w - rad ? lx - (r.w - rad - 1) : 0);
    const int dy = ly < rad ? rad - ly : (ly >= r.h - rad ? ly - (r.h - rad - 1) : 0);
    return dx * dx + dy * dy <= rad * rad;
}

/* baked 进来时是 src 拷贝；卡片区 blur 后在圆角内压暗，圆角外还原 src。 */
void bake_glass(uint8_t *baked, const uint8_t *src, int w, int h, const Rect *rects, int n,
                uint8_t *tmp)
{
    for (int i = 0; i < n; ++i) {
        for (int p = 0; p < kBlurPasses; ++p) {
            blur_h(baked, w, rects[i], tmp);
            blur_v(baked, w, h, rects[i], tmp);
        }
    }
    for (int i = 0; i < n; ++i) {
        const Rect &r = rects[i];
        for (int y = r.y; y < r.y + r.h; ++y) {
            for (int x = r.x; x < r.x + r.w; ++x) {
                const size_t o = ((size_t)y * w + x) * 3;
                if (in_round_rect(x, y, r, kCardRadius)) {
                    for (int c = 0; c < 3; ++c) baked[o + c] = (uint8_t)(baked[o + c] * kDarkenPct / 100);
                } else {
                    memcpy(baked + o, src + o, 3);
                }
            }
        }
    }
}

/* LVGL RGB888 内存序为 B,G,R；RGB565 为本机序 uint16。 */
void to_display(const uint8_t *rgb, int w, int h, lv_color_format_t cf, uint8_t *out)
{
    const size_t n = (size_t)w * h;
    if (cf == LV_COLOR_FORMAT_RGB888) {
        for (size_t i = 0; i < n; ++i) {
            out[i * 3 + 0] = rgb[i * 3 + 2];
            out[i * 3 + 1] = rgb[i * 3 + 1];
            out[i * 3 + 2] = rgb[i * 3 + 0];
        }
    } else {
        auto *o16 = reinterpret_cast<uint16_t *>(out);
        for (size_t i = 0; i < n; ++i) {
            o16[i] = (uint16_t)(((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) |
                                (rgb[i * 3 + 2] >> 3));
        }
    }
}

void init_dsc(lv_image_dsc_t *d, uint8_t *data, int w, int h, lv_color_format_t cf, int bpp)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * bpp;
    d->data_size = (uint32_t)(w * bpp * h);
    d->data = data;
}

bool build_wallpapers(Probe *p)
{
    const int bpp = p->cf == LV_COLOR_FORMAT_RGB888 ? 3 : (p->cf == LV_COLOR_FORMAT_RGB565 ? 2 : 0);
    if (!bpp) {
        ESP_LOGW(TAG, "unsupported color format %d, glass modes skipped", (int)p->cf);
        return false;
    }
    const size_t work_sz = (size_t)p->w * p->h * 3;
    const size_t out_sz = (size_t)p->w * p->h * bpp;
    auto *src = static_cast<uint8_t *>(heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM));
    auto *baked = static_cast<uint8_t *>(heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM));
    auto *tmp = static_cast<uint8_t *>(heap_caps_malloc((size_t)std::max(p->w, p->h) * 3, MALLOC_CAP_SPIRAM));
    p->buf[0] = static_cast<uint8_t *>(heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM));
    p->buf[1] = static_cast<uint8_t *>(heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM));
    bool ok = src && baked && tmp && p->buf[0] && p->buf[1];
    if (ok) {
        int64_t t0 = esp_timer_get_time();
        gen_wallpaper(src, p->w, p->h);
        to_display(src, p->w, p->h, p->cf, p->buf[0]);
        p->gen_us = esp_timer_get_time() - t0;

        t0 = esp_timer_get_time();
        memcpy(baked, src, work_sz);
        const Rect rects[2] = {p->g.hero, p->g.list};
        bake_glass(baked, src, p->w, p->h, rects, 2, tmp);
        to_display(baked, p->w, p->h, p->cf, p->buf[1]);
        p->bake_us = esp_timer_get_time() - t0;

        init_dsc(&p->img[0], p->buf[0], p->w, p->h, p->cf, bpp);
        init_dsc(&p->img[1], p->buf[1], p->w, p->h, p->cf, bpp);
        ESP_LOGI(TAG, "wallpaper %dx%d bpp=%d gen_ms=%lld bake_ms=%lld (blur r=%d x%d, 2 cards)",
                 p->w, p->h, bpp, p->gen_us / 1000, p->bake_us / 1000, kBlurRadius, kBlurPasses);
    } else {
        ESP_LOGE(TAG, "wallpaper alloc fail work=%u out=%u x2", (unsigned)work_sz, (unsigned)out_sz);
        heap_caps_free(p->buf[0]);
        heap_caps_free(p->buf[1]);
        p->buf[0] = p->buf[1] = nullptr;
    }
    heap_caps_free(src);
    heap_caps_free(baked);
    heap_caps_free(tmp);
    return ok;
}

/* ---------- 统计 ---------- */

void log_heap(const char *why)
{
    ESP_LOGI(TAG, "%s: internal_free=%u dma_largest=%u psram_free=%u", why,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void on_refresh(lv_event_t *e)
{
    Probe *p = static_cast<Probe *>(lv_event_get_user_data(e));
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
        case LV_EVENT_REFR_START:
            p->refr_start_us = now;
            p->drew = false;
            break;
        case LV_EVENT_RENDER_START:
            p->drew = true;
            break;
        case LV_EVENT_REFR_READY:
            if (p->drew && now - p->mode_started_us >= kWarmupUs) {
                const int64_t dt = now - p->refr_start_us;
                p->total_us += dt;
                p->max_us = std::max(p->max_us, dt);
                ++p->frames;
            }
            break;
        default:
            break;
    }
}

void update_stat_text(Probe *p)
{
    char buf[200];
    int n = snprintf(buf, sizeof(buf), "[%s] bake %lldms | ", kModeName[p->mode], p->bake_us / 1000);
    for (int m = 0; m < kModeCount && n < (int)sizeof(buf); ++m) {
        const Result &r = p->res[m];
        if (r.valid) {
            n += snprintf(buf + n, sizeof(buf) - n, "%s %d.%dfps %lld/%lldms  ", kModeName[m],
                          r.fps10 / 10, r.fps10 % 10, r.avg_us / 1000, r.max_us / 1000);
        }
    }
    snprintf(buf + std::min(n, (int)sizeof(buf) - 1), sizeof(buf) - std::min(n, (int)sizeof(buf) - 1),
             "| 点此退出");
    lv_label_set_text(p->stat, buf);
}

/* ---------- 界面 ---------- */

lv_obj_t *make_box(lv_obj_t *parent, const Rect &r)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *make_glass_card(lv_obj_t *parent, const Rect &r)
{
    lv_obj_t *o = make_box(parent, r);
    lv_obj_set_style_radius(o, kCardRadius, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_white(), 0);
    lv_obj_set_style_border_opa(o, 36, 0);
    lv_obj_set_style_pad_all(o, 16, 0);
    return o;
}

void build_hero(Probe *p)
{
    const Rect &r = p->g.hero;
    lv_obj_t *title = lv_label_create(p->hero);
    lv_label_set_text(title, "沉浸渲染实测");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    const int inner_w = r.w - 32;
    const int btn_h = 48;
    const int cover_h = r.h - 32 - 28 - btn_h - 24;
    lv_obj_t *cover = make_box(p->hero, {0, 28, inner_w, cover_h});
    lv_obj_set_style_radius(cover, 16, 0);
    lv_obj_set_style_bg_color(cover, UI_ACCENT, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);

    lv_obj_t *play = make_box(cover, {16, cover_h - 68, 52, 52});
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);

    static const char *kTabs[4] = {"本地", "网络", "电台", "插件"};
    const int gap = 10;
    const int bw = (inner_w - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *b = make_box(p->hero, {i * (bw + gap), r.h - 32 - btn_h, bw, btn_h});
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, i == 0 ? 60 : 30, 0); /* 小面积半透明，三模式一致 */
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, kTabs[i]);
        lv_obj_center(l);
    }
}

void build_list(Probe *p)
{
    const Rect &r = p->g.list;
    const int bar_h = 56;
    const int inner_w = r.w - 32;
    const int list_h = r.h - 32 - bar_h - 8;
    p->list = lv_obj_create(p->list_card);
    lv_obj_remove_style_all(p->list);
    lv_obj_set_pos(p->list, 0, 0);
    lv_obj_set_size(p->list, inner_w, list_h);
    lv_obj_set_scrollbar_mode(p->list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
    lv_obj_remove_flag(p->list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(p->list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kRowCount; ++i) {
        lv_obj_t *row = lv_obj_create(p->list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, inner_w, kRowH);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text_fmt(l, "%02d  测试曲目 %02d", i + 1, i + 1);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        if (i == 2) {
            lv_obj_set_style_text_color(l, UI_ACCENT, 0);
        }
        lv_obj_t *s = make_box(row, {inner_w - 40, (kRowH - 28) / 2, 28, 28});
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_set_style_border_color(s, lv_color_white(), 0);
        lv_obj_set_style_border_opa(s, 160, 0);
    }

    lv_obj_t *bar = make_box(p->list_card, {0, r.h - 32 - bar_h, inner_w, bar_h});
    lv_obj_set_style_radius(bar, 14, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar, 24, 0);
    lv_obj_t *track = make_box(bar, {72, bar_h / 2 - 2, inner_w / 3, 4});
    lv_obj_set_style_radius(track, 2, 0);
    lv_obj_set_style_bg_color(track, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(track, 90, 0);
    lv_obj_t *fill = make_box(track, {0, 0, inner_w / 10, 4});
    lv_obj_set_style_radius(fill, 2, 0);
    lv_obj_set_style_bg_color(fill, UI_ACCENT, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
}

void build_dock(Probe *p)
{
    lv_obj_set_style_radius(p->dock, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p->dock, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p->dock, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_ver(p->dock, 14, 0);
    lv_obj_set_flex_flow(p->dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int icon = std::min(48, p->g.dock.w - 20);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *c = lv_obj_create(p->dock);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, icon, icon);
        lv_obj_set_style_radius(c, 14, 0);
        lv_obj_set_style_bg_color(c, UI_ACCENT, 0);
        lv_obj_set_style_bg_opa(c, i == 4 ? LV_OPA_COVER : 70, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    }
}

void apply_mode(Probe *p, int m)
{
    if (m != kModeClassic && !p->have_wall) {
        m = kModeClassic;
    }
    p->mode = m;
    const bool glass = m != kModeClassic;
    if (glass) {
        lv_image_set_src(p->wall, &p->img[m == kModeAlpha ? 0 : 1]);
        lv_obj_remove_flag(p->wall, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(p->root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(p->root, lv_color_white(), 0);
    } else {
        lv_obj_add_flag(p->wall, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(p->root, UI_BG, 0);
        lv_obj_set_style_bg_opa(p->root, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(p->root, UI_TEXT, 0);
    }
    lv_obj_t *cards[2] = {p->hero, p->list_card};
    for (lv_obj_t *c : cards) {
        if (m == kModeClassic) {
            lv_obj_set_style_bg_color(c, UI_SURFACE, 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        } else if (m == kModeAlpha) {
            lv_obj_set_style_bg_color(c, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_70, 0);
        } else {
            lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
        }
    }
    p->frames = 0;
    p->total_us = 0;
    p->max_us = 0;
    p->mode_started_us = esp_timer_get_time();
    update_stat_text(p);
}

void on_mode_timer(lv_timer_t *t)
{
    Probe *p = static_cast<Probe *>(lv_timer_get_user_data(t));
    const int64_t span = esp_timer_get_time() - p->mode_started_us - kWarmupUs;
    Result &r = p->res[p->mode];
    r.valid = p->frames > 0 && span > 0;
    r.frames = p->frames;
    r.avg_us = p->frames ? p->total_us / p->frames : 0;
    r.max_us = p->max_us;
    r.fps10 = span > 0 ? (int)((int64_t)p->frames * 10000000LL / span) : 0;
    ESP_LOGI(TAG, "mode=%s frames=%lu fps=%d.%d refresh_us avg=%lld max=%lld (refr_period_cfg=%dms)",
             kModeName[p->mode], (unsigned long)r.frames, r.fps10 / 10, r.fps10 % 10, r.avg_us,
             r.max_us, CONFIG_NODELIO_UI_ACTIVE_REFRESH_MS);
    log_heap(kModeName[p->mode]);
    apply_mode(p, (p->mode + 1) % kModeCount);
}

void on_scroll_timer(lv_timer_t *t)
{
    Probe *p = static_cast<Probe *>(lv_timer_get_user_data(t));
    if (p->dir > 0) {
        lv_obj_scroll_by(p->list, 0, -kScrollStep, LV_ANIM_OFF);
        if (lv_obj_get_scroll_bottom(p->list) <= 0) p->dir = -1;
    } else {
        lv_obj_scroll_by(p->list, 0, kScrollStep, LV_ANIM_OFF);
        if (lv_obj_get_scroll_top(p->list) <= 0) p->dir = 1;
    }
    if (++p->scroll_ticks >= 60) { /* 约 1s 续一次，防实测中息屏 */
        p->scroll_ticks = 0;
        nodelio_panel_ui_note_activity();
    }
}

void on_stat_clicked(lv_event_t *)
{
    lv_async_call([](void *) { nl_ui_probe_close(); }, nullptr);
}

}  // namespace

extern "C" void nl_ui_probe_open(void)
{
    if (s_p) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return;
    }
    auto *p = static_cast<Probe *>(heap_caps_calloc(1, sizeof(Probe), MALLOC_CAP_SPIRAM));
    if (!p) {
        ESP_LOGE(TAG, "probe alloc fail");
        return;
    }
    s_p = p;
    p->disp = disp;
    p->w = lv_display_get_horizontal_resolution(disp);
    p->h = lv_display_get_vertical_resolution(disp);
    p->cf = lv_display_get_color_format(disp);
    p->g = geometry_for(p->w, p->h);
    p->dir = 1;
    log_heap("probe before");
    p->have_wall = build_wallpapers(p);
    log_heap("probe wallpapers");

    /* 主屏子节点暂藏，避免被盖住的旧界面仍参与重绘 */
    lv_obj_t *scr = lv_screen_active();
    const uint32_t cnt = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < cnt && p->hidden_n < kMaxHidden; ++i) {
        lv_obj_t *c = lv_obj_get_child(scr, (int32_t)i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
            p->hidden[p->hidden_n++] = c;
        }
    }

    p->root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p->root);
    lv_obj_set_pos(p->root, 0, 0);
    lv_obj_set_size(p->root, p->w, p->h);
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE); /* 吃掉触摸，不漏到下层 */
    nl_ov_set_font(p->root);

    p->wall = lv_image_create(p->root);
    lv_obj_set_pos(p->wall, 0, 0);
    lv_obj_add_flag(p->wall, LV_OBJ_FLAG_HIDDEN);

    p->dock = make_box(p->root, p->g.dock);
    build_dock(p);
    p->hero = make_glass_card(p->root, p->g.hero);
    build_hero(p);
    p->list_card = make_glass_card(p->root, p->g.list);
    build_list(p);

    p->stat = lv_label_create(p->root);
    lv_obj_set_width(p->stat, p->w - 160);
    lv_label_set_long_mode(p->stat, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_bg_color(p->stat, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p->stat, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(p->stat, lv_color_white(), 0);
    lv_obj_set_style_pad_hor(p->stat, 8, 0);
    lv_obj_set_style_pad_ver(p->stat, 4, 0);
    lv_obj_align(p->stat, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_add_flag(p->stat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(p->stat, 16);
    lv_obj_add_event_cb(p->stat, on_stat_clicked, LV_EVENT_CLICKED, nullptr);

    lv_display_add_event_cb(disp, on_refresh, LV_EVENT_REFR_START, p);
    lv_display_add_event_cb(disp, on_refresh, LV_EVENT_RENDER_START, p);
    lv_display_add_event_cb(disp, on_refresh, LV_EVENT_REFR_READY, p);

    apply_mode(p, kModeClassic);
    p->scroll_tmr = lv_timer_create(on_scroll_timer, kScrollMs, p);
    p->mode_tmr = lv_timer_create(on_mode_timer, kModeMs, p);
    ESP_LOGI(TAG, "probe open %dx%d cf=%d, %lus per mode", p->w, p->h, (int)p->cf,
             (unsigned long)(kModeMs / 1000));
}

extern "C" void nl_ui_probe_close(void)
{
    Probe *p = s_p;
    if (!p) {
        return;
    }
    s_p = nullptr;
    lv_display_remove_event_cb_with_user_data(p->disp, on_refresh, p);
    if (p->scroll_tmr) lv_timer_delete(p->scroll_tmr);
    if (p->mode_tmr) lv_timer_delete(p->mode_tmr);
    lv_obj_delete(p->root);
    for (int i = 0; i < 2; ++i) {
        if (p->buf[i]) {
            lv_image_cache_drop(&p->img[i]);
            heap_caps_free(p->buf[i]);
        }
    }
    for (int i = 0; i < p->hidden_n; ++i) {
        lv_obj_remove_flag(p->hidden[i], LV_OBJ_FLAG_HIDDEN);
    }
    heap_caps_free(p);
    log_heap("probe closed");
}

#else

extern "C" void nl_ui_probe_open(void) {}
extern "C" void nl_ui_probe_close(void) {}

#endif
