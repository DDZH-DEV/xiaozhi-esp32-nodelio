#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 全屏/区域图片预览（缩放 + 拖动画布）。客服气泡图、扫码、相册等共用。 */
typedef struct {
    int x;                 /**< 相对 layer_top；默认 0 */
    int y;
    int w;                 /**< 0 = 全屏宽 */
    int h;                 /**< 0 = 全屏高 */
    bool allow_zoom;       /**< 左侧放大/缩小 + 拖动画布；否则仅关闭 */
    bool close_on_blank;   /**< 点空白关闭（扫码左栏） */
    const char *loading_text;
} nl_ui_image_preview_opts_t;

/** 打开加载壳；已打开则先关。返回 generation，异步任务比对用。 */
uint32_t nl_ui_image_preview_open(const nl_ui_image_preview_opts_t *opts);

uint32_t nl_ui_image_preview_gen(void);
bool nl_ui_image_preview_visible(void);

/** 视口尺寸（解码 max_w/max_h）。未打开返回 false。 */
bool nl_ui_image_preview_view_size(int *w, int *h);

/**
 * 设置 RGB565。成功则接管 rgb（heap_caps_free）。
 * gen 不匹配或已关闭时释放 rgb 并返回 false。
 * 调用方须已持有显示锁（与现有 Job 回写一致）。
 */
bool nl_ui_image_preview_set_rgb565(uint32_t gen, uint8_t *rgb, uint32_t w, uint32_t h,
                                    uint32_t stride, size_t data_size);

void nl_ui_image_preview_fail(uint32_t gen, const char *msg);
void nl_ui_image_preview_hide(void);

/** 预览置顶后再抬 chrome（截图浮标等）。 */
void nl_ui_image_preview_raise(void);

#ifdef __cplusplus
}
#endif
