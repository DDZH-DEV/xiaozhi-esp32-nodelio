#pragma once

#include "content_envelope.h"
#include "nvs_config.h"

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化媒体任务队列（PSRAM 栈）。 */
esp_err_t nodelio_media_init(void);

bool nodelio_media_is_playing(void);

/** 已暂停但仍有曲目会话（播放器应保留）。 */
bool nodelio_media_is_paused(void);

/** 同 URL 正在播（或刚启动防抖窗内）则为 true；用于 skill auto_play 避免反复掐流。 */
bool nodelio_media_is_playing_url(const char *url);

/** 播放曲目：互斥小智会话；url 可为相对路径 / sample://。同 URL 不重启。 */
esp_err_t nodelio_media_play_track(const nodelio_nvs_config_t *cfg, const nl_env_track_t *track);

void nodelio_media_stop(void);
/** 暂停：停流但保留标题与播放器 UI；再点可 resume。 */
void nodelio_media_pause(void);
void nodelio_media_resume(void);

/**
 * 唤醒/开麦打断播放：同步停 HTTP，保留曲目，不启动「暂停后关播放器」计时。
 * 须在 Application 主任务调用。退下后用 nodelio_media_resume_after_voice。
 */
void nodelio_media_pause_for_voice(void);
/**
 * 语音通道关闭 / 离线下退后：若曾 pause_for_voice 则续播并返回 true（调用方应跳过 goodbye 息屏）。
 */
bool nodelio_media_resume_after_voice(void);

/**
 * 打开新闻等重应用前：停播并等到 HTTP 播放器空闲（最多约 5s）。
 * 返回 true 表示本次确实停过媒体（可 toast）。
 */
bool nodelio_media_preempt_for_heavy_app(void);

void nodelio_media_now_title(char *buf, size_t len);
/** 当前曲艺人（可空）。 */
void nodelio_media_now_artist(char *buf, size_t len);
/** 当前播放 URL（http / sd:// / 相对路径解析后），可空。 */
void nodelio_media_now_url(char *buf, size_t len);

/** 已播毫秒 / 估算总时长（0=未知或直播）。 */
uint32_t nodelio_media_position_ms(void);
uint32_t nodelio_media_duration_ms(void);
/**
 * Seek：按比例映射字节并重启播放（本地 fseek / HTTP Range）。
 * duration 未知或直播返回 false。
 */
bool nodelio_media_seek_ms(uint32_t ms);

/** HTTP 通知音结束/失败时由 Application 回调，清理 playing 态。 */
void nodelio_media_on_playback_ended(bool success);

/** 兼容旧调用：仅表示「正在播放」（息屏策略见 media_sleep_en）。 */
bool nodelio_media_has_session(void);

/** 已武装唤醒压音时按当前 NVS 百分比重算；未压音则忽略。 */
void nodelio_media_wake_duck_refresh(void);

/**
 * 点选开播后的短窗口，或手指刚按下时：忽略唤醒。
 * 切流和敲屏都会让 WakeNet 误报，把对话窗顶出来。
 */
bool nodelio_media_wake_blocked(void);
/** 手指按下：短时间挡住误唤醒。 */
void nodelio_media_note_touch(void);
/** 误唤醒被丢掉之后，等屏蔽窗结束再打开唤醒词。 */
void nodelio_media_wake_rearm_later(void);
/** 误唤醒被丢掉后，等屏蔽窗结束再把 WakeNet 打开。 */
void nodelio_media_wake_defer_rearm(void);

#ifdef __cplusplus
}
#endif
