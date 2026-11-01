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

static lv_obj_t* s_toast;
static lv_obj_t* s_toast_lbl;
static lv_timer_t* s_toast_tmr;

void nl_panel_shell_dismiss_toast(void) {
    if (s_toast_tmr) {
        lv_timer_del(s_toast_tmr);
        s_toast_tmr = nullptr;
    }
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = nullptr;
        s_toast_lbl = nullptr;
    }
}

void nl_panel_shell_notify(const char* msg, const lv_font_t* font, int bottom_inset,
                           int duration_ms) {
    if (!s_toast) {
        if (!msg || !msg[0]) {
            return;
        }
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_toast);
        lv_obj_set_width(s_toast, LV_SIZE_CONTENT);
        lv_obj_set_height(s_toast, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_toast, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_toast, 1, 0);
        lv_obj_set_style_border_color(s_toast, UI_ACCENT, 0);
        lv_obj_set_style_border_opa(s_toast, 160, 0);
        lv_obj_set_style_pad_hor(s_toast, 18, 0);
        lv_obj_set_style_pad_ver(s_toast, 10, 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
        s_toast_lbl = lv_label_create(s_toast);
        lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(s_toast_lbl, font, 0);
        lv_obj_set_style_text_color(s_toast_lbl, UI_TEXT, 0);
        lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    if (!msg || !msg[0]) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        if (s_toast_tmr) {
            lv_timer_del(s_toast_tmr);
            s_toast_tmr = nullptr;
        }
        return;
    }
    lv_label_set_text(s_toast_lbl, msg);
    /* 短句贴文字收窄；超长才在屏宽内换行 */
    lv_obj_set_style_text_font(s_toast_lbl, font, 0);
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
    lv_obj_set_width(s_toast_lbl, tw);
    lv_obj_set_width(s_toast, LV_SIZE_CONTENT);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_tmr) {
        lv_timer_del(s_toast_tmr);
        s_toast_tmr = nullptr;
    }
    s_toast_tmr = lv_timer_create(
        [](lv_timer_t* t) {
            (void)t;
            if (s_toast) {
                lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            }
            s_toast_tmr = nullptr;
        },
        duration_ms > 0 ? duration_ms : 1, nullptr);
    if (s_toast_tmr) {
        lv_timer_set_repeat_count(s_toast_tmr, 1);
    }
}
