#pragma once

#include "ui_theme.h"

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_styles_init(void);
/** 按当前调色板重写静态样式和默认主题。可重复调用。 */
void ui_styles_apply(void);
/** 浅色卡片短投影；深色保持无阴影。 */
void ui_style_card(lv_obj_t *obj);
/** Contrast-safe foreground for user-defined solid layout colors. */
lv_color_t ui_color_on(lv_color_t background);

/** 设置 hub 分组卡：标题 + 圆角容器，返回挂磁贴的父对象。
 *  508 组内两列 wrap；其它板组内单列全宽行。 */
lv_obj_t *ui_make_settings_section(lv_obj_t *parent, const char *title);
/** 磁贴墙列宽（508 两列等）；force_single_col 时取整屏约 1/5。 */
int ui_settings_tile_width(int avail, bool force_single_col, int *columns_out);
/** 分组内行高（标题 1 行 + 副文 1 行，不低于 UI_SETTINGS_TILE_H）。 */
int ui_settings_tile_height(void);

/** 设置入口卡：左 Icon Pod · 右标题/副文。
 *  全板均挂在分组内；508 为组内两列磁贴，其它板为全宽行。force_single_col：宽屏技能卡占右时左侧单列。 */
lv_obj_t *ui_make_settings_tile(lv_obj_t *parent, const char *glyph, const char *title,
                                const char *sub, uint32_t accent_c, const char *right_icon,
                                lv_event_cb_t cb, bool force_single_col);

/** 底栏 Tab；checked 用 LV_STATE_CHECKED. */
lv_obj_t *ui_make_tab(lv_obj_t *parent, const char *txt);
void ui_tab_set_checked(lv_obj_t *btn, bool checked);

/**
 * 内页顶栏（高度 h）：背景 UI_PANEL，左返回胶囊 + 标题。
 * 返回点击调用 on_back（可为 NULL）.
 */
lv_obj_t *ui_make_nav_bar(lv_obj_t *parent, int h, const char *title, lv_event_cb_t on_back);

/** 全宽文本按钮：kind 0=主色 1=危险 2=中性. */
lv_obj_t *ui_make_text_btn(lv_obj_t *parent, const char *label, int kind, lv_event_cb_t cb);

/** 表单内小按钮（扫描/协议等）：绿底 + pressed. */
lv_obj_t *ui_make_action_btn(lv_obj_t *parent, const char *label, int w, int h, lv_event_cb_t cb);

void ui_style_slider(lv_obj_t *sl);
void ui_style_switch(lv_obj_t* sw);

/** 驱动禁用态（灰显 + 不可点）. */
void ui_set_enabled(lv_obj_t *obj, bool enabled);

#ifdef __cplusplus
}
#endif
