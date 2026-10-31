#include "ui_widgets.h"
#include "board_info.h"

#include "fonts/font_puhui_14_1.h"

static lv_style_t st_btn_base;
static lv_style_t st_btn_pressed;
static lv_style_t st_btn_off;
static lv_style_t st_btn_on;
static lv_style_t st_tab_base;
static lv_style_t st_tab_checked;
static bool s_inited;

static const lv_font_t *ui_font(void)
{
    return &font_puhui_14_1;
}

static void apply_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, ui_font(), 0);
    }
}

void ui_styles_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    lv_style_init(&st_btn_base);
    lv_style_set_bg_color(&st_btn_base, UI_SURFACE);
    lv_style_set_bg_opa(&st_btn_base, LV_OPA_COVER);
    lv_style_set_radius(&st_btn_base, UI_RADIUS_SM);
    lv_style_set_border_width(&st_btn_base, 0);
    lv_style_set_shadow_width(&st_btn_base, 0);
    lv_style_set_pad_all(&st_btn_base, UI_PAD);
    lv_style_set_pad_row(&st_btn_base, 4);

    lv_style_init(&st_btn_pressed);
    lv_style_set_bg_color(&st_btn_pressed, UI_SURFACE_PRESSED);
    lv_style_set_translate_y(&st_btn_pressed, 1);

    lv_style_init(&st_btn_off);
    lv_style_set_bg_color(&st_btn_off, UI_SURFACE_OFF);
    lv_style_set_opa(&st_btn_off, LV_OPA_50);

    lv_style_init(&st_btn_on);
    lv_style_set_border_width(&st_btn_on, 1);
    lv_style_set_border_color(&st_btn_on, UI_ACCENT);

    lv_style_init(&st_tab_base);
    lv_style_set_bg_opa(&st_tab_base, LV_OPA_TRANSP);
    lv_style_set_radius(&st_tab_base, UI_RADIUS_SM);
    lv_style_set_border_width(&st_tab_base, 0);
    lv_style_set_shadow_width(&st_tab_base, 0);
    lv_style_set_pad_ver(&st_tab_base, 6);
    lv_style_set_text_color(&st_tab_base, UI_TEXT_MUTED);

    lv_style_init(&st_tab_checked);
    lv_style_set_bg_opa(&st_tab_checked, 18);
    lv_style_set_bg_color(&st_tab_checked, UI_TAB_GLOW);
    lv_style_set_text_color(&st_tab_checked, UI_TEXT_SUB);
}

lv_obj_t *ui_make_settings_tile(lv_obj_t *parent, const char *glyph, const char *title,
                                const char *sub, uint32_t accent_c, const char *right_icon,
                                lv_event_cb_t cb)
{
    ui_styles_init();

    /* 一行两列半宽；卡内左右：Icon Pod | 标题/副文 */
    const int pad_x = 16;
    const int gap = UI_GAP;
    const auto* profile = nodelio_display_profile();
    const int columns = profile->nav_width ? 4 : 2;
    const int tile_w =
        (profile->width - profile->nav_width - pad_x * 2 - gap * (columns - 1)) / columns;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, tile_w, UI_SETTINGS_TILE_H);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(btn, UI_TILE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 8, 0);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, 8, 0);

    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent_c), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *pod = lv_obj_create(btn);
    lv_obj_remove_style_all(pod);
    lv_obj_set_size(pod, 36, 36);
    lv_obj_set_style_radius(pod, 10, 0);
    lv_obj_set_style_bg_color(pod, lv_color_hex(accent_c), 0);
    lv_obj_set_style_bg_opa(pod, LV_OPA_20, 0);
    lv_obj_set_style_border_width(pod, 1, 0);
    lv_obj_set_style_border_color(pod, lv_color_hex(accent_c), 0);
    lv_obj_set_style_border_opa(pod, 90, 0);
    lv_obj_clear_flag(pod, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pod, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *g = lv_label_create(pod);
    lv_label_set_text(g, glyph ? glyph : "");
    apply_font(g);
    lv_obj_set_style_text_color(g, lv_color_hex(accent_c), 0);
    lv_obj_center(g);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *text = lv_obj_create(btn);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, 2, 0);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(text, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(text);
    lv_label_set_text(t, title ? title : "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    apply_font(t);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(t, UI_TEXT, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *s = lv_label_create(text);
    lv_label_set_text(s, sub ? sub : "");
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, lv_pct(100));
    apply_font(s);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s, UI_TEXT_MUTED, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);

    if (right_icon && right_icon[0]) {
        lv_obj_t *ric = lv_label_create(btn);
        lv_label_set_text(ric, right_icon);
        lv_obj_set_style_text_color(ric, UI_TEXT_MUTED, 0);
        lv_obj_clear_flag(ric, LV_OBJ_FLAG_CLICKABLE);
    }

    return btn;
}

lv_obj_t *ui_make_tab(lv_obj_t *parent, const char *txt)
{
    ui_styles_init();
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, lv_pct(100));
    lv_obj_set_ext_click_area(b, UI_EXT_CLICK);
    lv_obj_add_style(b, &st_tab_base, 0);
    lv_obj_add_style(b, &st_tab_checked, LV_STATE_CHECKED);
    lv_obj_add_style(b, &st_btn_pressed, LV_STATE_PRESSED);
    apply_font(b);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt ? txt : "");
    apply_font(l);
    lv_obj_center(l);
    return b;
}

void ui_tab_set_checked(lv_obj_t *btn, bool checked)
{
    if (!btn) {
        return;
    }
    if (checked) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
    }
}

lv_obj_t *ui_make_nav_bar(lv_obj_t *parent, int h, const char *title, lv_event_cb_t on_back)
{
    ui_styles_init();
    lv_obj_t *top = lv_obj_create(parent);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, h);
    lv_obj_set_style_bg_color(top, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(top, UI_PAD_SM, 0);
    lv_obj_set_style_pad_top(top, 3, 0);
    lv_obj_set_style_pad_bottom(top, 1, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(top);
    lv_obj_set_size(back, UI_NAV_BACK_W, h > 6 ? h - 6 : h);
    lv_obj_set_ext_click_area(back, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(back, UI_BACK_BTN, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_style(back, &st_btn_pressed, LV_STATE_PRESSED);
    if (on_back) {
        lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    apply_font(bl);
    lv_obj_set_style_text_color(bl, UI_TEXT, 0);
    lv_obj_center(bl);

    lv_obj_t *tl = lv_label_create(top);
    lv_label_set_text(tl, title ? title : "");
    apply_font(tl);
    lv_obj_set_style_text_color(tl, UI_TEXT, 0);
    lv_obj_set_style_pad_left(tl, 10, 0);
    return top;
}

lv_obj_t *ui_make_text_btn(lv_obj_t *parent, const char *label, int kind, lv_event_cb_t cb)
{
    ui_styles_init();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, UI_BTN_H);
    lv_obj_set_ext_click_area(btn, UI_EXT_CLICK);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &st_btn_off, LV_STATE_DISABLED);

    lv_color_t bg = UI_ACCENT;
    lv_color_t fg = UI_TEXT;
    if (kind == 1) {
        bg = UI_DANGER_BG;
        fg = UI_DANGER_FG;
    } else if (kind == 2) {
        bg = UI_SURFACE;
        fg = UI_TEXT;
    }
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, label ? label : "");
    apply_font(lb);
    lv_obj_set_style_text_color(lb, fg, 0);
    lv_obj_center(lb);
    return btn;
}

lv_obj_t *ui_make_action_btn(lv_obj_t *parent, const char *label, int w, int h, lv_event_cb_t cb)
{
    ui_styles_init();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_ext_click_area(btn, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(btn, UI_BACK_BTN, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(btn, &st_btn_off, LV_STATE_DISABLED);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *lb = lv_label_create(btn);
    lv_label_set_text(lb, label ? label : "");
    apply_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    lv_obj_center(lb);
    return btn;
}

void ui_style_slider(lv_obj_t *sl)
{
    if (!sl) {
        return;
    }
    lv_obj_set_width(sl, lv_pct(100));
    lv_obj_set_height(sl, 28);
    lv_obj_set_style_pad_hor(sl, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(sl, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_SLIDER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_TEXT, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    /* pad 控制旋钮直径；8 过大，4 ≈ 轨道高度的精致圆点 */
    lv_obj_set_style_pad_all(sl, 4, LV_PART_KNOB);
}

void ui_set_enabled(lv_obj_t *obj, bool enabled)
{
    if (!obj) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(obj, LV_OPA_60, 0);
    }
}
