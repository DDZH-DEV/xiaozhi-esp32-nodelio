#pragma once

#include "content_envelope.h"
#include "nvs_config.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 展示 Envelope：会话抽屉上方 card；新结果覆盖旧结果。
 * env 须为堆分配；本函数接管所有权（含 detail_body），调用方勿再 free。
 * 避免解析侧 + UI 侧各一份 ~54KB 信封把 PSRAM 打满（开 MultiNet 后尤甚）。
 */
void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, nl_content_envelope_t *env,
                               bool auto_play_first);

void nodelio_skill_result_hide(void);

/** Built-in calendar opens offline; refresh may only update this open session. */
uint32_t nodelio_skill_daily_open(bool reset_view);
void nodelio_skill_daily_update(const nodelio_nvs_config_t* cfg, nl_content_envelope_t* env,
                                uint32_t generation);
/** Reuse the shell clock tick; updates date highlighting only, never requests weather. */
void nodelio_skill_daily_clock_tick(void);

/** 技能列表/结果卡是否仍显示（本地开应用后勿 goodbye 息屏）。 */
bool nodelio_skill_result_is_visible(void);
bool nodelio_skill_result_card_visible(void);

/** 停播并收起技能卡/播放条/悬浮球（语音「关闭电台」或 MCP close）。 */
void nodelio_skill_close_all(void);

/** 技能卡可见时抬到 layer_top 最前（防小智抽屉抢 z-order）。 */
void nodelio_skill_result_raise(void);

/** 应用分栏宽变化后重算卡/播放条宽度与落位。 */
void nodelio_skill_result_layout(void);

/** 全局显示文字档变更后重刷当前技能卡字体/列表。 */
void nodelio_skill_result_reapply_text_scale(void);

/** 音乐折叠后的右下悬浮球是否可见（应用页刷新钮要给它让位）。 */
bool nodelio_skill_mini_fab_visible(void);

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
 * 本地旁路朗读：op=read|continue。
 * 打开详情、拉正文、跟读滚屏，并把可播报原文写入 plain_out（无 MCP 指令壳）。
 */
bool nodelio_skill_news_local_speak(const char *op, char *plain_out, size_t plain_len);

/** 仅确保详情打开（列表态进当前/首条），不推进朗读游标。 */
bool nodelio_skill_news_prepare_read(void);

/** Event-driven news reading; no estimated-speed scroll timer. */
uint32_t nodelio_skill_news_read_token(void);
uint32_t nodelio_skill_news_on_sentence(const char* text);
bool nodelio_skill_news_finish_chunk(uint32_t token, bool played_audio, bool* more);
void nodelio_skill_news_cancel_read(void);
/** 详情底栏：朗读 / 暂停 / 继续。 */
void nodelio_skill_news_sync_read_btn(void);

/** 当前技能是否为 news_list 且卡/播放层仍在前台（语音「下一篇」仅此时命中）。 */
bool nodelio_skill_news_ui_active(void);

/** 当前 media_kind 为 live（电台）；切歌口令应失败，「换台」回列表。 */
bool nodelio_skill_is_live_radio(void);

/** 展开当前 music_list 列表卡（换台）；无列表返回 false。 */
bool nodelio_skill_reopen_media_list(void);

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
 * 新闻卡前台：「第 N 条/打开第一条/读第二条」→ 进详情；含读/念则 StartLocalNewsRead（keep_cloud=true）。
 * 已处理返回 true。
 */
bool nodelio_skill_try_news_pick_phrase(const char *text, bool *keep_cloud_tts);

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
