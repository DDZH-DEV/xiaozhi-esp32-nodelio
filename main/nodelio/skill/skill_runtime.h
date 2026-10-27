#pragma once

#include "content_envelope.h"
#include "nvs_config.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 是否音频列表类技能（曲库 / 电台 / 播客；兼容 market:: 前缀）。 */
bool nodelio_skill_id_is_music(const char *skill_id);

/**
 * 解析 Envelope JSON → 上屏；spoken_out 写短摘要给 LLM（可空）。
 * auto_play_first：调用方强制播首条（打开曲库）；与 Envelope display.auto_play 任一为真即播。
 */
void nodelio_skill_deliver_json(const nodelio_nvs_config_t *cfg, const char *json, bool auto_play_first,
                                char *spoken_out, size_t spoken_len);

/** 最近一次 deliver 的 spoken.audio.url（无则 NULL）；供本地 announce 播报。 */
const char *nodelio_skill_last_spoken_audio_url(void);
/** 连续播报：写入 urls[0..n)，最多 max_n；返回段数。 */
int nodelio_skill_last_spoken_audio_urls(char (*urls)[NL_ENV_URL], int max_n);

#ifdef __cplusplus
}
#endif
