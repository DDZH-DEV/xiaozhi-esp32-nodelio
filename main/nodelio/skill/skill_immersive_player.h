#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 同步播放状态（仅宽屏 P4；sila-86 方屏禁用）。
 * 不自动进沉浸；进沉浸只走底栏按钮。live 不进。
 */
void nodelio_skill_immersive_on_now_playing(const char *title, bool playing);

/** 用户主动进入沉浸（底栏按钮）。宽屏 + 非 live + 正在播/暂停。 */
void nodelio_skill_immersive_show(void);

/** 兼容旧调用；沉浸不再空闲自动进入，此函数为空操作。 */
void nodelio_skill_immersive_note_activity(void);

/** 播放器设置改黑胶旋转后立刻套用（沉浸未开则无操作）。 */
void nodelio_skill_immersive_apply_vinyl_pref(void);

bool nodelio_skill_immersive_visible(void);

void nodelio_skill_immersive_hide(void);

/** 收藏钮回调（由 favorites 模块注册；可空）。 */
typedef void (*nodelio_immersive_fav_cb_t)(void);
void nodelio_skill_immersive_set_fav_cb(nodelio_immersive_fav_cb_t cb);
void nodelio_skill_immersive_set_favorited(bool on);

#ifdef __cplusplus
}
#endif
