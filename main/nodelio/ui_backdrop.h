#pragma once

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 同步沉浸模式背景底图（段3）：
 * - 当 ui_palette_is_immersive() 为真时：首次生成/解码壁纸并按 display_profile 的
 *   hero_rect / list_rect 预烘焙毛玻璃（box-blur + 压暗 + 圆角遮罩），挂载到
 *   lv_screen_active() 与 root 的 bg_image_src。
 * - 当切换回浅色/深色时：卸载 bg_image_src（保留缓存避免反复重烘）。
 * 须在 LVGL 锁内调用。
 */
void nl_backdrop_sync(lv_obj_t *root);

/** 注入自定义 JPEG 壁纸并立即重烘（须在 LVGL 锁内调用）。 */
bool nl_backdrop_set_jpeg(lv_obj_t *root, const uint8_t *jpeg, size_t len);

/** 当前已烘焙的底图描述符（未启用沉浸模式或未就绪时返回 NULL）。 */
const lv_image_dsc_t *nl_backdrop_dsc(void);

#ifdef __cplusplus
}
#endif

