#pragma once

#include "nvs_config.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 是否曲库技能（兼容 market_id::music.search / music_search）。 */
bool nodelio_skill_id_is_music(const char *skill_id);

/**
 * 解析 Envelope JSON → 上屏；spoken_out 写短摘要给 LLM（可空）。
 * auto_play_first：调用方强制播首条（打开曲库）；与 Envelope display.auto_play 任一为真即播。
 */
void nodelio_skill_deliver_json(const nodelio_nvs_config_t *cfg, const char *json, bool auto_play_first,
                                char *spoken_out, size_t spoken_len);

#ifdef __cplusplus
}
#endif
