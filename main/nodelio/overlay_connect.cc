#include "overlay_priv.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#include "audio_codec.h"
#include "backlight.h"

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#define TAG "nl_conn"

static lv_obj_t *s_hist_row;
static lv_obj_t *s_hist_hint;
static lv_obj_t *s_ta_screenshot = nullptr;

static void on_wifi_scan_btn(lv_event_t *e);
static void rebuild_server_hist_chips(void);
static const char *proto_btn_text(void);

static void on_hist_chip(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nodelio_conn_hist_t hist;
    nodelio_conn_hist_cache_get(&hist);
    if (idx < 0 || idx >= (int)hist.n) {
        return;
    }
    const nodelio_conn_hist_entry_t *ent = &hist.e[idx];
    s_https = ent->https != 0;
    if (s_proto_lbl) {
        lv_label_set_text(s_proto_lbl, proto_btn_text());
    }
    if (s_ta_host) {
        lv_textarea_set_text(s_ta_host, ent->host);
    }
    if (s_ta_port) {
        lv_textarea_set_text(s_ta_port, ent->port[0] ? ent->port : "8899");
    }
    if (s_ta_owner && ent->owner[0]) {
        lv_textarea_set_text(s_ta_owner, ent->owner);
    }
    nl_ov_kb_set_visible(false);
    nl_ov_toast("已填入历史服务器");
}

static void rebuild_server_hist_chips(void)
{
    if (!s_hist_row) {
        return;
    }
    lv_obj_clean(s_hist_row);
    nodelio_conn_hist_t hist;
    nodelio_conn_hist_cache_get(&hist);
    if (hist.n == 0) {
        lv_obj_add_flag(s_hist_row, LV_OBJ_FLAG_HIDDEN);
        if (s_hist_hint) {
            lv_obj_add_flag(s_hist_hint, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_hist_hint) {
        lv_obj_clear_flag(s_hist_hint, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_hist_row, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < (int)hist.n && i < NODELIO_CONN_HIST_MAX; i++) {
        const nodelio_conn_hist_entry_t *ent = &hist.e[i];
        if (!ent->host[0]) {
            continue;
        }
        char label[120];
        std::snprintf(label, sizeof(label), "%s%s:%s", ent->https ? "S " : "", ent->host,
                      ent->port[0] ? ent->port : "8899");
        lv_obj_t *chip = lv_obj_create(s_hist_row);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, 36);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 12, 0);
        lv_obj_set_style_radius(chip, 18, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(chip, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_color(chip, UI_CAPSULE_ON, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, UI_TILE_OFF_BORDER, 0);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, 8);
        lv_obj_add_event_cb(chip, on_hist_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(chip);
        nl_ov_set_font(lbl);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
        lv_obj_center(lbl);
    }
}

/** 方屏（sila-86）用分组卡片布局；宽屏沿用原来的平铺小节。 */
static bool connect_square_layout(void)
{
    return nodelio_display_profile()->nav_width == 0;
}

/** 输入框边框宽度固定：聚焦只换颜色。改宽度会连带改内容区高度，整页跟着跳。 */
#define CONN_TA_BORDER 2
/** 方屏字段内边距大一点，触摸更容易命中；宽屏维持原来的紧凑值。 */
static int conn_ta_pad_v(void) { return connect_square_layout() ? 12 : 8; }

static int connect_field_h(void)
{
    return nl_ov_ui_font_line_h() + 4 + 2 * CONN_TA_BORDER + 2 * conn_ta_pad_v();
}

static void style_ta(lv_obj_t *ta)
{
    if (!ta) {
        return;
    }
    /* 先切单行：它内部会把高度改成 LV_SIZE_CONTENT，必须在之后再锁定固定高。 */
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_cursor_click_pos(ta, true);

    const int font_h = nl_ov_ui_font_line_h();
    const int pad_v = conn_ta_pad_v();
    /* 高度给字体上下留出 4px 安全裕量，彻底避免因基线/字高微小偏差导致 cur_pos.y+font_h > content_h 触发自动滚动 */
    const int safe_h = font_h + 4;
    lv_obj_set_height(ta, safe_h + 2 * CONN_TA_BORDER + 2 * pad_v);
    lv_obj_set_style_pad_ver(ta, pad_v, 0);
    lv_obj_set_style_pad_hor(ta, 12, 0);
    lv_obj_set_style_text_line_space(ta, 0, 0);
    lv_obj_set_style_bg_color(ta, UI_FIELD, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, CONN_TA_BORDER, 0);
    lv_obj_set_style_border_color(ta, UI_BORDER, 0);
    lv_obj_set_style_radius(ta, connect_square_layout() ? UI_RADIUS_SM : 0, 0);
    nl_ov_set_font(ta);
    lv_obj_set_style_text_color(ta, UI_TEXT, 0);
    lv_obj_set_style_text_font(ta, nl_ov_ui_font(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(ta, UI_TEXT_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, UI_FIELD_FOCUS, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, CONN_TA_BORDER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, UI_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
    /* 彻底杜绝垂直滚动与滚动条闪烁：单行输入框仅允许水平滚动 */
    lv_obj_set_scroll_dir(ta, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    /* 关掉聚焦时的自动滚动：它带 400ms 动画，和键盘弹开的重新布局叠在一起就是「跳」。 */
    lv_obj_clear_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

static void add_field_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    nl_ov_set_font(l);
    lv_obj_set_style_text_color(l, UI_TEXT_DIM, 0);
}

/* —— 方屏分组卡片：网络 / 服务器 / 设备 —— */
#define CONN_CARD_MAX 4
static lv_obj_t *s_conn_cards[CONN_CARD_MAX];
static int s_conn_card_n;
static lv_obj_t *s_conn_actions; /* 方屏底部常驻操作条 */
static lv_obj_t *s_conn_tip;

/** 分组容器：方屏建卡片，宽屏仍用平铺小节标题。返回后续控件该挂的父对象。 */
static lv_obj_t *conn_section(const char *title)
{
    if (!connect_square_layout()) {
        if (title && title[0]) {
            add_field_label(s_connect_scroll, title);
        }
        return s_connect_scroll;
    }
    lv_obj_t *card = lv_obj_create(s_connect_scroll);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    ui_style_card(card);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (s_conn_card_n < CONN_CARD_MAX) {
        s_conn_cards[s_conn_card_n++] = card;
    }
    if (title && title[0]) {
        lv_obj_t *hdr = lv_obj_create(card);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_width(hdr, lv_pct(100));
        lv_obj_set_height(hdr, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(hdr, 6, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *bar = lv_obj_create(hdr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 3, 13);
        lv_obj_set_style_bg_color(bar, UI_ACCENT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);

        lv_obj_t *t = lv_label_create(hdr);
        lv_label_set_text(t, title);
        nl_ov_set_font(t);
        lv_obj_set_style_text_color(t, UI_TEXT, 0);
    }
    return card;
}

/* —— keyboard (lazy) —— */

static const char *const s_kb_map_host[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    ".", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", LV_SYMBOL_OK, "",
};
static const lv_buttonmatrix_ctrl_t s_kb_ctrl_host[] = {
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_2, LV_BUTTONMATRIX_CTRL_WIDTH_2,
};

/* 字母盘按 1# 进入的数字符号页。默认图没有「.」，输 IP 只能回字母盘找。 */
static const char *const s_kb_map_spec[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", ".", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, "",
};
#define NL_KB(v) static_cast<lv_buttonmatrix_ctrl_t>(v)
static const lv_buttonmatrix_ctrl_t s_kb_ctrl_spec[] = {
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
    NL_KB(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1), NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_BUTTONMATRIX_CTRL_POPOVER | 1),
    NL_KB(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2), NL_KB(LV_BUTTONMATRIX_CTRL_CHECKED | 2), NL_KB(6), NL_KB(LV_BUTTONMATRIX_CTRL_CHECKED | 2),
    NL_KB(LV_KEYBOARD_CTRL_BUTTON_FLAGS | 2),
};
#undef NL_KB

static const char *const s_kb_map_port[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, "",
};
static const lv_buttonmatrix_ctrl_t s_kb_ctrl_port[] = {
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
    LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1, LV_BUTTONMATRIX_CTRL_WIDTH_1,
};

static const char *proto_btn_text(void)
{
    return s_https ? "HTTPS" : "HTTP";
}

/** 主机用数字盘，字母盘上的键盘图标回到数字盘，而不是关掉键盘 */
static lv_keyboard_mode_t kb_home_mode(lv_obj_t *ta)
{
    if (ta == s_ta_host) {
        return LV_KEYBOARD_MODE_USER_1;
    }
    if (ta == s_ta_port) {
        return LV_KEYBOARD_MODE_USER_2;
    }
    return LV_KEYBOARD_MODE_TEXT_LOWER;
}

/** 横屏用侧栏键盘（nav_width≠0）；方屏仍贴底。 */
static bool kb_side_layout(void)
{
    return nodelio_display_profile()->nav_width != 0;
}

/** 与设置子页同一块：左轨右侧的内容区。未布局完时用屏宽减去左轨。 */
static void connect_content_rect(int *x, int *y, int *w, int *h)
{
    const auto *p = nodelio_display_profile();
    if (p && p->nav_width) {
        *x = (int)p->nav_width;
        *y = 0;
        *w = (int)p->width - (int)p->nav_width;
        *h = (int)p->height;
        return;
    }
    *x = 0;
    *y = 0;
    *w = LV_HOR_RES;
    *h = LV_VER_RES;
}

/** 键盘弹出：宽屏左侧表单紧凑、右侧键盘铺满全部剩余宽度（约 724px）；方屏贴底并给表单加底垫。 */
static void kb_apply_layout(bool kb_on)
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    connect_content_rect(&x, &y, &w, &h);
    if (kb_side_layout()) {
        if (kb_on && s_kb) {
            /* 宽屏键盘展开：左侧表单宽 460px 足够容纳，右侧键盘饱满占据剩余所有宽度 */
            int form_w = 460;
            if (form_w > w / 2) {
                form_w = w / 2;
            }
            int kb_w = w - form_w;
            if (s_connect) {
                lv_obj_set_pos(s_connect, x, y);
                lv_obj_set_size(s_connect, form_w, h);
            }
            /* lv_keyboard 默认 BOTTOM_MID；set_pos 只是对齐偏移，会把键盘推出屏幕 */
            lv_obj_align(s_kb, LV_ALIGN_DEFAULT, x + form_w, y);
            lv_obj_set_size(s_kb, kb_w, h);
            lv_obj_set_style_min_height(s_kb, 48, LV_PART_ITEMS);
            lv_obj_set_style_pad_all(s_kb, 8, 0);
            lv_obj_set_style_pad_row(s_kb, 6, 0);
            lv_obj_set_style_pad_column(s_kb, 6, 0);
        } else {
            if (s_connect) {
                lv_obj_set_pos(s_connect, x, y);
                lv_obj_set_size(s_connect, w, h);
            }
            if (s_kb) {
                lv_obj_align(s_kb, LV_ALIGN_DEFAULT, x + w, y);
            }
        }
        if (s_connect_scroll) {
            lv_obj_set_style_pad_bottom(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
            lv_obj_set_style_pad_right(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
        }
        return;
    }
    if (s_connect) {
        lv_obj_set_pos(s_connect, x, y);
        lv_obj_set_size(s_connect, w, h);
    }
    if (s_kb) {
        lv_obj_set_width(s_kb, lv_pct(100));
        lv_obj_set_height(s_kb, lv_pct(42));
        lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_min_height(s_kb, 36, LV_PART_ITEMS);
    }
    /* 方屏「保存」条是 flex 末项：给整页加底部内边距，键盘弹开时它被顶到键盘上沿，
     * 而不是被键盘盖住。别用 lv_obj_align —— flex 子项的位置由流式布局决定。 */
    if (s_conn_actions && s_connect) {
        const int kb_h = kb_on ? (h * 42) / 100 : 0;
        lv_obj_set_style_pad_bottom(s_connect, kb_h, 0);
        /* 改内边距不会自动重排父容器，显式更新一次，否则卡片还按旧高度排。 */
        lv_obj_update_layout(s_connect);
    }
    if (s_connect_scroll) {
        /* 底部操作条已常驻，滚动区只需留一点呼吸位。 */
        lv_obj_set_style_pad_bottom(s_connect_scroll, kb_on ? 8 : NL_UI_SETTINGS_DETAIL_PAD, 0);
        lv_obj_set_style_pad_right(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
    }
}

void nl_ov_connect_relayout(void)
{
    if (!s_connect) {
        return;
    }
    const bool kb_on = s_kb && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    kb_apply_layout(kb_on);
}

static void wifi_list_hide(void)
{
    if (!s_wifi_list) {
        return;
    }
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(s_wifi_list);
}

void nl_ov_kb_set_visible(bool on)
{
    if (!s_kb) {
        kb_apply_layout(false);
        return;
    }
    kb_apply_layout(on);
    if (on) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        /* 方屏底栏会被键盘挡住；宽屏左轨跟其他设置页一样留着。 */
        if (s_tabbar && !nodelio_display_profile()->nav_width) {
            lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_connect) {
            lv_obj_move_foreground(s_connect);
        }
        lv_obj_move_foreground(s_kb);
        nl_ov_raise_chrome();
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, nullptr);
        bool form_open = s_connect && !lv_obj_has_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_tabbar && s_visible && !form_open) {
            nl_ov_nav_chrome_sync();
        }
    }
}

static void ensure_kb(void)
{
    if (s_kb) {
        return;
    }
    /* 连接页在 lv_layer_top()，键盘必须同层；宽屏侧栏、方屏贴底见 kb_apply_layout */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(s_kb, 8, 0);
    lv_obj_set_style_pad_row(s_kb, 8, 0);
    lv_obj_set_style_pad_column(s_kb, 8, 0);
    lv_obj_set_style_bg_color(s_kb, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb, kb_side_layout() ? 1 : 0, 0);
    lv_obj_set_style_border_side(s_kb, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(s_kb, UI_BORDER, 0);
    lv_obj_set_style_radius(s_kb, 0, 0);
    kb_apply_layout(false);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_USER_1, s_kb_map_host, s_kb_ctrl_host);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_USER_2, s_kb_map_port, s_kb_ctrl_port);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_SPECIAL, s_kb_map_spec, s_kb_ctrl_spec);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(
        s_kb,
        [](lv_event_t *e) {
            lv_event_code_t code = lv_event_get_code(e);
            lv_obj_t *kb = static_cast<lv_obj_t *>(lv_event_get_target(e));
            if (code == LV_EVENT_CANCEL && kb) {
                uint32_t id = lv_buttonmatrix_get_selected_button(kb);
                const char *txt = (id == LV_BUTTONMATRIX_BUTTON_NONE)
                                      ? nullptr
                                      : lv_buttonmatrix_get_button_text(kb, id);
                lv_keyboard_mode_t home = kb_home_mode(lv_keyboard_get_textarea(kb));
                if (txt && strcmp(txt, LV_SYMBOL_KEYBOARD) == 0 &&
                    lv_keyboard_get_mode(kb) != home) {
                    lv_keyboard_set_mode(kb, home);
                    return;
                }
            }
            if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
                nl_ov_kb_set_visible(false);
                return;
            }
            if (code == LV_EVENT_VALUE_CHANGED && kb) {
                uint32_t id = lv_buttonmatrix_get_selected_button(kb);
                const char *txt = lv_buttonmatrix_get_button_text(kb, id);
                if (txt && strcmp(txt, "abc") == 0) {
                    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
                }
            }
        },
        LV_EVENT_ALL, nullptr);
}

static void bind_kb_for_ta(lv_obj_t *ta)
{
    ensure_kb();
    if (!s_kb || !ta) {
        return;
    }
    if (lv_keyboard_get_textarea(s_kb) == ta && !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_keyboard_set_textarea(s_kb, ta);
    lv_keyboard_mode_t mode = kb_home_mode(ta);
    if (lv_keyboard_get_mode(s_kb) != mode) {
        lv_keyboard_set_mode(s_kb, mode);
    }
    nl_ov_kb_set_visible(true);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
}

static void wire_ta_input(lv_obj_t *ta)
{
    auto cb = [](lv_event_t *e) {
        lv_obj_t *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
        if (!ta) {
            return;
        }
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
        bind_kb_for_ta(ta);
    };
    lv_obj_add_event_cb(ta, cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(ta, cb, LV_EVENT_CLICKED, nullptr);
}

void nl_ov_close_connect(void)
{
    nl_ov_close_forms();
}

static void apply_wifi_creds(const char *ssid, const char *pass, nodelio_nvs_config_t *cfg)
{
    if (!ssid || !ssid[0] || !cfg) {
        return;
    }
    auto &ssid_mgr = SsidManager::GetInstance();
    if (pass && pass[0]) {
        ssid_mgr.AddSsid(ssid, pass);
    } else {
        const auto &list = ssid_mgr.GetSsidList();
        int found = -1;
        for (size_t i = 0; i < list.size(); i++) {
            if (list[i].ssid == ssid) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) {
            return;
        }
        ssid_mgr.SetDefaultSsid(found);
    }
    std::snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", ssid);
    if (pass && pass[0]) {
        std::snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", pass);
    }
}

static void trim_cstr(char *s)
{
    if (!s) {
        return;
    }
    char *p = s;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != s) {
        std::memmove(s, p, std::strlen(p) + 1);
    }
    size_t n = std::strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

typedef struct {
    char ssid[33];
    char pass[65];
    char host[96];
    char port[8];
    char owner[64];
    char screenshot_url[128];
    uint8_t https;
    uint8_t reboot;
} nl_conn_save_job_t;

static void persist_connect_job(nl_conn_save_job_t *job)
{
    if (!job) {
        return;
    }
    nodelio_nvs_config_t cfg{};
    nodelio_nvs_cache_get(&cfg);
    char new_url[sizeof(cfg.server_url)];
    std::snprintf(new_url, sizeof(new_url), "%s://%s:%s", job->https ? "https" : "http", job->host,
                  job->port[0] ? job->port : "8899");
    const bool server_changed =
        std::strcmp(cfg.server_url, new_url) != 0 || std::strcmp(cfg.owner, job->owner) != 0;
    std::snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", new_url);
    std::snprintf(cfg.owner, sizeof(cfg.owner), "%s", job->owner);
    std::snprintf(cfg.screenshot_url, sizeof(cfg.screenshot_url), "%s", job->screenshot_url);
    const bool wifi_changed =
        job->ssid[0] && (std::strcmp(job->ssid, cfg.wifi_ssid) != 0 || job->pass[0]);
    if (job->ssid[0]) {
        std::snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", job->ssid);
        if (job->pass[0]) {
            std::snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", job->pass);
        }
    }
    if (server_changed) {
        nodelio_nvs_clear_mqtt(&cfg);
        (void)nodelio_layout_cache_clear();
        (void)nodelio_apps_cache_clear();
    }
    nodelio_conn_hist_t hist{};
    nodelio_conn_hist_cache_get(&hist);
    nodelio_conn_hist_push(&hist, job->https != 0, job->host, job->port[0] ? job->port : "8899",
                           job->owner);
    nodelio_conn_hist_cache_put(&hist);

    nodelio_nvs_config_t out = cfg;
    esp_err_t err = nodelio_nvs_save_connection(&out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connection save/readback failed: %s", esp_err_to_name(err));
        nodelio_nvs_config_t revert{};
        if (nodelio_nvs_load(&revert) == ESP_OK) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                s_cfg = revert;
            }
        }
        nl_ov_toast(err == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "保存失败：存储已满" : "保存失败");
        heap_caps_free(job);
        return;
    }
    if (wifi_changed) {
        apply_wifi_creds(job->ssid, job->pass, &out);
    }
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            s_cfg = out;
        }
    }
    nodelio_nvs_cache_put(&out);
    if (nodelio_nvs_has_headroom()) {
        (void)nodelio_conn_hist_save(&hist);
    } else {
        ESP_LOGW(TAG, "skip conn_hist flash (keep credentials)");
    }
    ESP_LOGI(TAG, "saved server=%s reboot=%u", out.server_url, (unsigned)job->reboot);
    const bool reboot = job->reboot != 0;
    heap_caps_free(job);
    job = nullptr;
    if (!reboot) {
        nl_ov_toast("已保存");
        return;
    }
    nl_ov_toast(server_changed ? "已保存，将重新注册…" : "已保存，重启…");
    static esp_timer_handle_t reboot_timer = nullptr;
    if (!reboot_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void *) {
            Application::GetInstance().Schedule([]() { esp_restart(); });
        };
        args.name = "nl_conn_reboot";
        err = esp_timer_create(&args, &reboot_timer);
    }
    if (err == ESP_OK) {
        (void)esp_timer_stop(reboot_timer);
        err = esp_timer_start_once(reboot_timer, 800000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reboot scheduling failed: %s", esp_err_to_name(err));
        nl_ov_toast("已保存，请手动重启");
    }
}

/**
 * 从连接表单落盘。reboot=true 为「保存并重启」；false 为返回时静默保存。
 * 点击回调可能跑在 sila_touch 的 4KB 栈上：此处只拷短字段，重活丢主任务。
 */
static bool persist_connect_from_form(bool reboot)
{
    auto *job = static_cast<nl_conn_save_job_t *>(
        heap_caps_malloc(sizeof(nl_conn_save_job_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!job) {
        nl_ov_toast("保存失败");
        return false;
    }
    std::memset(job, 0, sizeof(*job));
    if (s_ta_ssid) {
        std::snprintf(job->ssid, sizeof(job->ssid), "%s", lv_textarea_get_text(s_ta_ssid));
    }
    if (s_ta_pass) {
        std::snprintf(job->pass, sizeof(job->pass), "%s", lv_textarea_get_text(s_ta_pass));
    }
    if (s_ta_host) {
        std::snprintf(job->host, sizeof(job->host), "%s", lv_textarea_get_text(s_ta_host));
    }
    if (s_ta_port) {
        std::snprintf(job->port, sizeof(job->port), "%s", lv_textarea_get_text(s_ta_port));
    }
    if (s_ta_owner) {
        std::snprintf(job->owner, sizeof(job->owner), "%s", lv_textarea_get_text(s_ta_owner));
    }
    if (s_ta_screenshot) {
        std::snprintf(job->screenshot_url, sizeof(job->screenshot_url), "%s", lv_textarea_get_text(s_ta_screenshot));
    }
    trim_cstr(job->screenshot_url);
    trim_cstr(job->ssid);
    trim_cstr(job->pass);
    trim_cstr(job->host);
    trim_cstr(job->port);
    trim_cstr(job->owner);
    if (!job->host[0] || !job->owner[0]) {
        heap_caps_free(job);
        if (reboot) {
            nl_ov_toast("请填写主机与绑定码");
        }
        return false;
    }
    if (job->ssid[0] && !job->pass[0] && std::strcmp(job->ssid, s_cfg.wifi_ssid) != 0) {
        heap_caps_free(job);
        if (reboot) {
            nl_ov_toast("新网络请填密码");
        }
        return false;
    }
    if (!job->port[0]) {
        std::snprintf(job->port, sizeof(job->port), "8899");
    }
    job->https = s_https ? 1 : 0;
    job->reboot = reboot ? 1 : 0;
    nl_ov_toast("正在保存…");
    Application::GetInstance().Schedule([job]() { persist_connect_job(job); });
    return true;
}

static void on_save(lv_event_t *e)
{
    (void)e;
    (void)persist_connect_from_form(true);
}

static void on_nav_back(lv_event_t *e)
{
    (void)e;
    (void)persist_connect_from_form(false);
    nl_ov_close_connect();
}

void nl_ov_open_connect(void)
{
    if (!s_connect) {
        return;
    }
    nl_ov_close_settings_detail();
    // Refresh from the RAM cache: background registration may have changed it.
    nodelio_nvs_cache_get(&s_cfg);
    char host[96] = {0};
    char port[8] = {0};
    nl_url_parse_server(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

    std::string cur = WifiManager::GetInstance().GetSsid();
    if (cur.empty() && s_cfg.wifi_ssid[0]) {
        cur = s_cfg.wifi_ssid;
    }
    if (s_ta_ssid) {
        lv_textarea_set_text(s_ta_ssid, cur.c_str());
    }
    if (s_ta_pass) {
        lv_textarea_set_text(s_ta_pass, "");
    }
    if (s_ta_host) {
        lv_textarea_set_text(s_ta_host, host);
    }
    if (s_ta_port) {
        lv_textarea_set_text(s_ta_port, port);
    }
    if (s_ta_owner) {
        lv_textarea_set_text(s_ta_owner, s_cfg.owner);
    }
    if (s_ta_screenshot) {
        lv_textarea_set_text(s_ta_screenshot, s_cfg.screenshot_url);
    }
    if (s_proto_lbl) {
        lv_label_set_text(s_proto_lbl, proto_btn_text());
    }
    if (s_scan_btn_lbl && !s_wifi_scanning) {
        lv_label_set_text(s_scan_btn_lbl, "扫描");
    }
    rebuild_server_hist_chips();
    nl_ov_kb_set_visible(false);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
    if (s_tabbar && !nodelio_display_profile()->nav_width) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_raise_chrome();
}


void nl_ov_build_connect(void)
{
    /* 挂在 lv_layer_top() 而非 s_root flex 容器，避免 move_foreground 打乱 flex 顺序导致状态栏移位 */
    s_connect = lv_obj_create(lv_layer_top());
    kb_apply_layout(false);
    lv_obj_set_style_bg_color(s_connect, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_connect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_connect, 0, 0);
    lv_obj_set_style_pad_all(s_connect, 0, 0);
    lv_obj_set_flex_flow(s_connect, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_connect, LV_OBJ_FLAG_HIDDEN);

    (void)ui_make_nav_bar(s_connect, UI_NAV_H, connect_square_layout() ? "连接设置" : "连接", on_nav_back);

    s_connect_scroll = lv_obj_create(s_connect);
    lv_obj_set_width(s_connect_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_connect_scroll, 1);
    lv_obj_set_style_bg_color(s_connect_scroll, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_connect_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_connect_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_right(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_top(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_row(s_connect_scroll, 6, 0);
    lv_obj_set_flex_flow(s_connect_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_connect_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_connect_scroll, LV_DIR_VER);
    lv_obj_clear_flag(s_connect_scroll, LV_OBJ_FLAG_SCROLL_ELASTIC);

    nodelio_nvs_cache_get(&s_cfg);
    {
        nodelio_conn_hist_t hist;
        nodelio_conn_hist_cache_get(&hist);
        /* 首次：把当前已配服务器写入历史，便于回填 */
        if (hist.n == 0 && s_cfg.server_url[0]) {
            char h0[96] = {0};
            char p0[8] = {0};
            bool https0 = false;
            nl_url_parse_server(s_cfg.server_url, h0, sizeof(h0), p0, sizeof(p0), &https0);
            if (h0[0]) {
                nodelio_conn_hist_push(&hist, https0, h0, p0, s_cfg.owner);
                nodelio_conn_hist_cache_put(&hist);
                Application::GetInstance().Schedule([hist]() {
                    (void)nodelio_conn_hist_save(&hist);
                });
            }
        }
    }
    char host[96] = {0};
    char port[8] = {0};
    nl_url_parse_server(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

    /* WiFi：扫描选网，不必手填 SSID */
    lv_obj_t *wifi_card = conn_section("网络");
    add_field_label(wifi_card, "WiFi 名称");
    lv_obj_t *ssid_row = lv_obj_create(wifi_card);
    lv_obj_set_width(ssid_row, lv_pct(100));
    lv_obj_set_height(ssid_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ssid_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ssid_row, 0, 0);
    lv_obj_set_style_pad_all(ssid_row, 0, 0);
    lv_obj_set_style_pad_column(ssid_row, UI_GAP, 0);
    lv_obj_set_flex_flow(ssid_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ssid_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ssid_row, LV_OBJ_FLAG_SCROLLABLE);

    s_ta_ssid = lv_textarea_create(ssid_row);
    style_ta(s_ta_ssid);
    lv_obj_set_flex_grow(s_ta_ssid, 1);
    lv_textarea_set_max_length(s_ta_ssid, 32);
    lv_textarea_set_placeholder_text(s_ta_ssid, "点扫描选网或手填");
    wire_ta_input(s_ta_ssid);

    lv_obj_t *scan_btn = ui_make_action_btn(ssid_row, "扫描", connect_square_layout() ? 72 : 80, connect_field_h(), on_wifi_scan_btn);
    s_scan_btn_lbl = lv_obj_get_child(scan_btn, 0);
    if (connect_square_layout()) {
        lv_obj_set_style_bg_color(scan_btn, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_border_width(scan_btn, 1, 0);
        lv_obj_set_style_border_color(scan_btn, UI_ACCENT, 0);
        lv_obj_set_style_border_opa(scan_btn, LV_OPA_COVER, 0);
        if (s_scan_btn_lbl) {
            lv_obj_set_style_text_color(s_scan_btn_lbl, UI_ACCENT, 0);
        }
    }

    s_wifi_list = lv_obj_create(wifi_card);
    lv_obj_set_width(s_wifi_list, lv_pct(100));
    lv_obj_set_height(s_wifi_list, 168); /* 约 4 行，内部自滚，避免撑满挡住下面表单 */
    lv_obj_set_style_bg_color(s_wifi_list, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_list, 1, 0);
    lv_obj_set_style_border_color(s_wifi_list, UI_BORDER, 0);
    lv_obj_set_style_radius(s_wifi_list, UI_RADIUS_SM, 0);
    lv_obj_set_style_pad_all(s_wifi_list, 4, 0);
    lv_obj_set_style_pad_row(s_wifi_list, 2, 0);
    lv_obj_set_flex_flow(s_wifi_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_wifi_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wifi_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    s_wifi_scanning = false;
    s_scan_n = 0;

    add_field_label(wifi_card, "WiFi 密码（已存网络可留空）");
    s_ta_pass = lv_textarea_create(wifi_card);
    style_ta(s_ta_pass);
    lv_obj_set_width(s_ta_pass, lv_pct(100));
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_max_length(s_ta_pass, 64);
    lv_textarea_set_placeholder_text(s_ta_pass, "输入密码");
    wire_ta_input(s_ta_pass);

    lv_obj_t *srv_card = conn_section("服务器");
    lv_obj_t *proto_row = lv_obj_create(srv_card);
    lv_obj_set_width(proto_row, lv_pct(100));
    lv_obj_set_height(proto_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(proto_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(proto_row, 0, 0);
    lv_obj_set_style_pad_all(proto_row, 0, 0);
    lv_obj_set_flex_flow(proto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(proto_row, UI_GAP, 0);
    lv_obj_set_flex_align(proto_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(proto_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *proto_btn = ui_make_action_btn(
        proto_row, proto_btn_text(), connect_square_layout() ? 68 : 76, connect_field_h(),
        [](lv_event_t *) {
            s_https = !s_https;
            if (s_proto_lbl) {
                lv_label_set_text(s_proto_lbl, proto_btn_text());
            }
        });
    s_proto_lbl = lv_obj_get_child(proto_btn, 0);
    if (connect_square_layout()) {
        lv_obj_set_style_bg_color(proto_btn, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_border_width(proto_btn, 1, 0);
        lv_obj_set_style_border_color(proto_btn, UI_TILE_OFF_BORDER, 0);
        if (s_proto_lbl) {
            lv_obj_set_style_text_color(s_proto_lbl, UI_ACCENT, 0);
        }
    }

    s_ta_host = lv_textarea_create(proto_row);
    style_ta(s_ta_host);
    lv_obj_set_flex_grow(s_ta_host, 1);
    lv_textarea_set_max_length(s_ta_host, 95);
    lv_textarea_set_text(s_ta_host, host);
    lv_textarea_set_placeholder_text(s_ta_host, "IP 或域名");
    lv_textarea_set_accepted_chars(s_ta_host,
                                   "0123456789.abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-");
    wire_ta_input(s_ta_host);

    s_ta_port = lv_textarea_create(proto_row);
    style_ta(s_ta_port);
    lv_obj_set_width(s_ta_port, connect_square_layout() ? 66 : 72);
    lv_textarea_set_placeholder_text(s_ta_port, "8899");
    lv_textarea_set_max_length(s_ta_port, 5);
    lv_textarea_set_text(s_ta_port, port);
    lv_textarea_set_accepted_chars(s_ta_port, "0123456789");
    wire_ta_input(s_ta_port);

    s_hist_hint = lv_label_create(srv_card);
    lv_label_set_text(s_hist_hint, "历史服务器（点选回填）");
    nl_ov_set_font(s_hist_hint);
    lv_obj_set_style_text_color(s_hist_hint, UI_TEXT_DIM, 0);
    lv_obj_add_flag(s_hist_hint, LV_OBJ_FLAG_HIDDEN);

    s_hist_row = lv_obj_create(srv_card);
    lv_obj_set_width(s_hist_row, lv_pct(100));
    lv_obj_set_height(s_hist_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_hist_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_hist_row, 0, 0);
    lv_obj_set_style_pad_all(s_hist_row, 0, 0);
    lv_obj_set_style_pad_column(s_hist_row, 6, 0);
    lv_obj_set_style_pad_row(s_hist_row, 6, 0);
    lv_obj_set_flex_flow(s_hist_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_hist_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_hist_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_hist_row, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dev_card = conn_section("设备");
    add_field_label(dev_card, "绑定码");
    s_ta_owner = lv_textarea_create(dev_card);
    style_ta(s_ta_owner);
    lv_obj_set_width(s_ta_owner, lv_pct(100));
    lv_textarea_set_max_length(s_ta_owner, sizeof(s_cfg.owner) - 1);
    lv_textarea_set_text(s_ta_owner, s_cfg.owner);
    wire_ta_input(s_ta_owner);

    add_field_label(dev_card, "截屏接收地址（留空同服务器）");
    s_ta_screenshot = lv_textarea_create(dev_card);
    style_ta(s_ta_screenshot);
    lv_textarea_set_placeholder_text(s_ta_screenshot, "如 192.168.1.x 或完整 URL");
    lv_obj_set_width(s_ta_screenshot, lv_pct(100));
    lv_textarea_set_max_length(s_ta_screenshot, sizeof(s_cfg.screenshot_url) - 1);
    lv_textarea_set_text(s_ta_screenshot, s_cfg.screenshot_url);
    wire_ta_input(s_ta_screenshot);

    /* 方屏把「保存」钉在屏底，不随表单滚走；宽屏仍留在表单末尾。 */
    lv_obj_t *btn_host = s_connect_scroll;
    if (connect_square_layout()) {
        s_conn_actions = lv_obj_create(s_connect);
        lv_obj_remove_style_all(s_conn_actions);
        lv_obj_set_width(s_conn_actions, lv_pct(100));
        lv_obj_set_height(s_conn_actions, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_conn_actions, UI_PANEL, 0);
        lv_obj_set_style_bg_opa(s_conn_actions, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_conn_actions, 1, 0);
        lv_obj_set_style_border_side(s_conn_actions, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_color(s_conn_actions, UI_BORDER, 0);
        lv_obj_set_style_border_opa(s_conn_actions, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_conn_actions, NL_UI_SETTINGS_DETAIL_PAD, 0);
        lv_obj_set_style_pad_ver(s_conn_actions, 10, 0);
        lv_obj_set_style_pad_column(s_conn_actions, 10, 0);
        lv_obj_set_flex_flow(s_conn_actions, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_conn_actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(s_conn_actions, LV_OBJ_FLAG_SCROLLABLE);
        btn_host = s_conn_actions;
    }

    lv_obj_t *btn_row = lv_obj_create(btn_host);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_column(btn_row, 10, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btn_row);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, connect_square_layout() ? 40 : UI_BTN_H_SM);
    lv_obj_set_ext_click_area(cancel, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(cancel, connect_square_layout() ? UI_SURFACE : UI_OFFLINE, 0);
    lv_obj_set_style_border_width(cancel, connect_square_layout() ? 1 : 0, 0);
    lv_obj_set_style_border_color(cancel, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_radius(cancel, connect_square_layout() ? UI_RADIUS_SM : 0, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { nl_ov_close_connect(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    nl_ov_set_font(cl);
    lv_obj_set_style_text_color(cl, UI_TEXT_SUB, 0);
    lv_obj_center(cl);

    lv_obj_t *save = lv_button_create(btn_row);
    lv_obj_set_flex_grow(save, connect_square_layout() ? 2 : 1);
    lv_obj_set_height(save, connect_square_layout() ? 40 : UI_BTN_H_SM);
    lv_obj_set_ext_click_area(save, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(save, UI_ACCENT, 0);
    lv_obj_set_style_bg_color(save, UI_ACCENT_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, connect_square_layout() ? UI_RADIUS_SM : 0, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "保存并重启");
    nl_ov_set_font(sl);
    lv_obj_set_style_text_color(sl, UI_ON_TILE, 0);
    lv_obj_center(sl);

    if (!connect_square_layout()) {
        s_conn_tip = lv_label_create(s_connect_scroll);
        lv_label_set_text(s_conn_tip, "先点「扫描」选 WiFi，再填密码与服务器；点「保存并重启」写入闪存。返回也会保存已填主机与绑定码");
        nl_ov_set_font(s_conn_tip);
        lv_obj_set_style_text_color(s_conn_tip, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(s_conn_tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_conn_tip, lv_pct(100));
    }
    rebuild_server_hist_chips();
}

static void on_ap_pick(lv_event_t *e)
{
    const char *ssid = static_cast<const char *>(lv_event_get_user_data(e));
    if (!ssid || !s_ta_ssid) {
        return;
    }
    lv_textarea_set_text(s_ta_ssid, ssid);
    wifi_list_hide(); /* 关掉列表，露出密码/服务器/绑定码 */
    nl_ov_toast("已选网络，请填密码");
    if (s_ta_pass) {
        lv_obj_add_state(s_ta_pass, LV_STATE_FOCUSED);
        bind_kb_for_ta(s_ta_pass);
    }
}

static void wifi_list_rebuild(void)
{
    if (!s_wifi_list) {
        return;
    }
    lv_obj_clean(s_wifi_list);
    lv_obj_clear_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    if (s_scan_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_wifi_list);
        lv_label_set_text(empty, "未扫到网络，可手填名称");
        nl_ov_set_font(empty);
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
        return;
    }
    for (int i = 0; i < s_scan_n; i++) {
        lv_obj_t *btn = lv_button_create(s_wifi_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_bg_color(btn, UI_FIELD, 0);
        lv_obj_set_style_bg_color(btn, UI_BACK_BTN, LV_STATE_PRESSED);
        lv_obj_set_ext_click_area(btn, UI_EXT_CLICK);
        lv_obj_set_style_radius(btn, UI_RADIUS_XS, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_add_event_cb(btn, on_ap_pick, LV_EVENT_CLICKED, s_scan_aps[i].ssid);

        char line[48];
        std::snprintf(line, sizeof(line), "%.24s  %ddBm%s", s_scan_aps[i].ssid,
                      (int)s_scan_aps[i].rssi, s_scan_aps[i].auth ? "" : " 开放");
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, line);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, lv_pct(100));
        nl_ov_set_font(lbl);
        lv_obj_set_style_text_color(lbl, UI_TEXT, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    auto &wifi = WifiManager::GetInstance();

    /*
     * SoftAP / Station 都会在 SCAN_DONE 里先读走 AP 列表。
     * 扫描全程 suppress，避免 60s 连网超时把 SoftAP 又拉起来。
     */
    nodelio_wifi_set_suppress_softap(true);
    if (wifi.IsConfigMode()) {
        wifi.StopConfigAp();
        vTaskDelay(pdMS_TO_TICKS(400));
    } else {
        wifi.StopStation();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    int n = 0;
    esp_err_t err = nodelio_wifi_scan(s_scan_aps, NL_WIFI_SCAN_MAX, &n);
    s_scan_n = (err == ESP_OK) ? n : 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan: %s", esp_err_to_name(err));
    }

    /* 扫完只拉回 Station。不要在这里 StartConfigAp，否则又弹配网并抢走列表。 */
    nodelio_wifi_release_scan_netif();
    wifi.StartStation();
    nodelio_wifi_set_suppress_softap(false);

    {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            s_wifi_scanning = false;
            if (s_scan_btn_lbl) {
                lv_label_set_text(s_scan_btn_lbl, "扫描");
            }
            if (s_connect && !lv_obj_has_flag(s_connect, LV_OBJ_FLAG_HIDDEN)) {
                wifi_list_rebuild();
            }
        } else {
            s_wifi_scanning = false;
        }
    }

    if (s_scan_n > 0) {
        char msg[32];
        std::snprintf(msg, sizeof(msg), "找到 %d 个网络", s_scan_n);
        nl_ov_toast(msg);
    } else {
        nl_ov_toast(err == ESP_OK ? "未扫到，可手填" : "扫描失败");
    }
    vTaskDelete(nullptr);
}

static void on_wifi_scan_btn(lv_event_t *e)
{
    (void)e;
    if (s_wifi_scanning) {
        return;
    }
    s_wifi_scanning = true;
    if (s_scan_btn_lbl) {
        lv_label_set_text(s_scan_btn_lbl, "…");
    }
    nl_ov_toast("正在扫描…");
    if (xTaskCreate(wifi_scan_task, "nl_wscan", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_wifi_scanning = false;
        if (s_scan_btn_lbl) {
            lv_label_set_text(s_scan_btn_lbl, "扫描");
        }
        nl_ov_toast("扫描失败");
    }
}

static void paint_connect_action(lv_obj_t *lbl)
{
    if (!lbl) {
        return;
    }
    lv_obj_t *btn = lv_obj_get_parent(lbl);
    if (!btn) {
        return;
    }
    if (connect_square_layout()) {
        lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_border_color(btn, lbl == s_scan_btn_lbl ? UI_ACCENT : UI_TILE_OFF_BORDER, 0);
    } else {
        lv_obj_set_style_bg_color(btn, UI_BACK_BTN, 0);
    }
    lv_obj_set_style_text_color(lbl, UI_ACCENT, 0);
}

void nl_ov_connect_reapply_appearance(void)
{
    if (s_connect) {
        lv_obj_set_style_bg_color(s_connect, UI_BG, 0);
        if (lv_obj_get_child_count(s_connect) > 0) {
            lv_obj_t *nav = lv_obj_get_child(s_connect, 0);
            lv_obj_set_style_bg_color(nav, UI_PANEL, 0);
            if (lv_obj_get_child_count(nav) > 0) {
                lv_obj_t *back = lv_obj_get_child(nav, 0);
                lv_obj_set_style_bg_color(back, UI_BACK_BTN, 0);
                if (lv_obj_t *bl = lv_obj_get_child(back, 0)) {
                    lv_obj_set_style_text_color(bl, UI_ACCENT, 0);
                }
            }
            if (lv_obj_get_child_count(nav) > 1) {
                lv_obj_set_style_text_color(lv_obj_get_child(nav, 1), UI_TEXT, 0);
            }
        }
        for (lv_obj_t *ta : {s_ta_ssid, s_ta_pass, s_ta_host, s_ta_port, s_ta_owner, s_ta_screenshot}) {
            if (ta) {
                style_ta(ta);
            }
        }
        if (s_wifi_list) {
            lv_obj_set_style_bg_color(s_wifi_list, UI_PANEL, 0);
            lv_obj_set_style_border_color(s_wifi_list, UI_BORDER, 0);
        }
        if (s_hist_hint) {
            lv_obj_set_style_text_color(s_hist_hint, UI_TEXT_DIM, 0);
        }
        if (s_connect_scroll) {
            lv_obj_set_style_bg_color(s_connect_scroll, UI_BG, 0);
            const uint32_t n = lv_obj_get_child_count(s_connect_scroll);
            for (uint32_t i = 0; i < n; i++) {
                lv_obj_t *ch = lv_obj_get_child(s_connect_scroll, i);
                if (ch == s_conn_tip) {
                    lv_obj_set_style_text_color(ch, UI_TEXT_TIP, 0);
                } else if (ch != s_hist_hint && lv_obj_check_type(ch, &lv_label_class)) {
                    lv_obj_set_style_text_color(ch, UI_TEXT_DIM, 0);
                }
            }
            /* 宽屏：按钮行是滚动区最后一个子对象；方屏已移到常驻操作条。 */
            if (n > 0) {
                lv_obj_t *row = lv_obj_get_child(s_connect_scroll, n - 1);
                if (row && lv_obj_get_child_count(row) >= 2) {
                    lv_obj_t *cancel = lv_obj_get_child(row, 0);
                    lv_obj_t *save = lv_obj_get_child(row, 1);
                    lv_obj_set_style_bg_color(cancel, connect_square_layout() ? UI_SURFACE : UI_OFFLINE, 0);
                    lv_obj_set_style_border_color(cancel, UI_TILE_OFF_BORDER, 0);
                    if (lv_obj_t *cl = lv_obj_get_child(cancel, 0)) {
                        lv_obj_set_style_text_color(cl, UI_TEXT_SUB, 0);
                    }
                    lv_obj_set_style_bg_color(save, UI_ACCENT, 0);
                    lv_obj_set_style_bg_color(save, UI_ACCENT_PRESSED, LV_STATE_PRESSED);
                    if (lv_obj_t *sl = lv_obj_get_child(save, 0)) {
                        lv_obj_set_style_text_color(sl, UI_ON_TILE, 0);
                    }
                }
            }
        }
        for (int i = 0; i < s_conn_card_n; i++) {
            lv_obj_t *card = s_conn_cards[i];
            if (!card) {
                continue;
            }
            lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
            lv_obj_set_style_border_color(card, UI_TILE_OFF_BORDER, 0);
            ui_style_card(card);
            if (lv_obj_get_child_count(card) > 0) {
                lv_obj_t *hdr = lv_obj_get_child(card, 0);
                if (lv_obj_check_type(hdr, &lv_label_class)) {
                    lv_obj_set_style_text_color(hdr, UI_TEXT_SUB, 0);
                } else if (lv_obj_get_child_count(hdr) >= 2) {
                    lv_obj_t *bar = lv_obj_get_child(hdr, 0);
                    lv_obj_set_style_bg_color(bar, UI_ACCENT, 0);
                    lv_obj_t *t = lv_obj_get_child(hdr, 1);
                    if (t && lv_obj_check_type(t, &lv_label_class)) {
                        lv_obj_set_style_text_color(t, UI_TEXT, 0);
                    }
                }
            }
        }
        if (s_conn_actions) {
            lv_obj_set_style_bg_color(s_conn_actions, UI_PANEL, 0);
            lv_obj_set_style_border_color(s_conn_actions, UI_BORDER, 0);
            if (lv_obj_get_child_count(s_conn_actions) > 0) {
                lv_obj_t *row = lv_obj_get_child(s_conn_actions, 0);
                if (row && lv_obj_get_child_count(row) >= 2) {
                    lv_obj_t *cancel = lv_obj_get_child(row, 0);
                    lv_obj_t *save = lv_obj_get_child(row, 1);
                    lv_obj_set_style_bg_color(cancel, connect_square_layout() ? UI_SURFACE : UI_OFFLINE, 0);
                    lv_obj_set_style_border_color(cancel, UI_TILE_OFF_BORDER, 0);
                    if (lv_obj_t *cl = lv_obj_get_child(cancel, 0)) {
                        lv_obj_set_style_text_color(cl, UI_TEXT_SUB, 0);
                    }
                    lv_obj_set_style_bg_color(save, UI_ACCENT, 0);
                    lv_obj_set_style_bg_color(save, UI_ACCENT_PRESSED, LV_STATE_PRESSED);
                    if (lv_obj_t *sl = lv_obj_get_child(save, 0)) {
                        lv_obj_set_style_text_color(sl, UI_ON_TILE, 0);
                    }
                }
            }
        }
        paint_connect_action(s_scan_btn_lbl);
        paint_connect_action(s_proto_lbl);
        rebuild_server_hist_chips();
    }
    if (!s_kb) {
        return;
    }
    lv_obj_t *ta = lv_keyboard_get_textarea(s_kb);
    const bool vis = !lv_obj_has_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(s_kb);
    lv_obj_delete(s_kb);
    s_kb = nullptr;
    ensure_kb();
    if (!s_kb) {
        return;
    }
    lv_keyboard_set_mode(s_kb, mode);
    if (ta) {
        lv_keyboard_set_textarea(s_kb, ta);
    }
    if (vis) {
        nl_ov_kb_set_visible(true);
    }
}

void nl_ov_do_factory_reset(void)
{
    nodelio_nvs_config_t cfg{};
    s_cfg = cfg;
    nodelio_nvs_cache_put(&cfg);
    (void)nodelio_layout_cache_clear();
    (void)nodelio_apps_cache_clear();
    ESP_LOGW(TAG, "factory reset — reboot");
    nl_ov_toast("已清空，重启…");
    Application::GetInstance().Schedule([cfg]() {
        (void)nodelio_nvs_save(&cfg);
        SsidManager::GetInstance().Clear(); /* 清小智 WiFi，重启后进 SoftAP */
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    });
}
