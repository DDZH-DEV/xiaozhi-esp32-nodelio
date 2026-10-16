#pragma once

#include "content_envelope.h"
#include "nvs_config.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 展示 Envelope：会话抽屉上方 card；新结果覆盖旧结果。 */
void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, const nl_content_envelope_t *env,
                               bool auto_play_first);

void nodelio_skill_result_hide(void);

/** 技能卡可见时抬到 layer_top 最前（防小智抽屉抢 z-order）。 */
void nodelio_skill_result_raise(void);

/** now_playing 条（media 回调；无 media 能力时为空操作）。
 *  playing=true 播放中；playing=false 且 title 非空=已暂停（保留条）；title 空=关闭。 */
void nodelio_panel_ui_media_now_playing(const char *title, bool playing);

/** 语音/MCP：切歌；无当前列表或边界时返回 false。 */
bool nodelio_skill_media_prev(void);
bool nodelio_skill_media_next(void);

/** 语音/MCP：新闻详情。delta<0 上一篇，>0 下一篇，0 读当前（未打开则打开第一篇）。
 *  成功时 out 写入「标题 + 正文」（可空）；正文同步向云端拉取。 */
bool nodelio_skill_news_step(int delta, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
