#pragma once

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 按曲名/艺人拉封面：iTunes Search → Cover Art Archive（MusicBrainz）双兜底。
 * 解码为 RGB565 后交给回调（须在显示锁内用）。同曲不重复拉。
 */
typedef void (*nodelio_cover_done_cb_t)(const lv_image_dsc_t *dsc, void *user);

void nodelio_cover_request(const char *title, const char *artist, nodelio_cover_done_cb_t cb,
                           void *user);

void nodelio_cover_clear(void);

#ifdef __cplusplus
}
#endif
