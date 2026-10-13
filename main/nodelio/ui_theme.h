/*
 * ui_theme.h — NodeLio 墙板唯一 UI 令牌。
 * 深绿 LCD 表面自底向上分层；品牌绿作唯一强调色。
 * 控件代码只读宏，禁止散落裸 hex。
 */
#pragma once

#include <lvgl.h>

/* ---- raw (uint32_t / 日志) ---------------------------------------------- */
#define UI_C_BG              0x1e2622
#define UI_C_SURFACE         0x2a342e
#define UI_C_SURFACE_PRESSED 0x344840
#define UI_C_SURFACE_OFF     0x1a221e
#define UI_C_PANEL           0x252d28
#define UI_C_BORDER          0x3a4a40
#define UI_C_FIELD           0x2a332e
#define UI_C_FIELD_FOCUS     0x33443a
#define UI_C_TEXT            0xe8f0ea
#define UI_C_TEXT_SUB        0xb8c8bc
#define UI_C_TEXT_DIM        0x8a9b8e
#define UI_C_TEXT_MUTED      0x6a7d70
#define UI_C_TEXT_TIP        0x4a5a50
#define UI_C_STATUS_CHROME   0x7a8c80
#define UI_C_ON_TILE         0xffffff
#define UI_C_TILE_TAG        0xe8fff0
#define UI_C_TILE_TAG_PWM    0xe8f6ff
#define UI_C_PWM_TRACK       0x163848
#define UI_C_ACCENT          0x34c759
#define UI_C_ACCENT_SOFT     0x5cdb7a
#define UI_C_WARN            0xff9f0a
#define UI_C_DANGER          0xff453a
#define UI_C_INFO            0x5ac8fa
#define UI_C_PURPLE          0xbf5af2
#define UI_C_DANGER_BG       0x4a3030
#define UI_C_DANGER_FG       0xe8c8c8
#define UI_C_BACK_BTN        0x2a4a34
#define UI_C_OFFLINE         0x3a4240
#define UI_C_DISABLED_FG     0x888888
#define UI_C_CHAT_BAND       0x1a221e
#define UI_C_SLIDER_TRACK    0x1a221e
#define UI_C_TILE_SWITCH     0x2f8f4e
#define UI_C_TILE_SCENE      0x1f5c38
#define UI_C_TILE_GPIO       0xa66b1a
#define UI_C_TILE_PWM        0x2a6d8f
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
#define UI_CHAT_BAND        lv_color_hex(UI_C_CHAT_BAND)
#define UI_SLIDER_TRACK     lv_color_hex(UI_C_SLIDER_TRACK)
#define UI_TAB_GLOW         lv_color_hex(UI_C_TAB_GLOW)

/* ---- geometry（8px 网格） ----------------------------------------------- */
#define UI_RADIUS           12
#define UI_RADIUS_SM        8
#define UI_RADIUS_XS        4
#define UI_PAD              12
#define UI_PAD_SM           8
#define UI_GAP              8
#define UI_GAP_LG           14
#define UI_EXT_CLICK        20
#define UI_SETTINGS_TILE_H  112
#define UI_NAV_BACK_W       64
#define UI_BTN_H            48
#define UI_BTN_H_SM         44
