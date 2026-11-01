#include "panel_shell.h"
#include <algorithm>
#include "ui_theme.h"

void nl_panel_shell_setup(const lv_font_t* font) {
    auto* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, UI_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, UI_TEXT, 0);
    lv_obj_set_style_text_font(screen, font, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

void nl_panel_shell_notify(const char* msg, const lv_font_t* font, int bottom_inset,
                           int duration_ms) {
    static lv_obj_t* toast;
    static lv_obj_t* toast_lbl;
    static lv_timer_t* hide_tmr;
    if (!toast) {
        if (!msg || !msg[0]) {
            return;
        }
        toast = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(toast);
        lv_obj_set_width(toast, LV_SIZE_CONTENT);
        lv_obj_set_height(toast, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(toast, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(toast, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(toast, 1, 0);
        lv_obj_set_style_border_color(toast, UI_ACCENT, 0);
        lv_obj_set_style_border_opa(toast, 160, 0);
        lv_obj_set_style_pad_hor(toast, 18, 0);
        lv_obj_set_style_pad_ver(toast, 10, 0);
        lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
        toast_lbl = lv_label_create(toast);
        lv_label_set_long_mode(toast_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(toast_lbl, font, 0);
        lv_obj_set_style_text_color(toast_lbl, UI_TEXT, 0);
        lv_obj_set_style_text_align(toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (!msg || !msg[0]) {
        lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
        if (hide_tmr) {
            lv_timer_del(hide_tmr);
            hide_tmr = nullptr;
        }
        return;
    }
    lv_label_set_text(toast_lbl, msg);
    /* 短句贴文字收窄；超长才在屏宽内换行 */
    lv_obj_set_style_text_font(toast_lbl, font, 0);
    const int32_t max_inner = std::max<int32_t>(
        1, lv_display_get_horizontal_resolution(lv_display_get_default()) - 120);
    lv_point_t sz;
    lv_text_get_size(&sz, msg, font, 0, 0, max_inner, LV_TEXT_FLAG_NONE);
    int32_t tw = sz.x + 2;
    if (tw < 1) {
        tw = 1;
    }
    if (tw > max_inner) {
        tw = max_inner;
    }
    lv_obj_set_width(toast_lbl, tw);
    lv_obj_set_width(toast, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    if (hide_tmr) {
        lv_timer_del(hide_tmr);
        hide_tmr = nullptr;
    }
    hide_tmr = lv_timer_create(
        [](lv_timer_t* t) {
            (void)t;
            if (toast) {
                lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
            }
            hide_tmr = nullptr;
        },
        duration_ms > 0 ? duration_ms : 1, nullptr);
    if (hide_tmr) {
        lv_timer_set_repeat_count(hide_tmr, 1);
    }
}
