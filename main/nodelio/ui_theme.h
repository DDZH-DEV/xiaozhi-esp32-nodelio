/*
 * ui_theme.h — NodeLio 墙板唯一 UI 令牌。
 * 「林夜控家」：略抬高的墨绿画布 + 开态亮色磁贴；关态统一中性石板。
 * 控件代码只读宏，禁止散落裸 hex。
 */
#pragma once

#include <lvgl.h>

/* ---- raw (uint32_t) ----------------------------------------------------- */
/* 画布：高级曜夜微晶深黑，告别暗绿沉闷，纯净深邃通透 */
#define UI_C_BG              0x0c0f14
#define UI_C_SURFACE         0x181e28
#define UI_C_SURFACE_PRESSED 0x222a38
#define UI_C_SURFACE_OFF     0x141822
#define UI_C_PANEL           0x141820
#define UI_C_BORDER          0x283242
#define UI_C_FIELD           0x181e28
#define UI_C_FIELD_FOCUS     0x222a38
#define UI_C_TEXT            0xf8fafc
#define UI_C_TEXT_SUB        0xcad5e2
#define UI_C_TEXT_DIM        0x94a3b8
#define UI_C_TEXT_MUTED      0x64748b
#define UI_C_TEXT_TIP        0x475569
#define UI_C_STATUS_CHROME   0x94a3b8
#define UI_C_ON_TILE         0xffffff
#define UI_C_TILE_TAG        0xe8fff0
#define UI_C_TILE_TAG_PWM    0xe8f6ff
#define UI_C_PWM_TRACK       0x183b54
#define UI_C_ACCENT          0x34c759
#define UI_C_ACCENT_SOFT     0x6ee08a
#define UI_C_WARN            0xffb340
#define UI_C_DANGER          0xff453a
#define UI_C_INFO            0x38bdf8
#define UI_C_PURPLE          0xc084fc
#define UI_C_DANGER_BG       0x401818
#define UI_C_DANGER_FG       0xfecaca
#define UI_C_BACK_BTN        0x1e3a2b
#define UI_C_OFFLINE         0x232932
#define UI_C_DISABLED_FG     0x64748b
#define UI_C_SLIDER_TRACK    0x1c2330
/* 开态：大厂经典环境微发光表面（Luminous Glass），清澈通透，质感高级 */
#define UI_C_TILE_SWITCH     0x0d3822
#define UI_C_TILE_SCENE      0x2a1447
#define UI_C_TILE_GPIO       0x3a250c
#define UI_C_TILE_PWM        0x0c3452
/* 关态：高级微晶深石板灰（Elevated Slate），极简纯粹，不再发绿 */
#define UI_C_TILE_OFF        0x161b24
#define UI_C_TILE_SWITCH_OFF UI_C_TILE_OFF
#define UI_C_TILE_SCENE_OFF  UI_C_TILE_OFF
#define UI_C_TILE_GPIO_OFF   UI_C_TILE_OFF
#define UI_C_TILE_PWM_OFF    UI_C_TILE_OFF
#define UI_C_TILE_ON_BORDER  0x10b981
#define UI_C_TILE_OFF_BORDER 0x232c3b
#define UI_C_TILE_PWM_BORDER 0x0ea5e9
#define UI_C_TILE_GPIO_BORDER 0xf59e0b
#define UI_C_CAPSULE_OFF     0x222a38
#define UI_C_CAPSULE_ON      0x10b981
#define UI_C_CAPSULE_PWM     0x0ea5e9
#define UI_C_CAPSULE_GPIO    0xf59e0b
#define UI_C_CAPSULE_SCENE   0x8b5cf6
#define UI_C_TILE_BADGE_ON   0x10b981
#define UI_C_TILE_BADGE_OFF  0x202733
#define UI_C_TILE_BADGE_OFFLINE 0x22262e
#define UI_C_TAB_GLOW        0xffffff

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
#define UI_TILE_SWITCH_OFF  lv_color_hex(UI_C_TILE_SWITCH_OFF)
#define UI_TILE_SCENE_OFF   lv_color_hex(UI_C_TILE_SCENE_OFF)
#define UI_TILE_GPIO_OFF    lv_color_hex(UI_C_TILE_GPIO_OFF)
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
#define UI_RADIUS           14
#define UI_RADIUS_MD        14
#define UI_RADIUS_SM        10
#define UI_RADIUS_XS        6
#define UI_PAD              12
#define UI_PAD_SM           8
#define UI_GAP              8
#define UI_GAP_LG           14
#define UI_EXT_CLICK        20
#define UI_SETTINGS_TILE_H  72
#define UI_NAV_H            40
#define UI_NAV_BACK_W       64
#define UI_BTN_H            48
#define UI_BTN_H_SM         44
/* 小智对话悬浮胶囊：收窄居中，正文保留最近 5 句 */
#define UI_AI_DRAWER_W         348
#define UI_AI_DRAWER_H         188
#define UI_AI_DRAWER_COMPACT_W 296 /* 唤醒建链条：贴文案宽度 */
#define UI_AI_DRAWER_COMPACT_H 56  /* 唤醒建链条：仅声波+标题 */
#define UI_AI_DRAWER_TEXT_H    120
#define UI_AI_DRAWER_BOTTOM    52
