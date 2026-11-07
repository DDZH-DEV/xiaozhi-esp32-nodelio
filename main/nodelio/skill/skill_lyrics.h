#pragma once

#include "nvs_config.h"

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 按曲名/艺人向本机服务端要歌词。直播、播客不要调。同曲已在查或已有则跳过。 */
void nodelio_lyrics_request(const nodelio_nvs_config_t *cfg, const char *title, const char *artist);

void nodelio_lyrics_clear(void);

/**
 * 播放中把当前句写到副标题。没有歌词返回 false，调用方继续用原来的「正在播放」。
 * 须在已持有显示锁时调用。
 */
bool nodelio_lyrics_apply(lv_obj_t *label);

#ifdef __cplusplus
}
#endif
