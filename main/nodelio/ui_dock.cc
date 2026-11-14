/*
 * ui_dock.cc — 段4 左侧悬浮白色圆角 Dock（仅 P4 62 / 508 启用）。
 *
 * 5 个纵向入口（由上至下）：
 *   0 首页（NL_TAB_HOME）：青绿渐变底座 + HOME 图标
 *   1 应用（NL_TAB_APPS）：紫粉渐变底座 + EXPLORE 图标
 *   2 小智（语音对话）：拟物银白扬声器圆盘 + MIC 图标 → nodelio_overlay_show_chat()
 *   3 音乐（曲库/播放）：橙粉渐变 + MUSIC_NOTE → 恢复曲库或打开 SD 音乐
 *   4 设置（NL_TAB_SET）：翠绿渐变底座 + SETTINGS 图标
 *
 * 兼容性策略：
 *   - 62（nav_width=96）：不动 nav_width；沉浸模式下将原 s_tabbar 设为全透明占位槽，
 *     并在 dock_rect.x/w 处按内容高度居中悬浮。
 *   - 508（nav_width=0）：沉浸模式下隐藏底部 Tab；Dock 随触摸显示、空闲自动收起，
 *     显示时对 s_body 设 pad_left 避让，隐藏时收回。
 */
#include "ui_dock.h"

#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_SILA_P4C5_62 || CONFIG_BOARD_TYPE_SILA_P4C5_508

#include "board_info.h"
#include "nodelio_overlay.h"
#include "overlay_priv.h"
#include "skill_result_ui.h"
#include "ui_theme.h"

#include <material_symbols.h>

#include <algorithm>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

namespace {

constexpr int kDockSlots = 5;
constexpr int kSlotHome = 0;
constexpr int kSlotApps = 1;
constexpr int kSlotVoice = 2;
constexpr int kSlotMusic = 3;
constexpr int kSlotSettings = 4;
constexpr int kSquareLeftInset = 84; /* 508: dock_x(12) + dock_w(68) + gap(4) */
constexpr int kDockRadius = 16; /* 勿用 CIRCLE，上下勿做成大胶囊半圆 */
constexpr int kDockPadVer = 8;
constexpr int kDockPadRow = 6;

lv_obj_t *s_dock = nullptr;
lv_obj_t *s_tiles[kDockSlots] = {nullptr};
lv_obj_t *s_dots[kDockSlots] = {nullptr};

lv_obj_t *make_grad_badge(lv_obj_t *parent, int sz, uint32_t c_top, uint32_t c_bot,
                          const char *symbol)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, sz, sz);
    lv_obj_set_style_radius(b, 11, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(c_top), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(c_bot), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_center(b);
    lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t *ic = lv_label_create(b);
    lv_obj_set_style_text_font(ic, sz >= 40 ? &font_material_symbols_30_4 : &font_material_symbols_20_4,
                               0);
    lv_obj_set_style_text_color(ic, lv_color_white(), 0);
    lv_label_set_text(ic, symbol);
    lv_obj_center(ic);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* 第 3 颗：拟物银白扬声器圆盘 */
void build_speaker_disc(lv_obj_t *tile, int badge_sz)
{
    lv_obj_t *outer = lv_obj_create(tile);
    lv_obj_remove_style_all(outer);
    lv_obj_set_size(outer, badge_sz, badge_sz);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_bg_grad_color(outer, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_bg_grad_dir(outer, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(outer, 1, 0);
    lv_obj_set_style_border_color(outer, lv_color_hex(0x94a3b8), 0);
    lv_obj_center(outer);
    lv_obj_remove_flag(outer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    lv_obj_t *ring = lv_obj_create(outer);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, badge_sz - 8, badge_sz - 8);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_40, 0);
    lv_obj_center(ring);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *ic = lv_label_create(outer);
    lv_obj_set_style_text_font(
        ic, badge_sz >= 40 ? &font_material_symbols_30_4 : &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x334155), 0);
    lv_label_set_text(ic, MATERIAL_SYMBOLS_MIC);
    lv_obj_center(ic);
    lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
}

void on_dock_slot_clicked(lv_event_t *e)
{
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);
    switch (slot) {
        case kSlotHome:
            nl_ov_show_page(NL_TAB_HOME);
            break;
        case kSlotApps:
            nl_ov_show_page(NL_TAB_APPS);
            break;
        case kSlotVoice:
            nodelio_overlay_show_chat();
            break;
        case kSlotMusic:
            if (!nodelio_skill_try_reopen_music_list()) {
                nodelio_skill_request_sd_library(false);
            }
            break;
        case kSlotSettings:
            nl_ov_show_page(NL_TAB_SET);
            break;
        default:
            break;
    }
}

void set_tabbar_children_hidden(lv_obj_t *tabbar, bool hidden)
{
    if (!tabbar) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(tabbar);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(tabbar, (int32_t)i);
        if (hidden) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(c, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

}  // namespace

extern "C" void nl_dock_init(lv_obj_t *root)
{
    if (!root) {
        return;
    }
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    if (!prof || prof->dock_rect.w == 0 || prof->dock_rect.h == 0) {
        return;
    }
    /* 图标尽量贴满 Dock 宽度，避免白边里一圈小方块 */
    const int tile_sz = std::max(52, (int)prof->dock_rect.w - 10);
    const int badge_sz = tile_sz - 8;
    const int dock_w = std::max((int)prof->dock_rect.w, tile_sz + 10);
    const int dock_h =
        kDockPadVer * 2 + tile_sz * kDockSlots + kDockPadRow * (kDockSlots - 1);
    const int dock_y = std::max(8, ((int)prof->height - dock_h) / 2);

    s_dock = lv_obj_create(root);
    lv_obj_remove_style_all(s_dock);
    lv_obj_add_flag(s_dock, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_IGNORE_LAYOUT |
                                            LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_dock, prof->dock_rect.x, dock_y);
    lv_obj_set_size(s_dock, dock_w, dock_h);
    lv_obj_set_style_radius(s_dock, kDockRadius, 0);
    lv_obj_set_style_bg_color(s_dock, UI_DOCK_BG, 0);
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dock, 1, 0);
    lv_obj_set_style_border_color(s_dock, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_shadow_width(s_dock, 14, 0);
    lv_obj_set_style_shadow_offset_y(s_dock, 2, 0);
    lv_obj_set_style_shadow_color(s_dock, UI_DOCK_SHADOW, 0);
    lv_obj_set_style_shadow_opa(s_dock, LV_OPA_30, 0);
    lv_obj_set_style_pad_ver(s_dock, kDockPadVer, 0);
    lv_obj_set_style_pad_row(s_dock, kDockPadRow, 0);
    lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < kDockSlots; ++i) {
        lv_obj_t *t = lv_obj_create(s_dock);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, tile_sz, tile_sz);
        lv_obj_set_style_radius(t, 16, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0xf8fafc), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0xe2e8f0), LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(t, 1, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_shadow_width(t, 6, 0);
        lv_obj_set_style_shadow_offset_y(t, 2, 0);
        lv_obj_set_style_shadow_color(t, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(t, LV_OPA_10, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(t, 6);
        lv_obj_add_event_cb(t, on_dock_slot_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        if (i == kSlotHome) {
            make_grad_badge(t, badge_sz, 0x38bdf8, 0x0284c7, MATERIAL_SYMBOLS_HOME);
        } else if (i == kSlotApps) {
            make_grad_badge(t, badge_sz, 0x818cf8, 0xc084fc, MATERIAL_SYMBOLS_EXPLORE);
        } else if (i == kSlotVoice) {
            build_speaker_disc(t, badge_sz);
        } else if (i == kSlotMusic) {
            make_grad_badge(t, badge_sz, 0xf97316, 0xdb2777, MATERIAL_SYMBOLS_MUSIC_NOTE);
        } else {
            make_grad_badge(t, badge_sz, 0x34c759, 0x0d9488, MATERIAL_SYMBOLS_SETTINGS);
        }

        /* 选中态左侧翠绿指示胶条 */
        lv_obj_t *dot = lv_obj_create(t);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 18);
        lv_obj_set_style_radius(dot, 2, 0);
        lv_obj_set_style_bg_color(dot, UI_ACCENT, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 1, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        s_tiles[i] = t;
        s_dots[i] = dot;
    }
}

extern "C" void nl_dock_sync(lv_obj_t *tabbar, lv_obj_t *body, int active_tab, bool shell_visible)
{
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    const bool imm = ui_palette_is_immersive() != 0;
    const bool rail = prof && prof->nav_width != 0;

    if (!imm) {
        if (s_dock) {
            lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        }
        if (tabbar) {
            lv_obj_set_style_bg_opa(tabbar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(tabbar, rail ? 0 : 1, 0);
            set_tabbar_children_hidden(tabbar, false);
        }
        if (body && !rail) {
            lv_obj_set_style_pad_left(body, 0, 0);
        }
        return;
    }

    /* 沉浸模式：左侧圆角 Dock。
     * 纯首页可空闲收起；应用/设置、或首页仍挂技能/文件卡时由调用方保持 shell_visible=true
     *（62 的 96px 槽位不随 Dock 伸缩，有应用卡时收起只会空出一条轨）。 */
    if (s_dock) {
        if (shell_visible) {
            lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_dock);
        } else {
            lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const int active_slot =
        active_tab == NL_TAB_APPS ? kSlotApps : (active_tab == NL_TAB_SET ? kSlotSettings : kSlotHome);
    for (int i = 0; i < kDockSlots; ++i) {
        const bool on = (i == active_slot);
        if (s_dots[i]) {
            if (on) {
                lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_tiles[i]) {
            lv_obj_set_style_border_color(
                s_tiles[i], on ? UI_ACCENT : lv_color_hex(0xe2e8f0), 0);
            lv_obj_set_style_border_width(s_tiles[i], on ? 2 : 1, 0);
        }
    }

    if (tabbar) {
        if (rail) {
            /* 62：保持 96px 布局槽位不缩水，隐藏旧按钮与底色 */
            lv_obj_set_style_bg_opa(tabbar, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(tabbar, 0, 0);
            set_tabbar_children_hidden(tabbar, true);
        } else {
            /* 508：隐藏底部悬浮 Tab */
            lv_obj_add_flag(tabbar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (body && !rail) {
        /* 仅 Dock 可见时避让；收起后收回左边距避免空条 */
        lv_obj_set_style_pad_left(body, shell_visible ? kSquareLeftInset : 0, 0);
    }
}

extern "C" int nl_dock_left_inset(void)
{
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    if (!prof) {
        return 0;
    }
    if (prof->nav_width != 0) {
        return (int)prof->nav_width;
    }
    return ui_palette_is_immersive() ? kSquareLeftInset : 0;
}

#else

extern "C" void nl_dock_init(lv_obj_t *root)
{
    (void)root;
}

extern "C" void nl_dock_sync(lv_obj_t *tabbar, lv_obj_t *body, int active_tab, bool shell_visible)
{
    (void)tabbar;
    (void)body;
    (void)active_tab;
    (void)shell_visible;
}

extern "C" int nl_dock_left_inset(void)
{
    const nodelio_display_profile_t *prof = nodelio_display_profile();
    return prof ? (int)prof->nav_width : 0;
}

#endif

