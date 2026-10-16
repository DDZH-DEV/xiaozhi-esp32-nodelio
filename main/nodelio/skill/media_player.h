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

void nodelio_media_now_title(char *buf, size_t len);

/** HTTP 通知音结束/失败时由 Application 回调，清理 playing 态。 */
void nodelio_media_on_playback_ended(bool success);

/** 兼容旧调用：仅表示「正在播放」（息屏策略见 media_sleep_en）。 */
bool nodelio_media_has_session(void);

#ifdef __cplusplus
}
#endif
