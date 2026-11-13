#pragma once

#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 在 s_root 上创建左侧悬浮白色胶囊 Dock（仅 P4 62 / 508 编入实现）。须持 LVGL 锁调用。 */
void nl_dock_init(lv_obj_t *root);

/** 同步沉浸模式 Dock 与经典 s_tabbar / s_body 状态，并高亮当前 Tab。须持 LVGL 锁调用。 */
void nl_dock_sync(lv_obj_t *tabbar, lv_obj_t *body, int active_tab, bool shell_visible);

/** 当前左侧导航占用的水平像素宽度（62 恒为 nav_width=96；508 沉浸模式为 84，经典模式为 0）。 */
int nl_dock_left_inset(void);

#ifdef __cplusplus
}
#endif

