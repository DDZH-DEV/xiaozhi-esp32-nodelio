#include "ui_widgets.h"
#include <cmath>
#include "board_info.h"
#include "nvs_config.h"

void nl_ov_set_font(lv_obj_t *obj);
int nl_ov_ui_line_box(void);
int nl_ov_ui_font_line_h(void);
int nl_ov_wrapped_text_h(const char *str, int max_w);

static lv_style_t st_btn_base;
static lv_style_t st_btn_pressed;
static lv_style_t st_btn_off;
static lv_style_t st_btn_on;
static lv_style_t st_tab_base;
static lv_style_t st_tab_checked;
static bool s_inited;

lv_color_t ui_color_on(lv_color_t background) {
    auto linear = [](uint8_t channel) {
        const float value = channel / 255.0f;
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    const float luminance = 0.2126f * linear(background.red) + 0.7152f * linear(background.green) +
                            0.0722f * linear(background.blue);
    return luminance > 0.179f ? UI_CONTRAST_DARK : UI_ON_TILE;
}

static void apply_font(lv_obj_t *obj)
{
    nl_ov_set_font(obj);
}

static void styles_bind_colors(void)
{
    if (auto* display = lv_display_get_default()) {
        lv_display_set_theme(display, lv_theme_default_init(display, UI_ACCENT, UI_SUCCESS,
                                                            ui_palette_is_dark() != 0, LV_FONT_DEFAULT));
    }
    lv_style_set_bg_color(&st_btn_base, UI_SURFACE);
    lv_style_set_bg_color(&st_btn_pressed, UI_SURFACE_PRESSED);
    lv_style_set_bg_color(&st_btn_off, UI_SURFACE_OFF);
    lv_style_set_border_color(&st_btn_on, UI_ACCENT);
    lv_style_set_text_color(&st_tab_base, UI_TEXT_MUTED);
    lv_style_set_bg_color(&st_tab_checked, UI_TAB_GLOW);
    lv_style_set_text_color(&st_tab_checked, UI_ACCENT);
    lv_obj_report_style_change(&st_btn_base);
    lv_obj_report_style_change(&st_btn_pressed);
    lv_obj_report_style_change(&st_btn_off);
    lv_obj_report_style_change(&st_btn_on);
    lv_obj_report_style_change(&st_tab_base);
    lv_obj_report_style_change(&st_tab_checked);
}

void ui_styles_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    lv_style_init(&st_btn_base);
    lv_style_set_bg_opa(&st_btn_base, LV_OPA_COVER);
    lv_style_set_radius(&st_btn_base, UI_RADIUS_SM);
    lv_style_set_border_width(&st_btn_base, 0);
    lv_style_set_shadow_width(&st_btn_base, 0);
    lv_style_set_pad_all(&st_btn_base, UI_PAD);
    lv_style_set_pad_row(&st_btn_base, 4);

    lv_style_init(&st_btn_pressed);
#if !CONFIG_IDF_TARGET_ESP32S3
    lv_style_set_translate_y(&st_btn_pressed, 1);
#endif

    lv_style_init(&st_btn_off);
    lv_style_set_opa(&st_btn_off, LV_OPA_50);

    lv_style_init(&st_btn_on);
    lv_style_set_border_width(&st_btn_on, 1);

    lv_style_init(&st_tab_base);
    lv_style_set_bg_opa(&st_tab_base, LV_OPA_TRANSP);
    lv_style_set_radius(&st_tab_base, 13);
    lv_style_set_border_width(&st_tab_base, 0);
    lv_style_set_shadow_width(&st_tab_base, 0);
    lv_style_set_pad_ver(&st_tab_base, 4);

    lv_style_init(&st_tab_checked);
    lv_style_set_bg_opa(&st_tab_checked, LV_OPA_COVER);
    styles_bind_colors();
}

void ui_styles_apply(void)
{
    if (!s_inited) {
        ui_styles_init();
        return;
    }
    styles_bind_colors();
}

void ui_style_card(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_BOARD_TYPE_SILA_P4C5_62
    /* S3 无 PPA；P4 还要做 90° 旋转。卡片高斯阴影会让滚动每帧重算 Alpha。 */
    lv_obj_set_style_shadow_width(obj, 0, 0);
    return;
#else
    if (ui_palette_is_dark()) {
        lv_obj_set_style_shadow_width(obj, 0, 0);
        return;
    }
    /* 模糊半径 6、下移 2，落在 8px 卡片间距里，避免大阴影缓存。 */
    lv_obj_set_style_shadow_color(obj, lv_color_hex(UI_C_SHADOW), 0);
    lv_obj_set_style_shadow_width(obj, 6, 0);
    lv_obj_set_style_shadow_spread(obj, 0, 0);
    lv_obj_set_style_shadow_ofs_x(obj, 0, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 2, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_30, 0);
#endif
}

int ui_settings_tile_width(int avail, bool force_single_col, int *columns_out)
{
    const int gap = (nodelio_text_scale() == 1) ? 12 : UI_GAP;
    const auto *profile = nodelio_display_profile();
    if (avail < 160) {
        avail = (int)profile->width - (int)profile->nav_width - 16 * 2;
    }
    if (force_single_col) {
        int tile_w = (int)profile->width / 5;
        if (tile_w < 160) {
            tile_w = 160;
        }
        if (tile_w > avail) {
            tile_w = avail;
        }
        if (columns_out) {
            *columns_out = 1;
        }
        return tile_w;
    }
    const int max_cols = profile->nav_width ? 4 : 2;
    /* 方屏（底栏）固定两列：480 宽扣边距后每列约 218~220，大字档 gap=12 时会跌破 220 退成单列。 */
    const int min_tile = profile->nav_width ? 220 : 160;
    int columns = max_cols;
    while (columns > 1) {
        const int tw = (avail - gap * (columns - 1)) / columns;
        if (tw >= min_tile) {
            break;
        }
        columns--;
    }
    if (columns_out) {
        *columns_out = columns;
    }
    return (avail - gap * (columns - 1)) / columns;
}

int ui_settings_tile_height(void)
{
    const int line = nl_ov_ui_line_box();
    const bool large = nodelio_text_scale() == 1;
    int tile_h = (large ? 14 : 10) + line + (large ? 6 : 4) + line * 2 + (large ? 14 : 10);
    if (tile_h < (large ? 80 : UI_SETTINGS_TILE_H)) {
        tile_h = (large ? 80 : UI_SETTINGS_TILE_H);
    }
    return tile_h;
}

lv_obj_t *ui_make_settings_tile(lv_obj_t *parent, const char *glyph, const char *title,
                                const char *sub, uint32_t accent_c, const char *right_icon,
                                lv_event_cb_t cb, bool force_single_col)
{
    ui_styles_init();

    /* 浏览态按容器宽度分列。技能卡打开时左卡固定为整屏 1/5，不把剩余区域拉满。 */
    int avail = parent ? (int)lv_obj_get_content_width(parent) : 0;
    const int tile_w = ui_settings_tile_width(avail, force_single_col, nullptr);

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    /* 标题 1 行 + 副文 2 行。大字档行框变高，卡片跟着长，避免副文被底边裁掉。 */
    const int tile_h = ui_settings_tile_height();
    lv_obj_set_size(btn, tile_w, tile_h);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_color(btn, UI_TILE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_MD, 0);
    ui_style_card(btn);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, UI_TILE_OFF_BORDER, 0);
    const bool large = nodelio_text_scale() == 1;
    lv_obj_set_style_border_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_pad_left(btn, large ? 12 : 10, 0);
    lv_obj_set_style_pad_right(btn, large ? 10 : 8, 0);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    const int pod_sz = 46;
    const int col_gap = large ? 14 : 12;
    lv_obj_set_style_pad_column(btn, col_gap, 0);

    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent_c), LV_STATE_PRESSED);
#if !CONFIG_IDF_TARGET_ESP32S3
    lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
#endif

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *pod = lv_obj_create(btn);
    lv_obj_remove_style_all(pod);
    lv_obj_set_size(pod, pod_sz, pod_sz);
    lv_obj_set_style_radius(pod, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(pod, lv_color_hex(accent_c), 0);
    lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pod, 0, 0);
    lv_obj_set_style_border_color(pod, lv_color_hex(accent_c), 0);
    lv_obj_set_style_border_opa(pod, 90, 0);
    lv_obj_clear_flag(pod, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pod, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *g = lv_label_create(pod);
    lv_label_set_text(g, glyph ? glyph : "");
    apply_font(g);
    lv_obj_set_style_text_color(g, UI_ON_TILE, 0);
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

    const int font_h = nl_ov_ui_font_line_h();
    /* 副文可用宽 = 卡宽 - 左右内边距 - 列间距 - 图标底座（有右图标再让出位置）。
     * 方屏双列 + 大字档只有 ~140px，一行装不下「重新拉取首页控件」，须折两行。 */
    int sub_w = tile_w - 10 - 8 - col_gap - pod_sz;
    if (right_icon && right_icon[0]) {
        sub_w -= font_h + 8;
    }
    if (sub_w < 48) {
        sub_w = 48;
    }

    lv_obj_t *t = lv_label_create(text);
    lv_label_set_text(t, title ? title : "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_height(t, font_h);
    apply_font(t);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(t, UI_TEXT, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *s = lv_label_create(text);
    lv_label_set_text(s, sub ? sub : "");
    /* 副文最多两行：折行 + 溢出打点。卡高由 ui_settings_tile_height() 按两行留，
     * 单行文案只占一行高，整组仍在卡内垂直居中。 */
    lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s, lv_pct(100));
    const int sub_cap = font_h * 2 + (nl_ov_ui_line_box() - font_h);
    lv_obj_set_height(s, sub_cap);
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
    lv_obj_set_ext_click_area(b, 4);
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
    /* 宽屏内容区紧贴左轨。返回再留 8px 会比顶栏左缘缩进去一截。 */
    const int pad_left = nodelio_display_profile()->nav_width ? 0 : UI_PAD_SM;
    lv_obj_set_style_pad_left(top, pad_left, 0);
    lv_obj_set_style_pad_right(top, UI_PAD_SM, 0);
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
    /* 方角：设置详情/连接页的返回是文字胶囊，不再画成圆球。 */
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
    lv_obj_set_style_text_color(bl, UI_ACCENT, 0);
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
    lv_color_t fg = UI_ON_TILE;
    if (kind == 1) {
        bg = UI_DANGER_BG;
        fg = UI_DANGER_FG;
    } else if (kind == 2) {
        bg = UI_SURFACE;
        fg = UI_TEXT;
    }
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_color(btn, kind == 0 ? UI_ACCENT_PRESSED : UI_SURFACE_PRESSED,
                              LV_STATE_PRESSED);
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
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
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
    lv_obj_set_style_text_color(lb, UI_ACCENT, 0);
    lv_obj_center(lb);
    return btn;
}

void ui_style_slider(lv_obj_t *sl)
{
    if (!sl) {
        return;
    }
    lv_obj_set_width(sl, lv_pct(100));
    lv_obj_set_height(sl, 6);
    lv_obj_set_ext_click_area(sl, 21);
    lv_obj_set_style_margin_top(sl, 8, 0);
    lv_obj_set_style_margin_bottom(sl, 8, 0);
    lv_obj_set_style_pad_all(sl, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, UI_SLIDER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, UI_ON_TILE, LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(sl, UI_BORDER, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
}

void ui_style_switch(lv_obj_t* sw) {
    lv_obj_set_size(sw, 52, 32);
    lv_obj_set_ext_click_area(sw, 8);
    lv_obj_set_style_bg_color(sw, UI_SLIDER_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_SUCCESS,
                              static_cast<lv_style_selector_t>(LV_PART_INDICATOR) |
                                  static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_set_style_bg_color(sw, UI_ON_TILE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
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
