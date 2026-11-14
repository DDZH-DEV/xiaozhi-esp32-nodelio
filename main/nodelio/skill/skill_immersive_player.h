#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 播放中空闲进入沉浸页；触摸退出。live/episode 不进。 */
void nodelio_skill_immersive_on_now_playing(const char *title, bool playing);

/** 用户活动：重置进入计时；若已沉浸则退出。 */
void nodelio_skill_immersive_note_activity(void);

bool nodelio_skill_immersive_visible(void);

void nodelio_skill_immersive_hide(void);

/** 收藏钮回调（由 favorites 模块注册；可空）。 */
typedef void (*nodelio_immersive_fav_cb_t)(void);
void nodelio_skill_immersive_set_fav_cb(nodelio_immersive_fav_cb_t cb);
void nodelio_skill_immersive_set_favorited(bool on);

#ifdef __cplusplus
}
#endif
