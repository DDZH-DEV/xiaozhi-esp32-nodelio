/*
 * ui_theme.h — NodeLio 墙板唯一 UI 令牌。
 * 浅色 / 曜夜两套调色板，几何宏仍是编译期常量。
 * 控件代码只读宏，禁止散落裸 hex。
 */
#pragma once

#include <lvgl.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bg;
    uint32_t surface;
    uint32_t surface_pressed;
    uint32_t surface_off;
    uint32_t panel;
    uint32_t border;
    uint32_t field;
    uint32_t field_focus;
    uint32_t text;
    uint32_t text_sub;
    uint32_t text_dim;
    uint32_t text_muted;
    uint32_t text_tip;
    uint32_t status_chrome;
    uint32_t on_tile;
    uint32_t contrast_dark;
    uint32_t tile_tag;
    uint32_t tile_tag_pwm;
    uint32_t pwm_track;
    uint32_t accent;
    uint32_t accent_soft;
    uint32_t accent_pressed;
    uint32_t selection;
    uint32_t success;
    uint32_t warn;
    uint32_t danger;
    uint32_t info;
    uint32_t purple;
    uint32_t danger_bg;
    uint32_t danger_fg;
    uint32_t back_btn;
    uint32_t offline;
    uint32_t disabled_fg;
    uint32_t slider_track;
    uint32_t tile_switch;
    uint32_t tile_scene;
    uint32_t tile_gpio;
    uint32_t tile_pwm;
    uint32_t tile_off;
    uint32_t tile_on_border;
    uint32_t tile_off_border;
    uint32_t tile_pwm_border;
    uint32_t tile_gpio_border;
    uint32_t capsule_off;
    uint32_t capsule_on;
    uint32_t capsule_pwm;
    uint32_t capsule_gpio;
    uint32_t capsule_scene;
    uint32_t tile_badge_on;
    uint32_t tile_badge_off;
    uint32_t tile_badge_offline;
    uint32_t tab_glow;
    uint32_t shadow;
} ui_palette_t;

const ui_palette_t *ui_palette(void);
/** 0=浅色（默认） 1=曜夜。其它值按浅色。 */
void ui_palette_set(int mode);
int ui_palette_is_dark(void);

#ifdef __cplusplus
}
#endif

/* Opaque surfaces avoid blur buffers and repeated alpha composition on S3/P4. */
#define UI_C_BG (ui_palette()->bg)
#define UI_C_SURFACE (ui_palette()->surface)
#define UI_C_SURFACE_PRESSED (ui_palette()->surface_pressed)
#define UI_C_SURFACE_OFF (ui_palette()->surface_off)
#define UI_C_PANEL (ui_palette()->panel)
#define UI_C_BORDER (ui_palette()->border)
#define UI_C_SHADOW (ui_palette()->shadow)
#define UI_C_FIELD (ui_palette()->field)
#define UI_C_FIELD_FOCUS (ui_palette()->field_focus)
#define UI_C_TEXT (ui_palette()->text)
#define UI_C_TEXT_SUB (ui_palette()->text_sub)
#define UI_C_TEXT_DIM (ui_palette()->text_dim)
#define UI_C_TEXT_MUTED (ui_palette()->text_muted)
#define UI_C_TEXT_TIP (ui_palette()->text_tip)
#define UI_C_STATUS_CHROME (ui_palette()->status_chrome)
#define UI_C_ON_TILE (ui_palette()->on_tile)
#define UI_C_CONTRAST_DARK (ui_palette()->contrast_dark)
#define UI_C_TILE_TAG (ui_palette()->tile_tag)
#define UI_C_TILE_TAG_PWM (ui_palette()->tile_tag_pwm)
#define UI_C_PWM_TRACK (ui_palette()->pwm_track)
#define UI_C_ACCENT (ui_palette()->accent)
#define UI_C_ACCENT_SOFT (ui_palette()->accent_soft)
#define UI_C_ACCENT_PRESSED (ui_palette()->accent_pressed)
#define UI_C_SELECTION (ui_palette()->selection)
#define UI_C_SUCCESS (ui_palette()->success)
#define UI_C_WARN (ui_palette()->warn)
#define UI_C_DANGER (ui_palette()->danger)
#define UI_C_INFO (ui_palette()->info)
#define UI_C_PURPLE (ui_palette()->purple)
#define UI_C_DANGER_BG (ui_palette()->danger_bg)
#define UI_C_DANGER_FG (ui_palette()->danger_fg)
#define UI_C_BACK_BTN (ui_palette()->back_btn)
#define UI_C_OFFLINE (ui_palette()->offline)
#define UI_C_DISABLED_FG (ui_palette()->disabled_fg)
#define UI_C_SLIDER_TRACK (ui_palette()->slider_track)
#define UI_C_TILE_SWITCH (ui_palette()->tile_switch)
#define UI_C_TILE_SCENE (ui_palette()->tile_scene)
#define UI_C_TILE_GPIO (ui_palette()->tile_gpio)
#define UI_C_TILE_PWM (ui_palette()->tile_pwm)
#define UI_C_TILE_OFF (ui_palette()->tile_off)
#define UI_C_TILE_SWITCH_OFF UI_C_TILE_OFF
#define UI_C_TILE_SCENE_OFF UI_C_TILE_OFF
#define UI_C_TILE_GPIO_OFF UI_C_TILE_OFF
#define UI_C_TILE_PWM_OFF UI_C_TILE_OFF
#define UI_C_TILE_ON_BORDER (ui_palette()->tile_on_border)
#define UI_C_TILE_OFF_BORDER (ui_palette()->tile_off_border)
#define UI_C_TILE_PWM_BORDER (ui_palette()->tile_pwm_border)
#define UI_C_TILE_GPIO_BORDER (ui_palette()->tile_gpio_border)
#define UI_C_CAPSULE_OFF (ui_palette()->capsule_off)
#define UI_C_CAPSULE_ON (ui_palette()->capsule_on)
#define UI_C_CAPSULE_PWM (ui_palette()->capsule_pwm)
#define UI_C_CAPSULE_GPIO (ui_palette()->capsule_gpio)
#define UI_C_CAPSULE_SCENE (ui_palette()->capsule_scene)
#define UI_C_TILE_BADGE_ON (ui_palette()->tile_badge_on)
#define UI_C_TILE_BADGE_OFF (ui_palette()->tile_badge_off)
#define UI_C_TILE_BADGE_OFFLINE (ui_palette()->tile_badge_offline)
#define UI_C_TAB_GLOW (ui_palette()->tab_glow)

#define UI_BG lv_color_hex(UI_C_BG)
#define UI_SURFACE lv_color_hex(UI_C_SURFACE)
#define UI_SURFACE_PRESSED lv_color_hex(UI_C_SURFACE_PRESSED)
#define UI_SURFACE_OFF lv_color_hex(UI_C_SURFACE_OFF)
#define UI_PANEL lv_color_hex(UI_C_PANEL)
#define UI_BORDER lv_color_hex(UI_C_BORDER)
#define UI_SHADOW lv_color_hex(UI_C_SHADOW)
#define UI_FIELD lv_color_hex(UI_C_FIELD)
#define UI_FIELD_FOCUS lv_color_hex(UI_C_FIELD_FOCUS)
#define UI_TEXT lv_color_hex(UI_C_TEXT)
#define UI_TEXT_SUB lv_color_hex(UI_C_TEXT_SUB)
#define UI_TEXT_DIM lv_color_hex(UI_C_TEXT_DIM)
#define UI_TEXT_MUTED lv_color_hex(UI_C_TEXT_MUTED)
#define UI_TEXT_TIP lv_color_hex(UI_C_TEXT_TIP)
#define UI_STATUS_CHROME lv_color_hex(UI_C_STATUS_CHROME)
#define UI_ON_TILE lv_color_hex(UI_C_ON_TILE)
#define UI_CONTRAST_DARK lv_color_hex(UI_C_CONTRAST_DARK)
#define UI_TILE_TAG lv_color_hex(UI_C_TILE_TAG)
#define UI_TILE_TAG_PWM lv_color_hex(UI_C_TILE_TAG_PWM)
#define UI_PWM_TRACK lv_color_hex(UI_C_PWM_TRACK)
#define UI_ACCENT lv_color_hex(UI_C_ACCENT)
#define UI_ACCENT_PRESSED lv_color_hex(UI_C_ACCENT_PRESSED)
#define UI_SELECTION lv_color_hex(UI_C_SELECTION)
#define UI_SUCCESS lv_color_hex(UI_C_SUCCESS)
#define UI_ACCENT_SOFT lv_color_hex(UI_C_ACCENT_SOFT)
#define UI_WARN lv_color_hex(UI_C_WARN)
#define UI_DANGER lv_color_hex(UI_C_DANGER)
#define UI_INFO lv_color_hex(UI_C_INFO)
#define UI_PURPLE lv_color_hex(UI_C_PURPLE)
#define UI_DANGER_BG lv_color_hex(UI_C_DANGER_BG)
#define UI_DANGER_FG lv_color_hex(UI_C_DANGER_FG)
#define UI_BACK_BTN lv_color_hex(UI_C_BACK_BTN)
#define UI_OFFLINE lv_color_hex(UI_C_OFFLINE)
#define UI_DISABLED_FG lv_color_hex(UI_C_DISABLED_FG)
#define UI_SLIDER_TRACK lv_color_hex(UI_C_SLIDER_TRACK)
#define UI_TILE_OFF lv_color_hex(UI_C_TILE_OFF)
#define UI_TILE_SWITCH lv_color_hex(UI_C_TILE_SWITCH)
#define UI_TILE_SWITCH_OFF lv_color_hex(UI_C_TILE_SWITCH_OFF)
#define UI_TILE_SCENE lv_color_hex(UI_C_TILE_SCENE)
#define UI_TILE_SCENE_OFF lv_color_hex(UI_C_TILE_SCENE_OFF)
#define UI_TILE_GPIO lv_color_hex(UI_C_TILE_GPIO)
#define UI_TILE_GPIO_OFF lv_color_hex(UI_C_TILE_GPIO_OFF)
#define UI_TILE_PWM lv_color_hex(UI_C_TILE_PWM)
#define UI_TILE_PWM_OFF lv_color_hex(UI_C_TILE_PWM_OFF)
#define UI_TILE_ON_BORDER lv_color_hex(UI_C_TILE_ON_BORDER)
#define UI_TILE_OFF_BORDER lv_color_hex(UI_C_TILE_OFF_BORDER)
#define UI_TILE_PWM_BORDER lv_color_hex(UI_C_TILE_PWM_BORDER)
#define UI_TILE_GPIO_BORDER lv_color_hex(UI_C_TILE_GPIO_BORDER)
#define UI_CAPSULE_OFF lv_color_hex(UI_C_CAPSULE_OFF)
#define UI_CAPSULE_ON lv_color_hex(UI_C_CAPSULE_ON)
#define UI_CAPSULE_PWM lv_color_hex(UI_C_CAPSULE_PWM)
#define UI_CAPSULE_GPIO lv_color_hex(UI_C_CAPSULE_GPIO)
#define UI_CAPSULE_SCENE lv_color_hex(UI_C_CAPSULE_SCENE)
#define UI_TILE_BADGE_ON lv_color_hex(UI_C_TILE_BADGE_ON)
#define UI_TILE_BADGE_OFF lv_color_hex(UI_C_TILE_BADGE_OFF)
#define UI_TILE_BADGE_OFFLINE lv_color_hex(UI_C_TILE_BADGE_OFFLINE)
#define UI_TAB_GLOW lv_color_hex(UI_C_TAB_GLOW)

/* ---- geometry（8px 网格） ----------------------------------------------- */
#define UI_RADIUS 20
#define UI_RADIUS_MD 16
#define UI_RADIUS_SM 12
#define UI_RADIUS_XS 6
#define UI_PAD 12
#define UI_PAD_SM 8
#define UI_GAP 8
#define UI_GAP_LG 14
#define UI_EXT_CLICK 20
/* 首页磁贴动效（开/关色过渡、图标弹跳、pending 转圈） */
#define UI_TILE_TRANS_MS 120
#define UI_TILE_ICON_POP_MS 120
#define UI_TILE_SPIN_MS 900
#define UI_SETTINGS_TILE_H 72
#define UI_NAV_H 40
#define UI_NAV_BACK_W 64
#define UI_BTN_H 48
#define UI_BTN_H_SM 44
/* 小智对话框。宽度、边框色、边框粗细、阴影都在这里改。
 * 边框不跟聆听/说话变色（原先聆听会刷成信息蓝）。 */
#define UI_AI_DRAWER_W 348
#define UI_AI_DRAWER_BORDER 0xe3e8ee
#define UI_AI_DRAWER_BORDER_W 1
#define UI_AI_DRAWER_SHADOW_W 12
#define UI_AI_DRAWER_SHADOW_Y 3
#define UI_AI_DRAWER_SHADOW_OPA LV_OPA_40
#define UI_AI_DRAWER_H 208 /* 含 48px 头栏（关闭/最小化可点） */
#define UI_AI_DRAWER_COMPACT_W 296 /* 唤醒建链条：贴文案宽度 */
#define UI_AI_DRAWER_COMPACT_H 72  /* 唤醒建链条：头栏按钮 44px */
#define UI_AI_DRAWER_TEXT_H 120
#define UI_AI_DRAWER_HDR_H 48 /* 头栏：对齐 44px 触控钮 */
#define UI_AI_DRAWER_BTN_SZ 44 /* 最小化/关闭：≥44 防穿透下层卡 */
#define UI_AI_DRAWER_BOTTOM 52
#define UI_AI_DRAWER_BG_OPA LV_OPA_COVER /* 保证正文对比度，避免反复透明混合 */
#define UI_AI_MINI_SZ 52 /* 最小化圆钮（仅图标，无文字） */
#define UI_AI_MINI_RIGHT 14
/* 新闻阅读：0=14px 普惠；1/2=资源 20px 真字库（禁止 transform 放大点阵，会糊） */
#define UI_NEWS_ZOOM_MAX 2
#define UI_NEWS_LINE_SPACE_0 4
#define UI_NEWS_LINE_SPACE_1 6
#define UI_NEWS_LINE_SPACE_2 10
#define UI_NEWS_LETTER_SPACE_2 1
