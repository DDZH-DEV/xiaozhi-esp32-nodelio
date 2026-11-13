/*
 * ui_backdrop.cc — 段3 壁纸解码/合成与预烘焙毛玻璃管线（仅 P4 62 / 508 启用）。
 *
 * 流程：
 *   1. 优先读取 /spiffs/wallpaper.jpg（若存在则经 jpeg_to_image_fit 解码并缩放至全屏）；
 *      若不存在则生成「森系林间光影」自然散景壁纸（右上阳光翠绿光斑 + 左下深林暗绿 + 防色带噪点）。
 *   2. 依 nodelio_display_profile() 的 hero_rect / list_rect 执行双趟分离式滑动窗均值模糊
 *      （O(1)/pixel，不随模糊半径增长）+ 圆角遮罩内压暗（保留 28% 亮度 ≈ 72% 黑毛玻璃）。
 *   3. 转换为面板原生像素格式（62: RGB888 BGR 序；508: RGB565），常驻 PSRAM 单缓冲，
 *      挂到 lv_screen_active() 与 s_root 的 bg_image_src。
 */
#include "ui_backdrop.h"

#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_SILA_P4C5_62 || CONFIG_BOARD_TYPE_SILA_P4C5_508

#include "board_info.h"
#include "jpg/jpeg_to_image.h"
#include "ui_theme.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char *TAG = "nl_backdrop";
constexpr const char *kSpiffsWallpaperPath = "/spiffs/wallpaper.jpg";
constexpr int kCardRadius = 20;
constexpr int kBlurRadius = 10;
constexpr int kBlurPasses = 2;
constexpr int kDarkenPct = 28; /* 保留 28% 亮度 ≈ 叠 72% 黑毛玻璃 */

EXT_RAM_BSS_ATTR lv_image_dsc_t s_dsc;
uint8_t *s_buf = nullptr;
int s_buf_w = 0;
int s_buf_h = 0;
lv_color_format_t s_buf_cf = LV_COLOR_FORMAT_UNKNOWN;

struct BokehSpot {
    int16_t cx_permil; /* 0..1000 相对屏宽 */
    int16_t cy_permil; /* 0..1000 相对屏高 */
    int16_t r_permil;  /* 半径占短边千分比 */
    uint8_t dr, dg, db;
};

constexpr BokehSpot kSpots[] = {
    {820, 140, 260, 75, 110, 25},
    {680, 220, 190, 55, 95, 20},
    {910, 320, 210, 60, 100, 18},
    {520, 130, 160, 38, 78, 16},
    {260, 200, 180, 22, 56, 18},
    {150, 680, 240, 12, 36, 15},
    {760, 740, 220, 28, 64, 18},
    {420, 520, 170, 24, 52, 14},
};

/* 生成森系林间散景壁纸（纯整数运算，带微噪点消除 RGB565 色带） */
void synth_forest_canopy(uint8_t *rgb, int w, int h)
{
    const int min_dim = std::min(w, h);
    struct SpotPx {
        int cx, cy, r2;
        int dr, dg, db;
    } spots[sizeof(kSpots) / sizeof(kSpots[0])];
    const int spot_n = (int)(sizeof(kSpots) / sizeof(kSpots[0]));
    for (int i = 0; i < spot_n; ++i) {
        const int r = std::max(16, min_dim * kSpots[i].r_permil / 1000);
        spots[i] = {w * kSpots[i].cx_permil / 1000,
                    h * kSpots[i].cy_permil / 1000,
                    r * r,
                    kSpots[i].dr,
                    kSpots[i].dg,
                    kSpots[i].db};
    }

    for (int y = 0; y < h; ++y) {
        uint8_t *px = rgb + (size_t)y * w * 3;
        const int yn = (y * 256) / h; /* 0..255 */
        for (int x = 0; x < w; ++x, px += 3) {
            const int xn = (x * 256) / w; /* 0..255 */
            /* 基础林间渐变：左下深邃暗绿 -> 右上阳光嫩绿 */
            const int sun = (xn * (255 - yn * 3 / 4)) >> 8;
            int r = 14 + (sun * 58 >> 8) + ((255 - yn) * 18 >> 8);
            int g = 38 + (sun * 118 >> 8) + ((255 - yn) * 42 >> 8);
            int b = 22 + (sun * 28 >> 8) + (xn * 18 >> 8);

            /* 叠加散景光斑（二次衰减，无开方） */
            for (int i = 0; i < spot_n; ++i) {
                const int dx = x - spots[i].cx;
                const int dy = y - spots[i].cy;
                const int d2 = dx * dx + dy * dy;
                if (d2 < spots[i].r2) {
                    const int wgt = ((spots[i].r2 - d2) * 256) / spots[i].r2;
                    const int w2 = (wgt * wgt) >> 8;
                    r += (spots[i].dr * w2) >> 8;
                    g += (spots[i].dg * w2) >> 8;
                    b += (spots[i].db * w2) >> 8;
                }
            }

            /* 叶片层次微纹理 + 蓝噪声抖动 */
            const int canopy = (((x * 5 + y * 3) ^ (x * 13 - y * 7)) >> 4) & 7;
            const int dither = ((x * 17 + y * 31) ^ (x * y)) & 7;
            r = std::clamp(r + canopy - 3 + (dither >> 1), 0, 255);
            g = std::clamp(g + canopy * 2 - 4 + dither, 0, 255);
            b = std::clamp(b + (canopy >> 1) - 2 + (dither >> 1), 0, 255);
            px[0] = (uint8_t)r;
            px[1] = (uint8_t)g;
            px[2] = (uint8_t)b;
        }
    }
}

/* 将解码出的 RGB565 图像双线性/最近邻缩放到 w x h 的 RGB888 缓冲 */
void scale_rgb565_to_rgb888(const uint8_t *src565, int sw, int sh, int sstride, uint8_t *dst888,
                            int dw, int dh)
{
    for (int y = 0; y < dh; ++y) {
        const int sy = (y * sh) / dh;
        const auto *srow = reinterpret_cast<const uint16_t *>(src565 + (size_t)sy * sstride);
        uint8_t *drow = dst888 + (size_t)y * dw * 3;
        for (int x = 0; x < dw; ++x) {
            const uint16_t p = srow[(x * sw) / dw];
            drow[x * 3 + 0] = (uint8_t)(((p >> 11) & 0x1f) * 255 / 31);
            drow[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) * 255 / 63);
            drow[x * 3 + 2] = (uint8_t)((p & 0x1f) * 255 / 31);
        }
    }
}

bool decode_jpeg_to_rgb888(const uint8_t *jpeg, size_t len, uint8_t *dst888, int dw, int dh)
{
    if (!jpeg || len == 0) {
        return false;
    }
    uint8_t *rgb565 = nullptr;
    size_t out_len = 0, w = 0, h = 0, stride = 0;
    const size_t max_bytes = (size_t)dw * dh * 2 * 2;
    if (jpeg_to_image_fit(jpeg, len, dw, dh, max_bytes, &rgb565, &out_len, &w, &h, &stride) !=
            ESP_OK ||
        !rgb565 || w < 1 || h < 1) {
        if (rgb565) {
            heap_caps_free(rgb565);
        }
        return false;
    }
    scale_rgb565_to_rgb888(rgb565, (int)w, (int)h, (int)(stride ? stride : w * 2), dst888, dw, dh);
    heap_caps_free(rgb565);
    return true;
}

bool try_load_spiffs_wallpaper(uint8_t *dst888, int dw, int dh)
{
    FILE *f = fopen(kSpiffsWallpaperPath, "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) {
        fclose(f);
        return false;
    }
    auto *jb = static_cast<uint8_t *>(heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM));
    if (!jb) {
        fclose(f);
        return false;
    }
    const size_t rd = fread(jb, 1, (size_t)sz, f);
    fclose(f);
    const bool ok = (rd == (size_t)sz) && decode_jpeg_to_rgb888(jb, rd, dst888, dw, dh);
    heap_caps_free(jb);
    if (ok) {
        ESP_LOGI(TAG, "loaded custom wallpaper %s (%ld bytes)", kSpiffsWallpaperPath, sz);
    }
    return ok;
}

void blur_h(uint8_t *rgb, int w, const nodelio_rect_t &r, uint8_t *tmp)
{
    const int win = kBlurRadius * 2 + 1;
    for (int y = r.y; y < r.y + r.h; ++y) {
        uint8_t *row = rgb + (size_t)y * w * 3;
        int s[3] = {0, 0, 0};
        for (int k = -kBlurRadius; k <= kBlurRadius; ++k) {
            const int sx = std::clamp((int)r.x + k, 0, w - 1);
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

void blur_v(uint8_t *rgb, int w, int h, const nodelio_rect_t &r, uint8_t *tmp)
{
    const int win = kBlurRadius * 2 + 1;
    for (int x = r.x; x < r.x + r.w; ++x) {
        int s[3] = {0, 0, 0};
        for (int k = -kBlurRadius; k <= kBlurRadius; ++k) {
            const int sy = std::clamp((int)r.y + k, 0, h - 1);
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

bool in_round_rect(int x, int y, const nodelio_rect_t &r, int rad)
{
    const int lx = x - r.x, ly = y - r.y;
    const int dx = lx < rad ? rad - lx : (lx >= r.w - rad ? lx - (r.w - rad - 1) : 0);
    const int dy = ly < rad ? rad - ly : (ly >= r.h - rad ? ly - (r.h - rad - 1) : 0);
    return dx * dx + dy * dy <= rad * rad;
}

void bake_cards(uint8_t *baked, const uint8_t *src, int w, int h, const nodelio_rect_t *rects,
                int n, uint8_t *tmp)
{
    for (int i = 0; i < n; ++i) {
        if (rects[i].w == 0 || rects[i].h == 0) continue;
        for (int p = 0; p < kBlurPasses; ++p) {
            blur_h(baked, w, rects[i], tmp);
            blur_v(baked, w, h, rects[i], tmp);
        }
    }
    for (int i = 0; i < n; ++i) {
        const nodelio_rect_t &r = rects[i];
        if (r.w == 0 || r.h == 0) continue;
        for (int y = r.y; y < r.y + r.h && y < h; ++y) {
            for (int x = r.x; x < r.x + r.w && x < w; ++x) {
                const size_t o = ((size_t)y * w + x) * 3;
                if (in_round_rect(x, y, r, kCardRadius)) {
                    for (int c = 0; c < 3; ++c) {
                        baked[o + c] = (uint8_t)(baked[o + c] * kDarkenPct / 100);
                    }
                } else {
                    memcpy(baked + o, src + o, 3);
                }
            }
        }
    }
}

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

bool build_backdrop(const uint8_t *custom_jpeg, size_t custom_len)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return false;
    }
    const int w = lv_display_get_horizontal_resolution(disp);
    const int h = lv_display_get_vertical_resolution(disp);
    const lv_color_format_t cf = lv_display_get_color_format(disp);
    const int bpp = cf == LV_COLOR_FORMAT_RGB888 ? 3 : (cf == LV_COLOR_FORMAT_RGB565 ? 2 : 0);
    if (!bpp || w <= 0 || h <= 0) {
        return false;
    }
    const size_t out_sz = (size_t)w * h * bpp;
    if (!s_buf || s_buf_w != w || s_buf_h != h || s_buf_cf != cf) {
        if (s_buf) {
            lv_image_cache_drop(&s_dsc);
            heap_caps_free(s_buf);
            s_buf = nullptr;
        }
        s_buf = static_cast<uint8_t *>(heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM));
        if (!s_buf) {
            ESP_LOGE(TAG, "backdrop alloc fail %u bytes", (unsigned)out_sz);
            return false;
        }
        s_buf_w = w;
        s_buf_h = h;
        s_buf_cf = cf;
    }

    const size_t work_sz = (size_t)w * h * 3;
    auto *src = static_cast<uint8_t *>(heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM));
    auto *baked = static_cast<uint8_t *>(heap_caps_malloc(work_sz, MALLOC_CAP_SPIRAM));
    auto *tmp = static_cast<uint8_t *>(heap_caps_malloc((size_t)std::max(w, h) * 3, MALLOC_CAP_SPIRAM));
    if (!src || !baked || !tmp) {
        ESP_LOGE(TAG, "backdrop work alloc fail");
        heap_caps_free(src);
        heap_caps_free(baked);
        heap_caps_free(tmp);
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    bool loaded = false;
    if (custom_jpeg && custom_len > 0) {
        loaded = decode_jpeg_to_rgb888(custom_jpeg, custom_len, src, w, h);
    }
    if (!loaded) {
        loaded = try_load_spiffs_wallpaper(src, w, h);
    }
    if (!loaded) {
        synth_forest_canopy(src, w, h);
    }
    const int64_t t1 = esp_timer_get_time();

    memcpy(baked, src, work_sz);
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    if (prof) {
        const nodelio_rect_t rects[2] = {prof->hero_rect, prof->list_rect};
        bake_cards(baked, src, w, h, rects, 2, tmp);
    }
    to_display(baked, w, h, cf, s_buf);
    const int64_t t2 = esp_timer_get_time();

    heap_caps_free(src);
    heap_caps_free(baked);
    heap_caps_free(tmp);

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = cf;
    s_dsc.header.w = w;
    s_dsc.header.h = h;
    s_dsc.header.stride = w * bpp;
    s_dsc.data_size = (uint32_t)out_sz;
    s_dsc.data = s_buf;
    lv_image_cache_drop(&s_dsc);

    ESP_LOGI(TAG,
             "baked %dx%d bpp=%d src_ms=%lld bake_ms=%lld internal_free=%u dma_largest=%u psram_free=%u",
             w, h, bpp, (t1 - t0) / 1000, (t2 - t1) / 1000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void bind_backdrop(lv_obj_t *root, const lv_image_dsc_t *dsc)
{
    if (lv_obj_t *scr = lv_screen_active()) {
        lv_obj_set_style_bg_image_src(scr, dsc, 0);
    }
    if (root) {
        lv_obj_set_style_bg_image_src(root, dsc, 0);
    }
}

}  // namespace

extern "C" void nl_backdrop_sync(lv_obj_t *root)
{
    if (!ui_palette_is_immersive()) {
        bind_backdrop(root, nullptr);
        return;
    }
    if (!s_buf && !build_backdrop(nullptr, 0)) {
        bind_backdrop(root, nullptr);
        return;
    }
    bind_backdrop(root, &s_dsc);
}

extern "C" bool nl_backdrop_set_jpeg(lv_obj_t *root, const uint8_t *jpeg, size_t len)
{
    if (!build_backdrop(jpeg, len)) {
        return false;
    }
    if (ui_palette_is_immersive()) {
        bind_backdrop(root, &s_dsc);
    }
    return true;
}

extern "C" const lv_image_dsc_t *nl_backdrop_dsc(void)
{
    return (ui_palette_is_immersive() && s_buf) ? &s_dsc : nullptr;
}

#else

extern "C" void nl_backdrop_sync(lv_obj_t *root)
{
    (void)root;
}

extern "C" bool nl_backdrop_set_jpeg(lv_obj_t *root, const uint8_t *jpeg, size_t len)
{
    (void)root;
    (void)jpeg;
    (void)len;
    return false;
}

extern "C" const lv_image_dsc_t *nl_backdrop_dsc(void)
{
    return nullptr;
}

#endif

