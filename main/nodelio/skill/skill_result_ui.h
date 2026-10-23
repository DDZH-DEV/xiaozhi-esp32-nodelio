#pragma once

#include "content_envelope.h"
#include "nvs_config.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 展示 Envelope：会话抽屉上方 card；新结果覆盖旧结果。 */
void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, const nl_content_envelope_t *env,
                               bool auto_play_first);

void nodelio_skill_result_hide(void);

/** 技能列表/结果卡是否仍显示（本地开应用后勿 goodbye 息屏）。 */
bool nodelio_skill_result_is_visible(void);

/** 停播并收起技能卡/播放条/悬浮球（语音「关闭电台」或 MCP close）。 */
void nodelio_skill_close_all(void);

/** 技能卡可见时抬到 layer_top 最前（防小智抽屉抢 z-order）。 */
void nodelio_skill_result_raise(void);

/** now_playing 条（media 回调；无 media 能力时为空操作）。
 *  playing=true 播放中；playing=false 且 title 非空=已暂停（保留条）；title 空=关闭。 */
void nodelio_panel_ui_media_now_playing(const char *title, bool playing);

/** 语音/MCP：切歌；无当前列表或边界时返回 false。 */
bool nodelio_skill_media_prev(void);
bool nodelio_skill_media_next(void);

/** 语音/MCP：从当前列表随机（或唯一可播）开播，不重新搜歌。
 *  成功时 title_out 写入曲名（可空）；无列表返回 false。 */
bool nodelio_skill_media_play_any(char *title_out, size_t title_out_len);

/** 语音/MCP：新闻。
 *  op: "prev"|"next"|"read"|"continue"|"mark_read"|"skip"|"close"
 *  成功时 out 写入短回包（分段正文，避免 MQTT 超长被掐断）。
 *  next/skip/mark_read 会把当前篇标为已读。close 关掉新闻卡。 */
bool nodelio_skill_news_action(const char *op, char *out, size_t out_len);

/**
 * 用户原话要关技能弹窗时：停播并收起（新闻/电台/播客/曲库/天气等）。
 * 已处理返回 true。
 */
bool nodelio_skill_try_dismiss_phrase(const char *text);

/**
 * DSH 列表可见时：「第几个工作区/任务」→ 进入或续聊。
 * 已处理返回 true。
 */
bool nodelio_skill_try_pick_phrase(const char *text);

/**
 * 续聊武装后语音派活已发出：进度卡切到「已投递/执行中」动画。
 * instruction 可空；卡未开时为空操作。
 */
void nodelio_skill_dsh_on_dispatched(const char *instruction);

/** 续聊待确认发送：instruction 非空显示预览+发送/取消；空则收起确认条。 */
void nodelio_skill_dsh_on_confirm_pending(const char *instruction);

/** arm 超时/用户取消续聊：进度卡回 IDLE。 */
void nodelio_skill_dsh_on_arm_cleared(void);

#ifdef __cplusplus
}
#endif
