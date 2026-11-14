#pragma once

#include "nvs_config.h"

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 按曲名/艺人要歌词：SD 缓存 → 本机 SERVER → 公开兜底。
 * cfg 可空或无 server（仅走公开接口）。直播、播客不要调。同曲已在查或已有则跳过（force 除外）。
 */
void nodelio_lyrics_request(const nodelio_nvs_config_t *cfg, const char *title, const char *artist,
                            bool force);

void nodelio_lyrics_clear(void);

/**
 * 播放中把当前句写到副标题。没有歌词返回 false，调用方继续用原来的「正在播放」。
 * 须在已持有显示锁时调用。
 */
bool nodelio_lyrics_apply(lv_obj_t *label);

/** 是否有可用歌词行。 */
bool nodelio_lyrics_ready(void);

/** 当前句下标（相对时间）；无则 -1。 */
int nodelio_lyrics_current_index(uint32_t position_ms);

/** 拷贝第 idx 行文本到 out；失败返回 false。 */
bool nodelio_lyrics_line_text(int idx, char *out, size_t out_len);

int nodelio_lyrics_line_count(void);

/** 歌词候选（长按换源列表）。 */
typedef struct {
    char id[40];
    char source[24];
    char title[64];
    char artist[48];
    char preview[84];
    bool synced;
} nodelio_lyric_candidate_t;

typedef void (*nodelio_lyrics_candidates_cb_t)(bool ok, int n, void *user);

/** 向 SERVER 拉候选；完成后主线程回调。无 server 则 ok=false。 */
void nodelio_lyrics_request_candidates(const nodelio_nvs_config_t *cfg, const char *title,
                                       const char *artist, nodelio_lyrics_candidates_cb_t cb,
                                       void *user);

int nodelio_lyrics_candidate_count(void);
bool nodelio_lyrics_candidate_at(int idx, nodelio_lyric_candidate_t *out);

/** 点选候选：向 SERVER pick 并替换当前歌词。 */
void nodelio_lyrics_apply_pick(const nodelio_nvs_config_t *cfg, const char *pick_id,
                               const char *title, const char *artist);

#ifdef __cplusplus
}
#endif
