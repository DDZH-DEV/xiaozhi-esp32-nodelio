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

static void on_wifi_scan_btn(lv_event_t *e);

static void style_ta(lv_obj_t *ta)
{
    lv_obj_set_height(ta, 40);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_cursor_click_pos(ta, true);
    lv_obj_set_style_bg_color(ta, UI_FIELD, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, UI_BORDER, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_pad_hor(ta, 10, 0);
    lv_obj_set_style_pad_ver(ta, 8, 0);
    nl_ov_set_font(ta);
    lv_obj_set_style_text_color(ta, UI_TEXT, 0);
    lv_obj_set_style_text_font(ta, nl_ov_ui_font(), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_color(ta, UI_TEXT_MUTED, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, UI_FIELD_FOCUS, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, UI_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
}

static void add_field_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    nl_ov_set_font(l);
    lv_obj_set_style_text_color(l, UI_TEXT_DIM, 0);
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

/** 键盘弹出时加底垫，才能把绑定码等滚到键盘上方 */
static void connect_scroll_kb_pad(bool kb_on)
{
    if (!s_connect_scroll) {
        return;
    }
    lv_obj_set_style_pad_bottom(s_connect_scroll, kb_on ? 220 : NL_UI_SETTINGS_DETAIL_PAD, 0);
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
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        if (s_tabbar) {
            lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
        }
        connect_scroll_kb_pad(true);
        lv_obj_move_foreground(s_kb);
    } else {
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_kb, nullptr);
        connect_scroll_kb_pad(false);
        bool form_open = s_connect && !lv_obj_has_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
        if (s_tabbar && s_visible && !form_open) {
            lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ensure_kb(void)
{
    if (s_kb) {
        return;
    }
    /* 连接页在 lv_layer_top()，键盘必须同层才能盖在表单上面 */
    s_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(s_kb, lv_pct(100));
    lv_obj_set_height(s_kb, lv_pct(42));
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(s_kb, 6, 0);
    lv_obj_set_style_pad_row(s_kb, 6, 0);
    lv_obj_set_style_pad_column(s_kb, 6, 0);
    lv_obj_set_style_min_height(s_kb, 36, LV_PART_ITEMS);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_USER_1, s_kb_map_host, s_kb_ctrl_host);
    lv_keyboard_set_map(s_kb, LV_KEYBOARD_MODE_USER_2, s_kb_map_port, s_kb_ctrl_port);
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

static void on_save(lv_event_t *e)
{
    (void)e;
    const char *ssid = s_ta_ssid ? lv_textarea_get_text(s_ta_ssid) : "";
    const char *pass = s_ta_pass ? lv_textarea_get_text(s_ta_pass) : "";
    const char *host = s_ta_host ? lv_textarea_get_text(s_ta_host) : "";
    const char *port = s_ta_port ? lv_textarea_get_text(s_ta_port) : "8899";
    const char *owner = s_ta_owner ? lv_textarea_get_text(s_ta_owner) : "";
    if (!host || !host[0] || !owner || !owner[0]) {
        nl_ov_toast("请填写主机与绑定码");
        return;
    }
    if (ssid && ssid[0] && (!pass || !pass[0])) {
        const auto &list = SsidManager::GetInstance().GetSsidList();
        bool known = false;
        for (const auto &it : list) {
            if (it.ssid == ssid) {
                known = true;
                break;
            }
        }
        if (!known) {
            nl_ov_toast("新网络请填密码");
            return;
        }
    }
    if (!port || !port[0]) {
        port = "8899";
    }

    /* LVGL 栈在 PSRAM，禁止此处 flash / WiFi NVS；拷贝表单后主任务落盘重启 */
    std::string ssid_s = ssid ? ssid : "";
    std::string pass_s = pass ? pass : "";
    std::string host_s = host ? host : "";
    std::string port_s = port ? port : "8899";
    std::string owner_s = owner ? owner : "";
    const bool https = s_https;
    nodelio_nvs_config_t cfg = s_cfg;
    char new_url[sizeof(cfg.server_url)];
    std::snprintf(new_url, sizeof(new_url), "%s://%s:%s", https ? "https" : "http", host_s.c_str(),
                  port_s.c_str());
    bool server_changed = std::strcmp(cfg.server_url, new_url) != 0;
    std::snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", new_url);
    std::snprintf(cfg.owner, sizeof(cfg.owner), "%s", owner_s.c_str());
    if (ssid_s[0]) {
        std::snprintf(cfg.wifi_ssid, sizeof(cfg.wifi_ssid), "%s", ssid_s.c_str());
        if (pass_s[0]) {
            std::snprintf(cfg.wifi_pass, sizeof(cfg.wifi_pass), "%s", pass_s.c_str());
        }
    }
    if (server_changed) {
        nodelio_nvs_clear_mqtt(&cfg);
    }
    s_cfg = cfg;
    nodelio_nvs_cache_put(&cfg);
    nl_ov_toast(server_changed ? "已保存，将重新注册…" : "已保存，重启…");
    Application::GetInstance().Schedule([cfg, ssid_s, pass_s]() mutable {
        nodelio_nvs_config_t out = cfg;
        apply_wifi_creds(ssid_s.c_str(), pass_s.c_str(), &out);
        (void)nodelio_nvs_save(&out);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    });
}

void nl_ov_open_connect(void)
{
    if (!s_connect) {
        return;
    }
    nl_ov_close_settings_detail();
    /* s_cfg 开机已加载；禁止在 LVGL 任务 nvs_load */
    char host[96] = {0};
    char port[8] = {0};
    nl_url_parse_server(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

    std::string cur = WifiManager::GetInstance().GetSsid();
    if (cur.empty()) {
        const auto &list = SsidManager::GetInstance().GetSsidList();
        if (!list.empty()) {
            cur = list[0].ssid;
        }
    }
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
    if (s_proto_lbl) {
        lv_label_set_text(s_proto_lbl, proto_btn_text());
    }
    if (s_scan_btn_lbl && !s_wifi_scanning) {
        lv_label_set_text(s_scan_btn_lbl, "扫描");
    }
    nl_ov_kb_set_visible(false);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
    if (s_tabbar) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    /* 与 s_root 同层；apply_visible_locked 会把 root 置顶，此处必须再抬到前面 */
    lv_obj_move_foreground(s_connect);
}


void nl_ov_build_connect(void)
{
    /* 挂在 lv_layer_top() 而非 s_root flex 容器，避免 move_foreground 打乱 flex 顺序导致状态栏移位 */
    s_connect = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_connect, LV_HOR_RES, LV_VER_RES - NL_UI_STATUS_H);
    lv_obj_align(s_connect, LV_ALIGN_TOP_LEFT, 0, NL_UI_STATUS_H);
    lv_obj_set_style_bg_color(s_connect, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_connect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_connect, 0, 0);
    lv_obj_set_style_pad_all(s_connect, 0, 0);
    lv_obj_set_flex_flow(s_connect, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_connect, LV_OBJ_FLAG_HIDDEN);

    (void)ui_make_nav_bar(s_connect, UI_NAV_H, "连接",
                          [](lv_event_t *) { nl_ov_close_connect(); });

    s_connect_scroll = lv_obj_create(s_connect);
    lv_obj_set_width(s_connect_scroll, lv_pct(100));
    lv_obj_set_flex_grow(s_connect_scroll, 1);
    lv_obj_set_style_bg_opa(s_connect_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_connect_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_connect_scroll, NL_UI_CONNECT_PAD_H, 0);
    lv_obj_set_style_pad_right(s_connect_scroll, NL_UI_CONNECT_PAD_H, 0);
    lv_obj_set_style_pad_top(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(s_connect_scroll, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_row(s_connect_scroll, 6, 0);
    lv_obj_set_flex_flow(s_connect_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_connect_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_connect_scroll, LV_DIR_VER);

    (void)nodelio_nvs_load(&s_cfg);
    char host[96] = {0};
    char port[8] = {0};
    nl_url_parse_server(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

    /* WiFi：扫描选网，不必手填 SSID */
    add_field_label(s_connect_scroll, "WiFi 名称");
    lv_obj_t *ssid_row = lv_obj_create(s_connect_scroll);
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

    lv_obj_t *scan_btn = ui_make_action_btn(ssid_row, "扫描", 80, 40, on_wifi_scan_btn);
    s_scan_btn_lbl = lv_obj_get_child(scan_btn, 0);

    s_wifi_list = lv_obj_create(s_connect_scroll);
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
    lv_obj_add_flag(s_wifi_list, LV_OBJ_FLAG_HIDDEN);
    s_wifi_scanning = false;
    s_scan_n = 0;

    add_field_label(s_connect_scroll, "WiFi 密码（已存网络可留空）");
    s_ta_pass = lv_textarea_create(s_connect_scroll);
    style_ta(s_ta_pass);
    lv_obj_set_width(s_ta_pass, lv_pct(100));
    lv_textarea_set_password_mode(s_ta_pass, true);
    lv_textarea_set_max_length(s_ta_pass, 64);
    lv_textarea_set_placeholder_text(s_ta_pass, "输入密码");
    wire_ta_input(s_ta_pass);

    add_field_label(s_connect_scroll, "服务器");
    lv_obj_t *proto_row = lv_obj_create(s_connect_scroll);
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
        proto_row, proto_btn_text(), 88, UI_BTN_H_SM,
        [](lv_event_t *) {
            s_https = !s_https;
            if (s_proto_lbl) {
                lv_label_set_text(s_proto_lbl, proto_btn_text());
            }
        });
    s_proto_lbl = lv_obj_get_child(proto_btn, 0);

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
    lv_obj_set_width(s_ta_port, 80);
    lv_textarea_set_max_length(s_ta_port, 5);
    lv_textarea_set_text(s_ta_port, port);
    lv_textarea_set_accepted_chars(s_ta_port, "0123456789");
    wire_ta_input(s_ta_port);

    add_field_label(s_connect_scroll, "绑定码");
    s_ta_owner = lv_textarea_create(s_connect_scroll);
    style_ta(s_ta_owner);
    lv_obj_set_width(s_ta_owner, lv_pct(100));
    lv_textarea_set_max_length(s_ta_owner, sizeof(s_cfg.owner) - 1);
    lv_textarea_set_text(s_ta_owner, s_cfg.owner);
    wire_ta_input(s_ta_owner);

    lv_obj_t *btn_row = lv_obj_create(s_connect_scroll);
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
    lv_obj_set_height(cancel, UI_BTN_H_SM);
    lv_obj_set_ext_click_area(cancel, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(cancel, UI_OFFLINE, 0);
    lv_obj_set_style_radius(cancel, UI_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { nl_ov_close_connect(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    nl_ov_set_font(cl);
    lv_obj_set_style_text_color(cl, UI_TEXT_SUB, 0);
    lv_obj_center(cl);

    lv_obj_t *save = lv_button_create(btn_row);
    lv_obj_set_flex_grow(save, 1);
    lv_obj_set_height(save, UI_BTN_H_SM);
    lv_obj_set_ext_click_area(save, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(save, UI_BACK_BTN, 0);
    lv_obj_set_style_bg_color(save, UI_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, UI_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, "保存并重启");
    nl_ov_set_font(sl);
    lv_obj_set_style_text_color(sl, UI_TEXT, 0);
    lv_obj_center(sl);

    lv_obj_t *tip = lv_label_create(s_connect_scroll);
    lv_label_set_text(tip, "先点「扫描」选 WiFi，再填密码与服务器；主机填局域网 IP");
    nl_ov_set_font(tip);
    lv_obj_set_style_text_color(tip, UI_TEXT_TIP, 0);
    lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tip, lv_pct(100));
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
    bool restore_station = false;

    /*
     * SoftAP / Station 都会在 SCAN_DONE 里 esp_wifi_scan_get_ap_records，
     * 把结果抢走 → overlay 常见 scan got 0。屏上扫描前先关掉它们。
     */
    if (wifi.IsConfigMode()) {
        /* 兜底：若仍残留 SoftAP，先关掉再扫 */
        nodelio_wifi_set_suppress_softap(true);
        wifi.StopConfigAp();
        vTaskDelay(pdMS_TO_TICKS(400));
    } else {
        restore_station = true;
        wifi.StopStation();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    int n = 0;
    esp_err_t err = nodelio_wifi_scan(s_scan_aps, NL_WIFI_SCAN_MAX, &n);
    s_scan_n = (err == ESP_OK) ? n : 0;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan: %s", esp_err_to_name(err));
    }

    if (restore_station) {
        nodelio_wifi_release_scan_netif();
        wifi.StartStation();
    }

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

void nl_ov_do_factory_reset(void)
{
    nodelio_nvs_config_t cfg{};
    s_cfg = cfg;
    nodelio_nvs_cache_put(&cfg);
    (void)nodelio_layout_cache_clear();
    ESP_LOGW(TAG, "factory reset — reboot");
    nl_ov_toast("已清空，重启…");
    Application::GetInstance().Schedule([cfg]() {
        (void)nodelio_nvs_save(&cfg);
        SsidManager::GetInstance().Clear(); /* 清小智 WiFi，重启后进 SoftAP */
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    });
}
