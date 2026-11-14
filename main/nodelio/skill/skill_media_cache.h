#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 有 SD 且已挂载时可缓存封面/歌词。禁止在 LVGL 回调里调用（会碰 FAT）。 */
bool nodelio_media_cache_available(void);

/** 根据 title+artist 生成缓存目录；成功写出绝对路径。 */
bool nodelio_media_cache_dir(const char *title, const char *artist, char *out, size_t out_len);

bool nodelio_media_cache_load_cover(const char *title, const char *artist, uint8_t *jpeg, size_t cap,
                                    size_t *out_n);
bool nodelio_media_cache_save_cover(const char *title, const char *artist, const uint8_t *jpeg,
                                    size_t jpeg_n);

/** lyrics.json：{"synced":bool,"lines":[{"ms":n,"text":"..."}]} */
bool nodelio_media_cache_load_lyrics(const char *title, const char *artist, char *json, size_t cap);
bool nodelio_media_cache_save_lyrics(const char *title, const char *artist, const char *json);

#ifdef __cplusplus
}
#endif
