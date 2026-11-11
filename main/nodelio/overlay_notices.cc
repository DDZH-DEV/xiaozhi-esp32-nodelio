#include "overlay_priv.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include <esp_log.h>
#include <lvgl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "nl_notices"

static lv_obj_t *s_notice_badge = nullptr;
static lv_obj_t *s_notice_badge_lbl = nullptr;
static lv_obj_t *s_notice_scrim = nullptr;
static lv_obj_t *s_notice_card = nullptr;
static lv_obj_t *s_notice_hdr = nullptr;
static lv_obj_t *s_notice_title_lbl = nullptr;
static lv_obj_t *s_notice_clear_btn = nullptr;
static lv_obj_t *s_notice_close_btn = nullptr;
static lv_obj_t *s_notice_list = nullptr;

static void fill_notice_rows(void);

static bool any_critical_or_alert(int n)
{
    for (int i = 0; i < n; ++i) {
        const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(i);
        if (!it) {
            continue;
        }
        if (strcmp(it->severity, "critical") == 0 || strcmp(it->mode, "alert") == 0) {
            return true;
        }
    }
    return false;
}

static bool any_action_or_warn(int n)
{
    for (int i = 0; i < n; ++i) {
        const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(i);
        if (!it) {
            continue;
        }
        if (strcmp(it->mode, "action") == 0 || strcmp(it->severity, "warning") == 0) {
            return true;
        }
    }
    return false;
}

static void on_badge_click(lv_event_t *e)
{
    (void)e;
    Board::GetInstance().NoteUserActivity();
    nl_ov_open_notices();
}

static void ensure_notice_badge(void)
{
    if (s_notice_badge) {
        return;
    }
    /* 挂状态栏右侧（时间旁），避免顶中漂浮压住设置/应用卡。 */
    lv_obj_t *parent = s_status_bar ? s_status_bar : lv_layer_top();
    s_notice_badge = lv_button_create(parent);
    if (!s_notice_badge) {
        return;
    }
    lv_obj_remove_style_all(s_notice_badge);
    if (parent == lv_layer_top()) {
        lv_obj_add_flag(s_notice_badge, LV_OBJ_FLAG_FLOATING);
    }
    lv_obj_add_flag(s_notice_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_notice_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_notice_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(s_notice_badge, 12);
    lv_obj_set_size(s_notice_badge, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_pad_hor(s_notice_badge, 8, 0);
    lv_obj_set_style_pad_ver(s_notice_badge, 2, 0);
    lv_obj_set_style_radius(s_notice_badge, 12, 0);
    lv_obj_set_style_bg_opa(s_notice_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_notice_badge, 0, 0);
    lv_obj_set_flex_flow(s_notice_badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notice_badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_notice_badge, on_badge_click, LV_EVENT_CLICKED, nullptr);

    s_notice_badge_lbl = lv_label_create(s_notice_badge);
    nl_ov_set_font_compact(s_notice_badge_lbl);
    lv_obj_set_style_text_color(s_notice_badge_lbl, UI_ON_TILE, 0);
}

lv_obj_t *nl_ov_notices_badge(void)
{
    return s_notice_badge;
}

void nl_ov_notices_refresh_badge(void)
{
    if (!s_ready) {
        return;
    }
    ensure_notice_badge();
    if (!s_notice_badge || !s_notice_badge_lbl) {
        return;
    }
    const int n = nodelio_panel_ui_notice_unread_count();
    if (n <= 0 || !s_visible || !s_screen_on) {
        lv_obj_add_flag(s_notice_badge, LV_OBJ_FLAG_HIDDEN);
        nl_ov_status_bar_relayout();
    } else {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "未读 %d", n);
        lv_label_set_text(s_notice_badge_lbl, buf);
        nl_ov_set_font_compact(s_notice_badge_lbl);
        lv_color_t bg = UI_ACCENT;
        if (any_critical_or_alert(n)) {
            bg = UI_DANGER;
        } else if (any_action_or_warn(n)) {
            bg = UI_WARN;
        }
        lv_obj_set_style_bg_color(s_notice_badge, bg, 0);
        lv_obj_clear_flag(s_notice_badge, LV_OBJ_FLAG_HIDDEN);
        /* 有未读时亮一下状态栏，避免角标随顶栏自动隐藏后看不见。 */
        if (s_status_bar) {
            lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_status_bar);
        }
        nl_ov_status_bar_relayout();
    }
    if (nl_ov_notices_visible()) {
        fill_notice_rows();
    }
}

static void on_scrim_click(lv_event_t *e)
{
    (void)e;
    Board::GetInstance().NoteUserActivity();
    nl_ov_close_notices();
}

static void on_clear_all_click(lv_event_t *e)
{
    (void)e;
    Board::GetInstance().NoteUserActivity();
    nodelio_panel_ui_notice_resolve_all();
    nl_ov_close_notices();
}

static void on_row_open_click(lv_event_t *e)
{
    const char *nid = static_cast<const char *>(lv_event_get_user_data(e));
    if (!nid || !nid[0]) {
        return;
    }
    char id_copy[40];
    std::snprintf(id_copy, sizeof(id_copy), "%s", nid);
    Board::GetInstance().NoteUserActivity();
    nl_ov_close_notices();
    nodelio_panel_ui_notice_resolve(id_copy, true);
}

static void on_row_dismiss_click(lv_event_t *e)
{
    const char *nid = static_cast<const char *>(lv_event_get_user_data(e));
    if (!nid || !nid[0]) {
        return;
    }
    char id_copy[40];
    std::snprintf(id_copy, sizeof(id_copy), "%s", nid);
    Board::GetInstance().NoteUserActivity();
    nodelio_panel_ui_notice_resolve(id_copy, false);
}

static void layout_notice_card(void)
{
    if (!s_notice_card) {
        return;
    }
    const auto *p = nodelio_display_profile();
    const int w = p->nav_width ? (int)p->result_width : ((int)p->width - 24);
    /* 顶栏已不占位，不要再空出 NL_UI_STATUS_H，否则卡片悬在屏顶下面。 */
    const int top = 12;
    int h = (int)p->height - top - 12;
    if (h < 160) {
        h = (int)p->height - 8;
    }
    lv_obj_set_size(s_notice_card, w, h);
    if (p->nav_width) {
        lv_obj_align(s_notice_card, LV_ALIGN_TOP_RIGHT, -12, top);
    } else {
        lv_obj_align(s_notice_card, LV_ALIGN_TOP_MID, 0, top);
    }
}

static void style_notice_chrome(void)
{
    if (!s_notice_card) {
        return;
    }
    lv_obj_set_style_bg_color(s_notice_card, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_notice_card, UI_BORDER, 0);
    lv_obj_set_style_radius(s_notice_card, UI_RADIUS_MD, 0);
    if (s_notice_title_lbl) {
        nl_ov_set_font_title(s_notice_title_lbl);
        lv_obj_set_style_text_color(s_notice_title_lbl, UI_TEXT, 0);
    }
    if (s_notice_clear_btn) {
        lv_obj_set_style_bg_color(s_notice_clear_btn, UI_SURFACE, 0);
        lv_obj_set_style_border_color(s_notice_clear_btn, UI_TILE_OFF_BORDER, 0);
        if (lv_obj_t *lbl = lv_obj_get_child(s_notice_clear_btn, 0)) {
            nl_ov_set_font_compact(lbl);
            lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
        }
    }
    if (s_notice_close_btn) {
        lv_obj_set_style_bg_color(s_notice_close_btn, UI_SURFACE, 0);
        lv_obj_set_style_border_color(s_notice_close_btn, UI_TILE_OFF_BORDER, 0);
        if (lv_obj_t *lbl = lv_obj_get_child(s_notice_close_btn, 0)) {
            nl_ov_set_icon_font(lbl);
            lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
        }
    }
}

static void ensure_notice_modal(void)
{
    if (s_notice_card) {
        return;
    }
    lv_obj_t *top = lv_layer_top();
    s_notice_scrim = lv_obj_create(top);
    lv_obj_remove_style_all(s_notice_scrim);
    lv_obj_set_size(s_notice_scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_notice_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_notice_scrim, 165, 0);
    lv_obj_add_flag(s_notice_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_notice_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_notice_scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_notice_scrim, on_scrim_click, LV_EVENT_CLICKED, nullptr);

    s_notice_card = lv_obj_create(top);
    lv_obj_remove_style_all(s_notice_card);
    lv_obj_add_flag(s_notice_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_notice_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_notice_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_notice_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_notice_card, 1, 0);
    lv_obj_set_style_border_opa(s_notice_card, 200, 0);
    lv_obj_set_style_pad_all(s_notice_card, 14, 0);
    lv_obj_set_style_pad_row(s_notice_card, 10, 0);
    lv_obj_set_flex_flow(s_notice_card, LV_FLEX_FLOW_COLUMN);

    s_notice_hdr = lv_obj_create(s_notice_card);
    lv_obj_remove_style_all(s_notice_hdr);
    lv_obj_set_size(s_notice_hdr, lv_pct(100), 42);
    lv_obj_set_flex_flow(s_notice_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notice_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_notice_hdr, LV_OBJ_FLAG_SCROLLABLE);

    s_notice_title_lbl = lv_label_create(s_notice_hdr);
    lv_label_set_text(s_notice_title_lbl, "消息提醒");

    lv_obj_t *act_wrap = lv_obj_create(s_notice_hdr);
    lv_obj_remove_style_all(act_wrap);
    lv_obj_set_size(act_wrap, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(act_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_wrap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act_wrap, 8, 0);
    lv_obj_clear_flag(act_wrap, LV_OBJ_FLAG_SCROLLABLE);

    s_notice_clear_btn = lv_button_create(act_wrap);
    lv_obj_remove_style_all(s_notice_clear_btn);
    lv_obj_set_size(s_notice_clear_btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(s_notice_clear_btn, 12, 0);
    lv_obj_set_style_radius(s_notice_clear_btn, 18, 0);
    lv_obj_set_style_bg_opa(s_notice_clear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_notice_clear_btn, 1, 0);
    lv_obj_set_flex_flow(s_notice_clear_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notice_clear_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_notice_clear_btn, on_clear_all_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *clr_lbl = lv_label_create(s_notice_clear_btn);
    lv_label_set_text(clr_lbl, "全部已读");

    s_notice_close_btn = lv_button_create(act_wrap);
    lv_obj_remove_style_all(s_notice_close_btn);
    lv_obj_set_size(s_notice_close_btn, 38, 38);
    lv_obj_set_style_radius(s_notice_close_btn, 19, 0);
    lv_obj_set_style_bg_opa(s_notice_close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_notice_close_btn, 1, 0);
    lv_obj_set_flex_flow(s_notice_close_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notice_close_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(s_notice_close_btn, on_scrim_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cls_lbl = lv_label_create(s_notice_close_btn);
    lv_label_set_text(cls_lbl, LV_SYMBOL_CLOSE);

    s_notice_list = lv_obj_create(s_notice_card);
    lv_obj_remove_style_all(s_notice_list);
    lv_obj_set_width(s_notice_list, lv_pct(100));
    lv_obj_set_flex_grow(s_notice_list, 1);
    lv_obj_set_flex_flow(s_notice_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_notice_list, 8, 0);
    lv_obj_add_flag(s_notice_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_notice_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_notice_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_notice_list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    layout_notice_card();
    style_notice_chrome();
}

static const char *mode_tag_text(const nodelio_panel_notice_t *it, lv_color_t *out_color)
{
    if (!it) {
        if (out_color) *out_color = UI_ACCENT;
        return "通知";
    }
    if (strcmp(it->mode, "alert") == 0 || strcmp(it->severity, "critical") == 0) {
        if (out_color) *out_color = UI_DANGER;
        return "告警";
    }
    if (strcmp(it->mode, "action") == 0 || strcmp(it->severity, "warning") == 0) {
        if (out_color) *out_color = UI_WARN;
        return "待确认";
    }
    if (out_color) *out_color = UI_ACCENT;
    return "通知";
}

static void fill_notice_rows(void)
{
    if (!s_notice_list) {
        return;
    }
    lv_obj_clean(s_notice_list);
    const int n = nodelio_panel_ui_notice_unread_count();
    if (s_notice_title_lbl) {
        char hbuf[48];
        if (n > 0) {
            std::snprintf(hbuf, sizeof(hbuf), "消息提醒 (%d)", n);
        } else {
            std::snprintf(hbuf, sizeof(hbuf), "消息提醒");
        }
        lv_label_set_text(s_notice_title_lbl, hbuf);
    }
    if (s_notice_clear_btn) {
        if (n > 0) {
            lv_obj_clear_flag(s_notice_clear_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_notice_clear_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (n <= 0) {
        lv_obj_t *empty = lv_label_create(s_notice_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "暂无未读消息");
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
        lv_obj_set_style_pad_top(empty, 24, 0);
        return;
    }

    for (int i = 0; i < n; ++i) {
        const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(i);
        if (!it) {
            continue;
        }
        lv_color_t tag_color = UI_ACCENT;
        const char *tag_str = mode_tag_text(it, &tag_color);
        const bool can_open = it->invoke_app[0] != '\0' || it->app_id[0] != '\0';

        lv_obj_t *row = lv_obj_create(s_notice_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 62, 0);
        lv_obj_set_style_bg_color(row, UI_TILE_OFF, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UI_RADIUS_MD, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, tag_color, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (can_open) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, on_row_open_click, LV_EVENT_CLICKED,
                                const_cast<char *>(it->notice_id));
        }

        lv_obj_t *left = lv_obj_create(row);
        lv_obj_remove_style_all(left);
        lv_obj_set_height(left, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 4, 0);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(left, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *meta = lv_label_create(left);
        nl_ov_set_font_compact(meta);
        char mtxt[96];
        if (it->app_id[0]) {
            std::snprintf(mtxt, sizeof(mtxt), "[%s] %s", tag_str, it->app_id);
        } else {
            std::snprintf(mtxt, sizeof(mtxt), "[%s]", tag_str);
        }
        lv_label_set_text(meta, mtxt);
        lv_obj_set_style_text_color(meta, tag_color, 0);

        lv_obj_t *ttl = lv_label_create(left);
        nl_ov_set_font(ttl);
        lv_obj_set_width(ttl, lv_pct(100));
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_WRAP);
        lv_label_set_text(ttl, it->title[0] ? it->title : "新通知");
        lv_obj_set_style_text_color(ttl, UI_TEXT, 0);

        lv_obj_t *btns = lv_obj_create(row);
        lv_obj_remove_style_all(btns);
        lv_obj_set_size(btns, LV_SIZE_CONTENT, 36);
        lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btns, 6, 0);
        lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

        if (can_open) {
            lv_obj_t *open_btn = lv_button_create(btns);
            lv_obj_remove_style_all(open_btn);
            lv_obj_set_size(open_btn, LV_SIZE_CONTENT, 34);
            lv_obj_set_style_pad_hor(open_btn, 12, 0);
            lv_obj_set_style_radius(open_btn, 17, 0);
            lv_obj_set_style_bg_color(open_btn, tag_color, 0);
            lv_obj_set_style_bg_opa(open_btn, LV_OPA_COVER, 0);
            lv_obj_set_flex_flow(open_btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(open_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_add_event_cb(open_btn, on_row_open_click, LV_EVENT_CLICKED,
                                const_cast<char *>(it->notice_id));
            lv_obj_t *ol = lv_label_create(open_btn);
            nl_ov_set_font_compact(ol);
            lv_label_set_text(ol, "查看");
            lv_obj_set_style_text_color(ol, UI_ON_TILE, 0);
        }

        lv_obj_t *dis_btn = lv_button_create(btns);
        lv_obj_remove_style_all(dis_btn);
        lv_obj_set_size(dis_btn, 34, 34);
        lv_obj_set_style_radius(dis_btn, 17, 0);
        lv_obj_set_style_bg_color(dis_btn, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(dis_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dis_btn, 1, 0);
        lv_obj_set_style_border_color(dis_btn, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_flex_flow(dis_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dis_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(dis_btn, on_row_dismiss_click, LV_EVENT_CLICKED,
                            const_cast<char *>(it->notice_id));
        lv_obj_t *dl = lv_label_create(dis_btn);
        nl_ov_set_icon_font(dl);
        lv_label_set_text(dl, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(dl, UI_TEXT_SUB, 0);
    }
}

void nl_ov_open_notices(void)
{
    ensure_notice_modal();
    if (!s_notice_card || !s_notice_scrim) {
        return;
    }
    layout_notice_card();
    style_notice_chrome();
    fill_notice_rows();
    lv_obj_clear_flag(s_notice_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_notice_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_notice_scrim);
    lv_obj_move_foreground(s_notice_card);
    nl_ov_raise_chrome();
}

void nl_ov_close_notices(void)
{
    if (s_notice_scrim) {
        lv_obj_add_flag(s_notice_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_notice_card) {
        lv_obj_add_flag(s_notice_card, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_notices_refresh_badge();
}

bool nl_ov_notices_visible(void)
{
    return s_notice_card && !lv_obj_has_flag(s_notice_card, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *nl_ov_notices_scroll_obj(void)
{
    if (!nl_ov_notices_visible() || !s_notice_list) {
        return nullptr;
    }
    return s_notice_list;
}

void nl_ov_notices_reapply_appearance(void)
{
    style_notice_chrome();
    nl_ov_notices_refresh_badge();
}

void nl_ov_notices_reapply_text_scale(void)
{
    style_notice_chrome();
    nl_ov_notices_refresh_badge();
}

