#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 注册沉浸页收藏回调；可重复调用。 */
void nodelio_favorites_init(void);

/** 拉取服务端收藏并展示 music_list（来源「收藏」）。 */
void nodelio_skill_request_favorites(void);

/** 收藏/取消当前播放曲；异步 HTTP。 */
void nodelio_media_favorite_toggle_current(void);

/** 当前曲是否在最近一次收藏缓存里。 */
bool nodelio_media_is_current_favorited(void);

/** 根据当前 URL 刷新沉浸页红心状态。 */
void nodelio_favorites_sync_immersive(void);

#ifdef __cplusplus
}
#endif
