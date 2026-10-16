#pragma once

#include "nvs_config.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析 Envelope JSON → 上屏；spoken_out 写短摘要给 LLM（可空）。
 * auto_play_first：仅当已申报 media.play_local 时播首条。
 */
void nodelio_skill_deliver_json(const nodelio_nvs_config_t *cfg, const char *json, bool auto_play_first,
                                char *spoken_out, size_t spoken_len);

#ifdef __cplusplus
}
#endif
