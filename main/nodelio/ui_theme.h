/*
 * ui_theme.h — NodeLio 墙板唯一 UI 令牌。
 * Apple-inspired light appearance: grouped surfaces, semantic color and quiet chrome.
 * 控件代码只读宏，禁止散落裸 hex。
 */
#pragma once

#include <lvgl.h>

/* ---- raw (uint32_t) ----------------------------------------------------- */
/* Opaque surfaces avoid blur buffers and repeated alpha composition on S3/P4. */
#define UI_C_BG              0xf2f2f7
#define UI_C_SURFACE         0xffffff
#define UI_C_SURFACE_PRESSED 0xe5e5ea
#define UI_C_SURFACE_OFF     0xe5e5ea
#define UI_C_PANEL           0xffffff
#define UI_C_BORDER          0xd1d1d6
#define UI_C_FIELD           0xf2f2f7
#define UI_C_FIELD_FOCUS     0xe8f1ff
#define UI_C_TEXT            0x1c1c1e
#define UI_C_TEXT_SUB        0x48484a
#define UI_C_TEXT_DIM        0x636366
#define UI_C_TEXT_MUTED      0x636366
#define UI_C_TEXT_TIP        0x636366
#define UI_C_STATUS_CHROME   0x48484a
#define UI_C_ON_TILE         0xffffff
#define UI_C_TILE_TAG        0x218539
#define UI_C_TILE_TAG_PWM    0x0066cc
#define UI_C_PWM_TRACK       0xd1d1d6
#define UI_C_ACCENT          0x0066cc
#define UI_C_ACCENT_SOFT     UI_C_ACCENT
#define UI_C_ACCENT_PRESSED  0x004c99
#define UI_C_SELECTION       0xe8f1ff
#define UI_C_SUCCESS         0x218539
#define UI_C_WARN            0x985600
#define UI_C_DANGER          0xc9342a
#define UI_C_INFO            UI_C_ACCENT
#define UI_C_PURPLE          0x8944ab
#define UI_C_DANGER_BG       0xffeceb
#define UI_C_DANGER_FG       0xb42318
#define UI_C_BACK_BTN        UI_C_SELECTION
#define UI_C_OFFLINE         0xe5e5ea
#define UI_C_DISABLED_FG     0x8e8e93
#define UI_C_SLIDER_TRACK    0xd1d1d6
/* Active device tint; titles retain dark text, badges carry the semantic color. */
#define UI_C_TILE_SWITCH     0xeaf5ed
#define UI_C_TILE_SCENE      0xf4ecf8
#define UI_C_TILE_GPIO       0xfff3e5
#define UI_C_TILE_PWM        UI_C_SELECTION
#define UI_C_TILE_OFF        UI_C_SURFACE
#define UI_C_TILE_SWITCH_OFF UI_C_TILE_OFF
#define UI_C_TILE_SCENE_OFF  UI_C_TILE_OFF
#define UI_C_TILE_GPIO_OFF   UI_C_TILE_OFF
#define UI_C_TILE_PWM_OFF    UI_C_TILE_OFF
#define UI_C_TILE_ON_BORDER  UI_C_SUCCESS
#define UI_C_TILE_OFF_BORDER UI_C_BORDER
#define UI_C_TILE_PWM_BORDER UI_C_ACCENT
#define UI_C_TILE_GPIO_BORDER UI_C_WARN
#define UI_C_CAPSULE_OFF     0xe5e5ea
#define UI_C_CAPSULE_ON      UI_C_SUCCESS
#define UI_C_CAPSULE_PWM     UI_C_ACCENT
#define UI_C_CAPSULE_GPIO    UI_C_WARN
#define UI_C_CAPSULE_SCENE   UI_C_PURPLE
#define UI_C_TILE_BADGE_ON   UI_C_SUCCESS
#define UI_C_TILE_BADGE_OFF  0xe5e5ea
#define UI_C_TILE_BADGE_OFFLINE 0xd1d1d6
#define UI_C_TAB_GLOW        UI_C_SELECTION

/* ---- lv_color ----------------------------------------------------------- */
#define UI_BG               lv_color_hex(UI_C_BG)
#define UI_SURFACE          lv_color_hex(UI_C_SURFACE)
#define UI_SURFACE_PRESSED  lv_color_hex(UI_C_SURFACE_PRESSED)
#define UI_SURFACE_OFF      lv_color_hex(UI_C_SURFACE_OFF)
#define UI_PANEL            lv_color_hex(UI_C_PANEL)
#define UI_BORDER           lv_color_hex(UI_C_BORDER)
#define UI_FIELD            lv_color_hex(UI_C_FIELD)
#define UI_FIELD_FOCUS      lv_color_hex(UI_C_FIELD_FOCUS)
#define UI_TEXT             lv_color_hex(UI_C_TEXT)
#define UI_TEXT_SUB         lv_color_hex(UI_C_TEXT_SUB)
#define UI_TEXT_DIM         lv_color_hex(UI_C_TEXT_DIM)
#define UI_TEXT_MUTED       lv_color_hex(UI_C_TEXT_MUTED)
#define UI_TEXT_TIP         lv_color_hex(UI_C_TEXT_TIP)
#define UI_STATUS_CHROME    lv_color_hex(UI_C_STATUS_CHROME)
#define UI_ON_TILE          lv_color_hex(UI_C_ON_TILE)
#define UI_TILE_TAG         lv_color_hex(UI_C_TILE_TAG)
#define UI_TILE_TAG_PWM     lv_color_hex(UI_C_TILE_TAG_PWM)
#define UI_PWM_TRACK        lv_color_hex(UI_C_PWM_TRACK)
#define UI_ACCENT           lv_color_hex(UI_C_ACCENT)
#define UI_ACCENT_PRESSED   lv_color_hex(UI_C_ACCENT_PRESSED)
#define UI_SELECTION        lv_color_hex(UI_C_SELECTION)
#define UI_SUCCESS          lv_color_hex(UI_C_SUCCESS)
#define UI_ACCENT_SOFT      lv_color_hex(UI_C_ACCENT_SOFT)
#define UI_WARN             lv_color_hex(UI_C_WARN)
#define UI_DANGER           lv_color_hex(UI_C_DANGER)
#define UI_INFO             lv_color_hex(UI_C_INFO)
#define UI_PURPLE           lv_color_hex(UI_C_PURPLE)
#define UI_DANGER_BG        lv_color_hex(UI_C_DANGER_BG)
#define UI_DANGER_FG        lv_color_hex(UI_C_DANGER_FG)
#define UI_BACK_BTN         lv_color_hex(UI_C_BACK_BTN)
#define UI_OFFLINE          lv_color_hex(UI_C_OFFLINE)
#define UI_DISABLED_FG      lv_color_hex(UI_C_DISABLED_FG)
#define UI_SLIDER_TRACK     lv_color_hex(UI_C_SLIDER_TRACK)
#define UI_TILE_OFF         lv_color_hex(UI_C_TILE_OFF)
#define UI_TILE_SWITCH      lv_color_hex(UI_C_TILE_SWITCH)
#define UI_TILE_SWITCH_OFF  lv_color_hex(UI_C_TILE_SWITCH_OFF)
#define UI_TILE_SCENE       lv_color_hex(UI_C_TILE_SCENE)
#define UI_TILE_SCENE_OFF   lv_color_hex(UI_C_TILE_SCENE_OFF)
#define UI_TILE_GPIO        lv_color_hex(UI_C_TILE_GPIO)
#define UI_TILE_GPIO_OFF    lv_color_hex(UI_C_TILE_GPIO_OFF)
#define UI_TILE_PWM         lv_color_hex(UI_C_TILE_PWM)
#define UI_TILE_PWM_OFF     lv_color_hex(UI_C_TILE_PWM_OFF)
#define UI_TILE_ON_BORDER   lv_color_hex(UI_C_TILE_ON_BORDER)
#define UI_TILE_OFF_BORDER  lv_color_hex(UI_C_TILE_OFF_BORDER)
#define UI_TILE_PWM_BORDER  lv_color_hex(UI_C_TILE_PWM_BORDER)
#define UI_TILE_GPIO_BORDER lv_color_hex(UI_C_TILE_GPIO_BORDER)
#define UI_CAPSULE_OFF      lv_color_hex(UI_C_CAPSULE_OFF)
#define UI_CAPSULE_ON       lv_color_hex(UI_C_CAPSULE_ON)
#define UI_CAPSULE_PWM      lv_color_hex(UI_C_CAPSULE_PWM)
#define UI_CAPSULE_GPIO     lv_color_hex(UI_C_CAPSULE_GPIO)
#define UI_CAPSULE_SCENE    lv_color_hex(UI_C_CAPSULE_SCENE)
#define UI_TILE_BADGE_ON    lv_color_hex(UI_C_TILE_BADGE_ON)
#define UI_TILE_BADGE_OFF   lv_color_hex(UI_C_TILE_BADGE_OFF)
#define UI_TILE_BADGE_OFFLINE lv_color_hex(UI_C_TILE_BADGE_OFFLINE)
#define UI_TAB_GLOW         lv_color_hex(UI_C_TAB_GLOW)

/* ---- geometry（8px 网格） ----------------------------------------------- */
#define UI_RADIUS           20
#define UI_RADIUS_MD        16
#define UI_RADIUS_SM        12
#define UI_RADIUS_XS        6
#define UI_PAD              12
#define UI_PAD_SM           8
#define UI_GAP              8
#define UI_GAP_LG           14
#define UI_EXT_CLICK        20
/* 首页磁贴动效（开/关色过渡、图标弹跳、pending 转圈） */
#define UI_TILE_TRANS_MS       120
#define UI_TILE_ICON_POP_MS    120
#define UI_TILE_SPIN_MS        900
#define UI_SETTINGS_TILE_H  72
#define UI_NAV_H            40
#define UI_NAV_BACK_W       64
#define UI_BTN_H            48
#define UI_BTN_H_SM         44
/* 小智对话悬浮胶囊：收窄居中，正文保留最近 5 句；半透明以免挡设置项 */
#define UI_AI_DRAWER_W         348
#define UI_AI_DRAWER_H         208 /* 含 48px 头栏（关闭/最小化可点） */
#define UI_AI_DRAWER_COMPACT_W 296 /* 唤醒建链条：贴文案宽度 */
#define UI_AI_DRAWER_COMPACT_H 72  /* 唤醒建链条：头栏按钮 44px */
#define UI_AI_DRAWER_TEXT_H    120
#define UI_AI_DRAWER_HDR_H     48  /* 头栏：对齐 44px 触控钮 */
#define UI_AI_DRAWER_BTN_SZ    44  /* 最小化/关闭：≥44 防穿透下层卡 */
#define UI_AI_DRAWER_BOTTOM    52
#define UI_AI_DRAWER_BG_OPA    LV_OPA_COVER /* 保证正文对比度，避免反复透明混合 */
#define UI_AI_MINI_SZ          52  /* 最小化圆钮（仅图标，无文字） */
#define UI_AI_MINI_RIGHT       14
/* 新闻阅读：0=14px 普惠；1/2=资源 20px 真字库（禁止 transform 放大点阵，会糊） */
#define UI_NEWS_ZOOM_MAX         2
#define UI_NEWS_LINE_SPACE_0     4
#define UI_NEWS_LINE_SPACE_1     6
#define UI_NEWS_LINE_SPACE_2     10
#define UI_NEWS_LETTER_SPACE_2   1
