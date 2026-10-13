#include "nodelio_overlay.h"

#include "board_info.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_provision.h"
#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "display.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_manager.h"

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_overlay"

#define GRID_GAP UI_GAP

typedef enum {
    TILE_SWITCH = 0,
    TILE_SCENE,
    TILE_GPIO,
    TILE_PWM,
} tile_kind_t;

static nodelio_nvs_config_t s_cfg;
static nodelio_panel_layout_t *s_layout;
static bool s_have_layout;
static bool s_fab_ready;
static bool s_ready;
static bool s_visible;
static bool s_building;
static bool s_want_settings;
static bool s_want_chat;
static bool s_https;
static bool s_wifi_ok;
static bool s_cloud_ok;

static lv_obj_t *s_fab;
static lv_obj_t *s_root;
static lv_obj_t *s_body;
static lv_obj_t *s_home;
static lv_obj_t *s_chat;
static lv_obj_t *s_phase_lbl; /* 上区：聆听/说话/待命 */
static lv_obj_t *s_eq_bars[3];
static lv_obj_t *s_chat_band;   /* 下区可滚容器 */
static lv_obj_t *s_chat_lbl;
static lv_obj_t *s_settings;
static lv_obj_t *s_tabbar;
static lv_obj_t *s_tab_btns[3];
static lv_obj_t *s_brand_lbl;
static lv_obj_t *s_clock_lbl;
static lv_obj_t *s_status_lbl;
static lv_obj_t *s_wifi_lbl;
static lv_obj_t *s_cloud_lbl;
static lv_obj_t *s_status_bar;
static esp_timer_handle_t s_status_tmr;
static lv_timer_t *s_activity_tmr;
static char s_clock_last[8];
static bool s_screen_on = true;
static nodelio_panel_ui_screen_cb_t s_screen_cb;

static lv_obj_t *s_settings_detail;
static lv_obj_t *s_bri_val_lbl;
static lv_obj_t *s_vol_val_lbl;
static uint8_t s_brightness = 80;
static uint8_t s_volume = 70;
static lv_timer_t *s_sys_timer;
static lv_obj_t *s_sys_up_lbl;
static lv_obj_t *s_sys_dram_lbl;
static lv_obj_t *s_sys_psram_lbl;
static lv_obj_t *s_sys_tasks_lbl;
static char s_sys_last_up[40];
static char s_sys_last_dram[24];
static char s_sys_last_psram[24];
static char s_sys_last_tasks[16];

static lv_obj_t *s_connect;
static lv_obj_t *s_connect_scroll;
static lv_obj_t *s_kb;
static lv_obj_t *s_ta_host;
static lv_obj_t *s_ta_port;
static lv_obj_t *s_ta_owner;
static lv_obj_t *s_proto_lbl;
static lv_obj_t *s_ta_ssid;
static lv_obj_t *s_ta_pass;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_scan_btn_lbl;
static nodelio_wifi_ap_t s_scan_aps[NL_WIFI_SCAN_MAX];
static int s_scan_n;
static bool s_wifi_scanning;
static int s_grid_ox;
static int s_grid_oy;
static int s_active_tab; /* 0 home 1 chat 2 settings */

enum chat_phase_t : uint8_t { CHAT_IDLE = 0, CHAT_LISTEN, CHAT_SPEAK, CHAT_CONNECT };
static chat_phase_t s_chat_phase = CHAT_IDLE;
static char s_phase_text[32];
/* 对话历史：约 1.5～3KB 中文（原 640 太短，上下复读更糟） */
#define NL_CHAT_CAP 3072
static char s_chat_text[NL_CHAT_CAP];

static nl_dev_cache_t *s_devs = nullptr;
static int s_dev_n = 0;

static void ensure_devs_cache()
{
    if (!s_devs) {
        s_devs = (nl_dev_cache_t *)heap_caps_calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_devs) {
            s_devs = (nl_dev_cache_t *)calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t));
        }
    }
}
static lv_style_transition_dsc_t s_tile_trans;
static bool s_tile_trans_inited;

enum { TILE_CHILD_ICON = 1, TILE_CHILD_TITLE = 2, TILE_CHILD_HINT = 3, TILE_CHILD_CAPSULE = 4 };

static Display *disp()
{
    return Board::GetInstance().GetDisplay();
}

static void utf8_copy(char *dst, size_t dst_len, const char *src);

static const lv_font_t *ui_font()
{
    return &font_puhui_14_1;
}

static void set_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, ui_font(), 0);
    }
}

static void set_icon_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    }
}

/** 首页磁贴矢量图标字库（20px/30px 4bpp 原生抗锯齿，绝不用 transform_scale，防位图模糊与溢出） */
static const lv_font_t *tile_icon_font(bool is_large)
{
    return is_large ? &font_material_symbols_30_4 : &font_material_symbols_20_4;
}

/* 字体视觉重心偏上：统一 +1；右侧留白与左侧 12px 接近 */
#define NL_STATUS_Y 1
#define NL_STATUS_RIGHT_PAD (-18)

static void status_bar_relayout(void)
{
    if (!s_status_lbl || !s_cloud_lbl || !s_wifi_lbl) {
        return;
    }
    lv_obj_align(s_status_lbl, LV_ALIGN_RIGHT_MID, NL_STATUS_RIGHT_PAD, NL_STATUS_Y);
    lv_obj_align_to(s_cloud_lbl, s_status_lbl, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_align_to(s_wifi_lbl, s_cloud_lbl, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    if (s_clock_lbl) {
        lv_obj_align(s_clock_lbl, LV_ALIGN_CENTER, 0, NL_STATUS_Y);
    }
    if (s_brand_lbl) {
        lv_obj_align(s_brand_lbl, LV_ALIGN_LEFT_MID, 12, NL_STATUS_Y);
    }
}

static void refresh_clock_lbl(void)
{
    if (!s_clock_lbl) {
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);
    char buf[8];
    if (tm_now.tm_year >= (2025 - 1900)) {
        strftime(buf, sizeof(buf), "%H:%M", &tm_now);
    } else {
        std::snprintf(buf, sizeof(buf), "--:--");
    }
    if (strcmp(buf, s_clock_last) == 0) {
        return;
    }
    std::snprintf(s_clock_last, sizeof(s_clock_last), "%s", buf);
    lv_label_set_text(s_clock_lbl, s_clock_last);
}

static void refresh_status_icons(bool wifi_ok, bool cloud_ok, const char *msg)
{
    s_wifi_ok = wifi_ok;
    s_cloud_ok = cloud_ok;
    if (s_wifi_lbl) {
        lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI);
        set_icon_font(s_wifi_lbl);
        lv_obj_set_style_text_color(s_wifi_lbl, wifi_ok ? UI_ACCENT : UI_TEXT_MUTED, 0);
    }
    if (s_cloud_lbl) {
        /* 无独立云符号：UPLOAD≈云同步 */
        lv_label_set_text(s_cloud_lbl, LV_SYMBOL_UPLOAD);
        set_icon_font(s_cloud_lbl);
        lv_obj_set_style_text_color(s_cloud_lbl, cloud_ok ? UI_ACCENT : UI_TEXT_MUTED, 0);
    }
    if (s_status_lbl && msg) {
        lv_label_set_text(s_status_lbl, msg);
    }
    refresh_clock_lbl();
    status_bar_relayout();
}

static void status_poll_cb(void *arg)
{
    (void)arg;
    if (!s_ready || !s_screen_on) {
        return;
    }
    wifi_ap_record_t ap{};
    bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    bool cloud_ok = nodelio_mqtt_panel_connected();
    if (!cloud_ok && s_cfg.mqtt_server[0] == '\0') {
        /* 未配 MQTT 时用最近业务成功作「云」 */
        cloud_ok = s_cloud_ok;
    }
    char msg[20];
    std::snprintf(msg, sizeof(msg), "v%d",
                  (s_have_layout && s_layout) ? s_layout->layout_version : 0);
    nodelio_panel_ui_set_status(wifi_ok, cloud_ok, msg);
}

static void status_poll_set_period(bool awake)
{
    if (!s_status_tmr) {
        return;
    }
    esp_timer_stop(s_status_tmr);
    /* 亮屏 1s 刷时钟；息屏 15s 空转（回调内直接 return） */
    esp_timer_start_periodic(s_status_tmr, (uint64_t)(awake ? 1 : 15) * 1000 * 1000);
}

static void start_status_poll(void)
{
    if (s_status_tmr) {
        return;
    }
    esp_timer_create_args_t args = {
        .callback = status_poll_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nl_status",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_status_tmr) != ESP_OK) {
        return;
    }
    status_poll_set_period(true);
    status_poll_cb(nullptr);
}

static void toast(const char *msg)
{
    if (msg && msg[0] && disp()) {
        disp()->ShowNotification(msg, 2500);
    }
}

static void parse_server_url(const char *url, char *host, size_t host_len, char *port, size_t port_len,
                             bool *https)
{
    if (host_len) {
        host[0] = '\0';
    }
    if (port_len) {
        std::snprintf(port, port_len, "8899");
    }
    if (https) {
        *https = false;
    }
    if (!url || !url[0]) {
        return;
    }
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        if (https) {
            *https = true;
        }
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    const char *colon = nullptr;
    for (const char *q = p; *q && q != slash; q++) {
        if (*q == ':') {
            colon = q;
        }
    }
    size_t hn = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (hn >= host_len) {
        hn = host_len - 1;
    }
    memcpy(host, p, hn);
    host[hn] = '\0';
    if (colon && colon[1] && port_len) {
        size_t pn = 0;
        for (const char *q = colon + 1; *q && *q != '/' && pn + 1 < port_len; q++) {
            port[pn++] = *q;
        }
        port[pn] = '\0';
    }
}

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
    set_font(ta);
    lv_obj_set_style_text_color(ta, UI_TEXT, 0);
    lv_obj_set_style_text_font(ta, ui_font(), LV_PART_TEXTAREA_PLACEHOLDER);
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
    set_font(l);
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

static void kb_set_visible(bool on)
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
    if (s_kb || !s_root) {
        return;
    }
    s_kb = lv_keyboard_create(s_root);
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
            if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
                kb_set_visible(false);
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
    if (ta == s_ta_host) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_USER_1);
    } else if (ta == s_ta_port) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_USER_2);
    } else if (ta == s_ta_pass) {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    }
    kb_set_visible(true);
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

/* —— home grid —— */

static void short_label(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 4) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    size_t n = 0;
    size_t maxb = out_len - 1;
    if (maxb > 18) {
        maxb = 18;
    }
    while (in[n] && n < maxb) {
        unsigned char c = (unsigned char)in[n];
        size_t step = 1;
        if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (n + step > maxb) {
            break;
        }
        memcpy(out + n, in + n, step);
        n += step;
    }
    out[n] = '\0';
    if (in[n] != '\0' && n + 3 < out_len) {
        std::snprintf(out + n, out_len - n, "..");
    }
}

static tile_kind_t tile_kind_of(const nl_widget_t *w)
{
    if (w->type == NL_WIDGET_SLIDER_TILE ||
        (w->bind.kind == NL_BIND_DEVICE_PIN && strcmp(w->bind.capability, "pwm") == 0)) {
        return TILE_PWM;
    }
    if (w->type == NL_WIDGET_SCENE_CHIP || w->type == NL_WIDGET_SCENE_GRID ||
        w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return TILE_SCENE;
    }
    if (w->type == NL_WIDGET_PIN_TOGGLE || w->bind.kind == NL_BIND_DEVICE_PIN) {
        return TILE_GPIO;
    }
    return TILE_SWITCH;
}

static uint32_t kind_fill_on(tile_kind_t kind)
{
    switch (kind) {
    case TILE_SCENE:
        return UI_C_TILE_SCENE;
    case TILE_GPIO:
        return UI_C_TILE_GPIO;
    case TILE_PWM:
        return UI_C_TILE_PWM;
    default:
        return UI_C_TILE_SWITCH;
    }
}

static uint32_t kind_fill_off(tile_kind_t kind)
{
    (void)kind;
    /* 关态统一石板：不跟自定义底色，开/关一眼可辨 */
    return UI_C_TILE_OFF;
}

static const char *kind_symbol(const nl_widget_t *w, tile_kind_t kind, bool on)
{
    (void)on;
    if (w && w->skin[0]) {
        if (strcmp(w->skin, "fan") == 0) {
            return MATERIAL_SYMBOLS_REFRESH;
        }
        if (strcmp(w->skin, "dimmer") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        }
        if (strcmp(w->skin, "heater") == 0) {
            return MATERIAL_SYMBOLS_DEVICE_THERMOSTAT;
        }
        if (strcmp(w->skin, "momentary") == 0) {
            return MATERIAL_SYMBOLS_NOTIFICATIONS;
        }
        if (strcmp(w->skin, "dual_line") == 0) {
            return MATERIAL_SYMBOLS_REPEAT;
        }
        if (strcmp(w->skin, "gauge") == 0 || strcmp(w->skin, "tank") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        }
    }
    switch (kind) {
    case TILE_SCENE:
        return MATERIAL_SYMBOLS_PLAY_ARROW;
    case TILE_PWM:
        return MATERIAL_SYMBOLS_BRIGHTNESS_6;
    default:
        /* 灯具、继电器、插座等标准开关：统一使用高雅经典的 Power 电源键符号 */
        return MATERIAL_SYMBOLS_POWER_SETTINGS_NEW;
    }
}

static bool parse_bg_hex(const char *s, uint32_t *out)
{
    if (!s || !out || s[0] != '#') {
        return false;
    }
    uint32_t v = 0;
    for (int i = 1; i <= 6; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static uint32_t color_pressed(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xff) * 200 / 255;
    uint32_t g = ((c >> 8) & 0xff) * 200 / 255;
    uint32_t b = (c & 0xff) * 200 / 255;
    return (r << 16) | (g << 8) | b;
}

static uint32_t color_brighten(uint32_t c, uint32_t add)
{
    uint32_t r = ((c >> 16) & 0xff);
    uint32_t g = ((c >> 8) & 0xff);
    uint32_t b = (c & 0xff);
    r = (r + add > 255) ? 255 : (r + add);
    g = (g + add > 255) ? 255 : (g + add);
    b = (b + add > 255) ? 255 : (b + add);
    return (r << 16) | (g << 8) | b;
}

static nl_dev_cache_t *find_dev(const char *device_id)
{
    ensure_devs_cache();
    if (!s_devs || !device_id || !device_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_dev_n; i++) {
        if (strcmp(s_devs[i].id, device_id) == 0) {
            return &s_devs[i];
        }
    }
    return nullptr;
}

static nl_dev_cache_t *ensure_dev(const char *device_id)
{
    ensure_devs_cache();
    if (!s_devs) {
        return nullptr;
    }
    nl_dev_cache_t *d = find_dev(device_id);
    if (d) {
        return d;
    }
    if (!device_id || !device_id[0] || s_dev_n >= NL_DEV_CACHE_MAX) {
        return nullptr;
    }
    d = &s_devs[s_dev_n++];
    std::memset(d, 0, sizeof(*d));
    std::snprintf(d->id, sizeof(d->id), "%s", device_id);
    d->online = true;
    return d;
}

static bool state_is_on(const char *st)
{
    if (!st || !st[0]) {
        return false;
    }
    if (strcasecmp(st, "ON") == 0 || strcasecmp(st, "OPEN") == 0) {
        return true;
    }
    if (strcasecmp(st, "OFF") == 0 || strcasecmp(st, "CLOSE") == 0 ||
        strcasecmp(st, "UNKNOWN") == 0) {
        return false;
    }
    /* 自锁 "1" / PWM duty "1"…"100"；勿把裸 GPIO 高低与逻辑开关联想 */
    char *end = nullptr;
    long v = strtol(st, &end, 10);
    if (end != st && *end == '\0' && v > 0) {
        return true;
    }
    return false;
}

static int bind_channel_index(const nl_widget_t *w)
{
    if (!w) {
        return -1;
    }
    if (w->bind.kind == NL_BIND_DEVICE_CHANNEL) {
        return w->bind.channel;
    }
    if (w->bind.kind == NL_BIND_DEVICE_PIN) {
        return w->bind.pin >= 0 ? w->bind.pin : w->bind.channel;
    }
    return -1;
}

static void set_channel_state(nl_dev_cache_t *d, int ch, const char *st)
{
    if (!d || ch < 0 || !st) {
        return;
    }
    for (uint8_t i = 0; i < d->ch_n; i++) {
        if (d->ch_idx[i] == ch) {
            std::snprintf(d->ch_st[i], sizeof(d->ch_st[i]), "%s", st);
            return;
        }
    }
    if (d->ch_n < NL_CH_PER_DEV) {
        d->ch_idx[d->ch_n] = (int16_t)ch;
        std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", st);
        d->ch_n++;
    }
}

/** 通道/引脚只看对应通道态；勿回退整机 power（易把 UNKNOWN/其它路当成开）。 */
static bool widget_is_on(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w) {
        return false;
    }
    if (kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return true;
    }
    nl_dev_cache_t *d = find_dev(w->bind.device_id);
    if (!d) {
        return false;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == ch) {
                return state_is_on(d->ch_st[i]);
            }
        }
        return false;
    }
    return state_is_on(d->power);
}

static int widget_pwm_duty(const nl_widget_t *w)
{
    if (!w) {
        return 0;
    }
    nl_dev_cache_t *d = find_dev(w->bind.device_id);
    if (!d) {
        return 0;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == ch) {
                return (int)strtol(d->ch_st[i], nullptr, 10);
            }
        }
        return 0;
    }
    return (int)strtol(d->power, nullptr, 10);
}

static void set_widget_cache_state(const nl_widget_t *w, const char *st)
{
    if (!w || !st || w->bind.device_id[0] == '\0') {
        return;
    }
    nl_dev_cache_t *d = ensure_dev(w->bind.device_id);
    if (!d) {
        return;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        set_channel_state(d, ch, st);
        return;
    }
    std::snprintf(d->power, sizeof(d->power), "%s", st);
}

static void flip_widget_cache(const nl_widget_t *w)
{
    if (!w) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    bool on = widget_is_on(w, kind);
    set_widget_cache_state(w, on ? "OFF" : "ON");
}

static void ensure_tile_trans(void)
{
    if (s_tile_trans_inited) {
        return;
    }
    s_tile_trans_inited = true;
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR,
                                            LV_STYLE_PROP_INV};
    lv_style_transition_dsc_init(&s_tile_trans, props, lv_anim_path_ease_out, 140, 0, nullptr);
}

static uint32_t widget_fill_for_state(const nl_widget_t *w, tile_kind_t kind, bool online, bool on)
{
    if (!online) {
        return UI_C_OFFLINE;
    }
    if (!on) {
        return kind_fill_off(kind);
    }
    /* 开态：优先 layout 自定义底色，否则语义亮色 */
    uint32_t custom = 0;
    if (w && parse_bg_hex(w->bg_color, &custom)) {
        return custom;
    }
    return kind_fill_on(kind);
}

static void style_tile_visual(lv_obj_t *tile, const nl_widget_t *w)
{
    if (!tile || !w) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    bool online = true;
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0]) {
        online = nodelio_panel_ui_device_online(w->bind.device_id);
    }
    bool on = widget_is_on(w, kind);
    uint32_t fill = widget_fill_for_state(w, kind, online, on);
    ensure_tile_trans();
    lv_obj_set_style_transition(tile, &s_tile_trans, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    if (online && on && kind != TILE_SCENE) {
        /* 开态：极简微弱融合边界，无生硬高亮框 */
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_color_t bc = UI_TILE_ON_BORDER;
        if (kind == TILE_PWM) {
            bc = UI_TILE_PWM_BORDER;
        } else if (kind == TILE_GPIO) {
            bc = UI_TILE_GPIO_BORDER;
        }
        lv_obj_set_style_border_color(tile, bc, 0);
        lv_obj_set_style_border_opa(tile, 60, 0);
    } else if (online && !on && kind != TILE_SCENE) {
        /* 关态细边：暗调精致分界，半透明极细边 */
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(tile, 60, 0);
    } else {
        lv_obj_set_style_border_width(tile, 0, 0);
    }
    ui_set_enabled(tile, online || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO);

    lv_color_t icon_c = (!online) ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT_DIM);
    lv_color_t title_c = (!online) ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT);
    lv_color_t hint_c = (!online) ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT_MUTED);
    lv_color_t badge_bg =
        (!online) ? UI_TILE_BADGE_OFFLINE : (on ? UI_TILE_BADGE_ON : UI_TILE_BADGE_OFF);

    bool is_large = (w && (w->gw >= 2 && w->gh >= 2));
    bool is_row = (w && w->gh <= 1);
    uint32_t sub = lv_obj_get_child_count(tile);
    for (uint32_t j = 0; j < sub; j++) {
        lv_obj_t *ch = lv_obj_get_child(tile, j);
        intptr_t role = (intptr_t)lv_obj_get_user_data(ch);
        if (role == TILE_CHILD_CAPSULE) {
            lv_color_t cap_bg = UI_CAPSULE_OFF;
            if (on && online) {
                uint32_t custom_bg = 0;
                if (w && parse_bg_hex(w->bg_color, &custom_bg)) {
                    /* 自定义底色：同色系提亮微光，避免红底绿胶囊等恶性撞色 */
                    cap_bg = lv_color_hex(color_brighten(custom_bg, 40));
                } else if (kind == TILE_PWM) {
                    cap_bg = UI_CAPSULE_PWM;
                } else if (kind == TILE_GPIO) {
                    cap_bg = UI_CAPSULE_GPIO;
                } else {
                    cap_bg = UI_CAPSULE_ON;
                }
            }
            lv_obj_set_style_bg_color(ch, cap_bg, 0);
            if (lv_obj_get_child_count(ch) > 0) {
                lv_obj_t *icon_lbl = lv_obj_get_child(ch, 0);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(icon_lbl, tile_icon_font(is_large), 0);
                lv_obj_set_style_text_color(icon_lbl, icon_c, 0);
                lv_obj_set_style_text_opa(icon_lbl, (on && online) ? LV_OPA_COVER : LV_OPA_80, 0);
                lv_obj_center(icon_lbl);
            }
        } else if (lv_obj_check_type(ch, &lv_label_class)) {
            if (role == TILE_CHILD_ICON) {
                lv_label_set_text(ch, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(ch, tile_icon_font(is_large), 0);
                lv_obj_set_style_text_color(ch, icon_c, 0);
            } else if (role == TILE_CHILD_HINT) {
                if (kind == TILE_PWM) {
                    char hb[20];
                    const char *prefix = "";
                    if (w && w->skin[0]) {
                        if (strcmp(w->skin, "fan") == 0) {
                            prefix = "风 ";
                        } else if (strcmp(w->skin, "dimmer") == 0) {
                            prefix = "光 ";
                        } else if (strcmp(w->skin, "heater") == 0) {
                            prefix = "热 ";
                        } else if (strcmp(w->skin, "tank") == 0) {
                            prefix = "位 ";
                        } else if (strcmp(w->skin, "gauge") == 0) {
                            prefix = "表 ";
                        }
                    }
                    std::snprintf(hb, sizeof(hb), "%s%d%%", prefix, widget_pwm_duty(w));
                    lv_label_set_text(ch, hb);
                    set_font(ch);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
                } else if (kind == TILE_SCENE) {
                    lv_label_set_text(ch, "情景");
                    set_font(ch);
                    lv_obj_set_style_text_color(ch, UI_TEXT_MUTED, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
                    lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
                } else if (w && strcmp(w->skin, "momentary") == 0) {
                    /* 点动角标 */
                    lv_label_set_text(ch, (!online) ? "离" : "点动");
                    set_font(ch);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, online ? LV_OPA_60 : LV_OPA_50, 0);
                    lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
                } else if (w && strcmp(w->skin, "dual_line") == 0) {
                    /* DualLine 互斥双段：开态显示 NC 名，关态显示 NO 名 */
                    const char *nc = (w->labels_on[0]) ? w->labels_on : "NC";
                    const char *no = (w->labels_off[0]) ? w->labels_off : "NO";
                    lv_label_set_text(ch, (!online) ? "离" : (on ? nc : no));
                    set_font(ch);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, on && online ? LV_OPA_70 : LV_OPA_50, 0);
                    lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
                } else {
                    /* 开/关/离：右上角/右侧小角标，不占主列 */
                    lv_label_set_text(ch, (!online) ? "离" : (on ? "开" : "关"));
                    set_font(ch);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, on && online ? LV_OPA_70 : LV_OPA_50, 0);
                    lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
                }
            } else if (role == TILE_CHILD_TITLE) {
                set_font(ch);
                lv_obj_set_style_text_color(ch, title_c, 0);
            }
        } else if (lv_obj_check_type(ch, &lv_slider_class)) {
            ui_set_enabled(ch, online);
            int duty = widget_pwm_duty(w);
            int mn = w->bind.min;
            int mx = w->bind.max > mn ? w->bind.max : 100;
            if (duty < mn) {
                duty = mn;
            }
            if (duty > mx) {
                duty = mx;
            }
            lv_slider_set_value(ch, duty, LV_ANIM_ON);
        }
    }
}

static void refresh_home_tiles(void)
{
    if (!s_home) {
        return;
    }
    uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(child));
        if (!w) {
            continue;
        }
        style_tile_visual(child, w);
    }
}

static void place_grid_obj(lv_obj_t *obj, const nl_widget_t *w, int cell_w, int cell_h)
{
    int gw = w->gw > 0 ? w->gw : 1;
    int gh = w->gh > 0 ? w->gh : 1;
    int col = w->col >= 0 ? w->col : 0;
    int row = w->row >= 0 ? w->row : 0;
    int x = s_grid_ox + col * (cell_w + GRID_GAP);
    int y = s_grid_oy + row * (cell_h + GRID_GAP);
    int tw = gw * cell_w + (gw - 1) * GRID_GAP;
    int th = gh * cell_h + (gh - 1) * GRID_GAP;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, tw, th);
}

static void strip_server_base(const char *base, char *out, size_t out_len)
{
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    if (n >= 22 && strncmp(base + n - 22, "/api/devices/register", 22) == 0) {
        n -= 22;
    }
    std::snprintf(out, out_len, "%.*s", (int)n, base);
}

static void capture_and_upload_screenshot(void)
{
    Display *disp = Board::GetInstance().GetDisplay();
    if (!disp) {
        toast("未检测到显示屏");
        return;
    }
    toast("截屏中…");
    std::string jpeg;
    if (!disp->SnapshotToJpeg(jpeg, 80, s_root) || jpeg.empty()) {
        ESP_LOGW(TAG, "SnapshotToJpeg failed");
        toast("截屏生成失败");
        return;
    }

    if (s_cfg.server_url[0] == '\0' || s_cfg.device_id[0] == '\0') {
        toast("未配置服务器");
        return;
    }

    char base[160];
    strip_server_base(s_cfg.server_url, base, sizeof(base));
    char url[256];
    std::snprintf(url, sizeof(url), "%s/api/devices/%s/panel/screenshot", base, s_cfg.device_id);

    int http_status = 0;
    esp_err_t err = nodelio_http_post_image(url, s_cfg.device_key, jpeg.data(), jpeg.size(), &http_status);
    if (err == ESP_OK && http_status >= 200 && http_status < 300) {
        ESP_LOGI(TAG, "Screenshot uploaded successfully: %u bytes", (unsigned)jpeg.size());
        toast("已上传实机截图");
    } else {
        ESP_LOGW(TAG, "Screenshot upload failed err=%s http=%d", esp_err_to_name(err), http_status);
        toast("截图上传失败");
    }
}

static void on_brand_click(lv_event_t *e)
{
    (void)e;
    static uint32_t last_click_ms = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_click_ms < 600) {
        last_click_ms = 0;
        capture_and_upload_screenshot();
        return;
    }
    last_click_ms = now;
}

static void widget_cmd(const nl_widget_t *w, const char *value)
{
    if (!w) {
        return;
    }
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
        !nodelio_panel_ui_device_online(w->bind.device_id)) {
        toast("设备未在线");
        return;
    }
    int http = 0;
    esp_err_t err = nodelio_panel_command(&s_cfg, w->id, "set", &w->bind, value, &http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd %s http=%d err=%s", w->id, http, esp_err_to_name(err));
        toast(http == 503 ? "设备未在线" : "操作失败");
    } else {
        s_cloud_ok = true;
        if (s_cloud_lbl) {
            refresh_status_icons(s_wifi_ok, true, nullptr);
        }
    }
}

static void on_btn(lv_event_t *e)
{
    lv_obj_t *tgt = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (tgt && lv_obj_has_state(tgt, LV_STATE_DISABLED)) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w) {
        return;
    }
    if (w->type == NL_WIDGET_SCENE_CHIP || w->type == NL_WIDGET_SCENE_GRID ||
        w->bind.kind == NL_BIND_HOME_SCENARIO) {
        widget_cmd(w, w->bind.name);
        /* 轻反馈：按下后闪一下边框 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
        }
        return;
    }
    if (w && strcmp(w->skin, "momentary") == 0) {
        if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
            !nodelio_panel_ui_device_online(w->bind.device_id)) {
            toast("设备未在线");
            return;
        }
        /* 点动：下发触发脉冲，轻微边框反馈，不把本地缓存翻成开态 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
        }
        int http = 0;
        esp_err_t err = nodelio_panel_command(&s_cfg, w->id, "set", &w->bind, "TOGGLE", &http);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cmd %s http=%d err=%s", w->id, http, esp_err_to_name(err));
            toast(http == 503 ? "设备未在线" : "操作失败");
        } else {
            s_cloud_ok = true;
            if (s_cloud_lbl) {
                refresh_status_icons(s_wifi_ok, true, nullptr);
            }
        }
        return;
    }
    /* 先翻本地态，再下发绝对 ON/OFF；联动同引脚所有卡片瞬间同步响应（0ms 响应） */
    flip_widget_cache(w);
    refresh_home_tiles();
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
        !nodelio_panel_ui_device_online(w->bind.device_id)) {
        toast("设备未在线");
        flip_widget_cache(w);
        refresh_home_tiles();
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    const char *want = widget_is_on(w, kind) ? "ON" : "OFF";
    int http = 0;
    esp_err_t err = nodelio_panel_command(&s_cfg, w->id, "set", &w->bind, want, &http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cmd %s http=%d err=%s", w->id, http, esp_err_to_name(err));
        toast(http == 503 ? "设备未在线" : "操作失败");
        flip_widget_cache(w);
        refresh_home_tiles();
    } else {
        set_widget_cache_state(w, want);
        refresh_home_tiles();
        s_cloud_ok = true;
        if (s_cloud_lbl) {
            refresh_status_icons(s_wifi_ok, true, nullptr);
        }
    }
}

static void on_slider(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (!w || !slider) {
        return;
    }
    int duty = (int)lv_slider_get_value(slider);
    char val[12];
    std::snprintf(val, sizeof(val), "%d", duty);
    set_widget_cache_state(w, val);
    refresh_home_tiles();
    widget_cmd(w, val);
}

static lv_obj_t *make_tile_shell(const nl_widget_t *w, tile_kind_t kind, int cell_w, int cell_h)
{
    uint32_t fill = widget_fill_for_state(w, kind, true, false);
    lv_obj_t *tile = lv_obj_create(s_home);
    lv_obj_set_user_data(tile, (void *)w);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, 60, 0);
    lv_obj_set_style_radius(tile, UI_RADIUS_MD, 0);
    lv_obj_set_style_pad_all(tile, UI_PAD_SM, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    place_grid_obj(tile, w, cell_w, cell_h);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tile, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(tile, 1, LV_STATE_PRESSED);
    ensure_tile_trans();
    lv_obj_set_style_transition(tile, &s_tile_trans, 0);
    return tile;
}

static void add_tile_labels(lv_obj_t *tile, const nl_widget_t *w, tile_kind_t kind, const char *title)
{
    char short_t[24];
    short_label(title, short_t, sizeof(short_t));

    bool is_row = (w && w->gh <= 1);
    bool is_large = (w && (w->gw >= 2 && w->gh >= 2));

    if (is_row) {
        /* 行高为 1：左右结构（左图标胶囊、中标题、右角标） */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 8, 0);
        lv_obj_set_style_pad_right(tile, 36, 0); /* 预留右侧浮动药丸角标宽度，防文字重叠 */
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else {
        /* 行高 >= 2：上下结构（上图标底座、下标题），文字与图标物理隔开 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(tile, is_large ? 12 : 8, 0);
        lv_obj_set_style_pad_top(tile, is_large ? 8 : 4, 0);
        lv_obj_set_style_pad_bottom(tile, is_large ? 8 : 4, 0);
    }

    /* 1. 图标胶囊底座容器（固定尺寸正圆，绝对居中） */
    lv_coord_t cap_size = is_large ? 46 : (is_row ? 28 : 36);
    lv_obj_t *capsule = lv_obj_create(tile);
    lv_obj_remove_style_all(capsule);
    lv_obj_set_user_data(capsule, (void *)(intptr_t)TILE_CHILD_CAPSULE);
    lv_obj_set_size(capsule, cap_size, cap_size);
    lv_obj_set_style_radius(capsule, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(capsule, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    /* 胶囊内的矢量图标（绝对居中，使用高清矢量字库，严禁 transform_scale） */
    lv_obj_t *icon_lbl = lv_label_create(capsule);
    lv_label_set_text(icon_lbl, kind_symbol(w, kind, false));
    lv_obj_set_style_text_font(icon_lbl, tile_icon_font(is_large), 0);
    lv_obj_set_style_text_color(icon_lbl, UI_TEXT_DIM, 0);
    lv_obj_center(icon_lbl);

    /* 2. 标题文字 */
    lv_obj_t *lb = lv_label_create(tile);
    lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
    lv_label_set_text(lb, short_t[0] ? short_t : title);
    set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    if (is_row) {
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_flex_grow(lb, 1);
    } else {
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lb, lv_pct(94));
    }
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

    /* 药丸浮动角标：左右结构靠中右，上下结构靠右上 */
    lv_obj_t *hint = lv_label_create(tile);
    lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
    lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_left(hint, 6, 0);
    lv_obj_set_style_pad_right(hint, 6, 0);
    lv_obj_set_style_pad_top(hint, 2, 0);
    lv_obj_set_style_pad_bottom(hint, 2, 0);
    lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
    lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_OFF, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
    lv_obj_align(hint, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);
    if (kind == TILE_SCENE) {
        lv_label_set_text(hint, "情景");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else if (w && strcmp(w->skin, "momentary") == 0) {
        lv_label_set_text(hint, "点动");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else if (w && strcmp(w->skin, "dual_line") == 0) {
        const char *no = (w->labels_off[0]) ? w->labels_off : "NO";
        lv_label_set_text(hint, no);
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else {
        lv_label_set_text(hint, "关");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    }
}

static void fill_home(void)
{
    if (!s_home) {
        return;
    }
    lv_obj_clean(s_home);
    lv_obj_set_layout(s_home, 0);
    lv_obj_set_style_pad_left(s_home, NL_UI_HOME_PAD_H, 0);
    lv_obj_set_style_pad_right(s_home, NL_UI_HOME_PAD_H, 0);
    lv_obj_set_style_pad_top(s_home, NL_UI_HOME_PAD_V, 0);
    lv_obj_set_style_pad_bottom(s_home, NL_UI_HOME_PAD_V, 0);
    lv_obj_set_style_bg_color(s_home, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_have_layout || !s_layout) {
        lv_obj_t *hint = lv_label_create(s_home);
        lv_label_set_text(hint, "拉取布局中…\n失败请到「设置」填写服务器与绑定码");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(hint);
        return;
    }

    int home_idx = -1;
    for (int i = 0; i < s_layout->page_count; i++) {
        if (s_layout->pages[i].system_page[0] == '\0') {
            home_idx = i;
            break;
        }
    }
    if (home_idx < 0) {
        lv_obj_t *hint = lv_label_create(s_home);
        lv_label_set_text(hint, "无首页\n请在 APP 编辑布局");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_SUB, 0);
        lv_obj_center(hint);
        return;
    }

    const nl_page_t *home = &s_layout->pages[home_idx];
    int widget_total = 0;
    for (int si = 0; si < home->section_count; si++) {
        widget_total += home->sections[si].widget_count;
    }
    if (widget_total == 0) {
        lv_obj_t *hint = lv_label_create(s_home);
        lv_label_set_text(hint, "首页为空\nAPP 添加开关后自动刷新");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_SUB, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(hint);
        return;
    }

    int cols = s_layout->grid_cols > 0 ? s_layout->grid_cols : 6;
    int rows = s_layout->grid_rows > 0 ? s_layout->grid_rows : 6;
    lv_obj_update_layout(s_home);
    lv_coord_t area_w = lv_obj_get_content_width(s_home);
    lv_coord_t area_h = lv_obj_get_content_height(s_home);
    if (area_w < 40) {
        area_w = 452;
    }
    if (area_h < 40) {
        area_h = 380;
    }
    int cell_w = (area_w - GRID_GAP * (cols - 1)) / cols;
    int cell_h = (area_h - GRID_GAP * (rows - 1)) / rows;
    if (cell_w < 20) {
        cell_w = 20;
    }
    if (cell_h < 20) {
        cell_h = 20;
    }
    int used_w = cell_w * cols + GRID_GAP * (cols - 1);
    s_grid_ox = (area_w > used_w) ? (area_w - used_w) / 2 : 0;
    s_grid_oy = 0;

    for (int si = 0; si < home->section_count; si++) {
        const nl_section_t *sec = &home->sections[si];
        for (int wi = 0; wi < sec->widget_count; wi++) {
            nl_widget_t *w = &s_layout->pages[home_idx].sections[si].widgets[wi];
            const char *title = w->title[0] ? w->title : (w->bind.name[0] ? w->bind.name : w->id);
            tile_kind_t kind = tile_kind_of(w);
            if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER) {
                continue;
            }
            if (kind == TILE_PWM) {
                bool is_large = (w && (w->gw >= 2 && w->gh >= 2));
                bool is_row = (w && w->gh <= 1);
                lv_obj_t *row = make_tile_shell(w, kind, cell_w, cell_h);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                     LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_row(row, is_large ? 8 : 4, 0);
                lv_obj_set_style_pad_top(row, is_large ? 8 : 6, 0);
                lv_obj_set_style_pad_bottom(row, is_large ? 8 : 6, 0);

                char short_t[24];
                short_label(title, short_t, sizeof(short_t));
                lv_coord_t cap_size = is_large ? 46 : (is_row ? 28 : 36);
                lv_obj_t *capsule = lv_obj_create(row);
                lv_obj_remove_style_all(capsule);
                lv_obj_set_user_data(capsule, (void *)(intptr_t)TILE_CHILD_CAPSULE);
                lv_obj_set_size(capsule, cap_size, cap_size);
                lv_obj_set_style_radius(capsule, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_color(capsule, UI_CAPSULE_PWM, 0);
                lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
                lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

                lv_obj_t *icon_lbl = lv_label_create(capsule);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, true));
                lv_obj_set_style_text_font(icon_lbl, tile_icon_font(is_large), 0);
                lv_obj_set_style_text_color(icon_lbl, UI_ON_TILE, 0);
                lv_obj_center(icon_lbl);

                lv_obj_t *lb = lv_label_create(row);
                lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
                lv_label_set_text(lb, short_t[0] ? short_t : title);
                set_font(lb);
                lv_obj_set_style_text_color(lb, UI_TEXT, 0);
                lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
                lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
                lv_obj_set_width(lb, lv_pct(94));

                lv_obj_t *hint = lv_label_create(row);
                lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
                lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
                lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
                lv_label_set_text(hint, "0%");
                set_font(hint);
                lv_obj_set_style_pad_left(hint, 6, 0);
                lv_obj_set_style_pad_right(hint, 6, 0);
                lv_obj_set_style_pad_top(hint, 2, 0);
                lv_obj_set_style_pad_bottom(hint, 2, 0);
                lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
                lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_ON, 0);
                lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
                lv_obj_align(hint, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, 0, 0);

                lv_obj_t *sl = lv_slider_create(row);
                lv_obj_set_width(sl, lv_pct(88));
                lv_obj_set_height(sl, 8);
                lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_MAIN);
                lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(sl, UI_PWM_TRACK, LV_PART_MAIN);
                lv_obj_set_style_bg_color(sl, UI_TILE_PWM_BORDER, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(sl, UI_ON_TILE, LV_PART_KNOB);
                lv_slider_set_range(sl, w->bind.min, w->bind.max > w->bind.min ? w->bind.max : 100);
                lv_obj_add_event_cb(sl, on_slider, LV_EVENT_ALL, w);
                style_tile_visual(row, w);
                continue;
            }
            lv_obj_t *tile = make_tile_shell(w, kind, cell_w, cell_h);
            lv_obj_add_event_cb(tile, on_btn, LV_EVENT_CLICKED, w);
            add_tile_labels(tile, w, kind, title);
            style_tile_visual(tile, w);
        }
    }
}

/* —— connect overlay —— */

static void close_settings_detail(void);

static void close_forms(void)
{
    kb_set_visible(false);
    close_settings_detail();
    if (s_connect) {
        lv_obj_add_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tabbar && s_visible) {
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_connect(void)
{
    close_forms();
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
        toast("请填写主机与绑定码");
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
            toast("新网络请填密码");
            return;
        }
    }
    if (!port || !port[0]) {
        port = "8899";
    }

    nodelio_nvs_config_t cfg{};
    nodelio_nvs_load(&cfg);
    char new_url[sizeof(cfg.server_url)];
    std::snprintf(new_url, sizeof(new_url), "%s://%s:%s", s_https ? "https" : "http", host, port);
    bool server_changed = std::strcmp(cfg.server_url, new_url) != 0;
    apply_wifi_creds(ssid, pass, &cfg);
    std::snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", new_url);
    std::snprintf(cfg.owner, sizeof(cfg.owner), "%s", owner);
    /* MQTT 由当前 server 注册响应下发；换地址后旧 broker 必须作废，开机重新 register */
    if (server_changed) {
        nodelio_nvs_clear_mqtt(&cfg);
    }
    if (nodelio_nvs_save(&cfg) != ESP_OK) {
        toast("保存失败");
        return;
    }
    s_cfg = cfg;
    toast(server_changed ? "已保存，将重新注册…" : "已保存，重启…");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

static void open_connect(void)
{
    if (!s_connect) {
        return;
    }
    close_settings_detail();
    (void)nodelio_nvs_load(&s_cfg);
    char host[96] = {0};
    char port[8] = {0};
    parse_server_url(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

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
        lv_label_set_text(s_proto_lbl, s_https ? "https" : "http");
    }
    if (s_scan_btn_lbl && !s_wifi_scanning) {
        lv_label_set_text(s_scan_btn_lbl, "扫描");
    }
    kb_set_visible(false);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
    if (s_tabbar) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_connect);
}

static void on_wifi_scan_btn(lv_event_t *e);

static void build_connect(void)
{
    s_connect = lv_obj_create(s_root);
    lv_obj_add_flag(s_connect, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_connect, lv_pct(100), lv_pct(100));
    lv_obj_align(s_connect, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_connect, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_connect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_connect, 0, 0);
    lv_obj_set_style_pad_all(s_connect, 0, 0);
    lv_obj_set_flex_flow(s_connect, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_connect, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_connect, LV_OBJ_FLAG_HIDDEN);

    (void)ui_make_nav_bar(s_connect, UI_NAV_H, "连接",
                          [](lv_event_t *) { close_connect(); });

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
    parse_server_url(s_cfg.server_url, host, sizeof(host), port, sizeof(port), &s_https);

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
        proto_row, s_https ? "https" : "http", 72, UI_BTN_H_SM,
        [](lv_event_t *) {
            s_https = !s_https;
            if (s_proto_lbl) {
                lv_label_set_text(s_proto_lbl, s_https ? "https" : "http");
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
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { close_connect(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "取消");
    set_font(cl);
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
    set_font(sl);
    lv_obj_set_style_text_color(sl, UI_TEXT, 0);
    lv_obj_center(sl);

    lv_obj_t *tip = lv_label_create(s_connect_scroll);
    lv_label_set_text(tip, "先点「扫描」选 WiFi，再填密码与服务器；主机填局域网 IP");
    set_font(tip);
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
    toast("已选网络，请填密码");
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
        set_font(empty);
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
        set_font(lbl);
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
        DisplayLockGuard lock(disp());
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
        toast(msg);
    } else {
        toast(err == ESP_OK ? "未扫到，可手填" : "扫描失败");
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
    toast("正在扫描…");
    if (xTaskCreate(wifi_scan_task, "nl_wscan", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_wifi_scanning = false;
        if (s_scan_btn_lbl) {
            lv_label_set_text(s_scan_btn_lbl, "扫描");
        }
        toast("扫描失败");
    }
}

static void do_factory_reset(void)
{
    nodelio_nvs_config_t cfg{};
    (void)nodelio_nvs_save(&cfg);
    s_cfg = cfg;
    (void)nodelio_layout_cache_clear();
    SsidManager::GetInstance().Clear(); /* 清小智 WiFi，重启后进 SoftAP */
    ESP_LOGW(TAG, "factory reset — reboot");
    toast("已清空，重启…");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

/* —— settings hub / detail —— */

typedef enum {
    SET_PAGE_DISP = 1,
    SET_PAGE_SYS = 2,
    SET_PAGE_RESET = 3,
} set_page_id_t;

static void open_settings_detail(set_page_id_t page);

static void on_factory_confirm(lv_event_t *e)
{
    (void)e;
    do_factory_reset();
}

static void load_audio_prefs(void)
{
    auto *bl = Board::GetInstance().GetBacklight();
    if (bl) {
        uint8_t b = bl->brightness();
        s_brightness = (b >= 5) ? b : 80;
    } else if (s_cfg.brightness >= 5) {
        s_brightness = s_cfg.brightness;
    }
    auto *codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        int v = codec->output_volume();
        if (v < 0) {
            v = 0;
        }
        if (v > 100) {
            v = 100;
        }
        s_volume = (uint8_t)v;
    } else if (s_cfg.volume <= 100) {
        s_volume = s_cfg.volume;
    }
}

static void apply_brightness(uint8_t pct)
{
    if (pct < 5) {
        pct = 5;
    }
    if (pct > 100) {
        pct = 100;
    }
    s_brightness = pct;
    s_cfg.brightness = pct;
    auto *bl = Board::GetInstance().GetBacklight();
    if (bl) {
        bl->SetBrightness(pct, true);
    }
}

static void apply_volume(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    s_volume = pct;
    s_cfg.volume = pct;
    auto *codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->SetOutputVolume((int)pct);
    }
}

static void persist_ui_prefs(void)
{
    s_cfg.brightness = s_brightness;
    s_cfg.volume = s_volume;
    (void)nodelio_nvs_save(&s_cfg);
}

static lv_obj_t *audio_ctrl_band(lv_obj_t *parent, const char *title, const char *val, lv_obj_t **out_val)
{
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_set_width(band, lv_pct(100));
    lv_obj_set_style_bg_color(band, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(band, UI_RADIUS, 0);
    lv_obj_set_style_pad_left(band, 16, 0);
    lv_obj_set_style_pad_right(band, 16, 0);
    lv_obj_set_style_pad_top(band, 12, 0);
    lv_obj_set_style_pad_bottom(band, 14, 0);
    lv_obj_set_style_pad_row(band, 8, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(band);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, 28);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, title);
    set_font(t);
    lv_obj_set_style_text_color(t, UI_TEXT_DIM, 0);

    lv_obj_t *v = lv_label_create(head);
    lv_label_set_text(v, val);
    set_font(v);
    lv_obj_set_style_text_color(v, UI_ACCENT, 0);
    if (out_val) {
        *out_val = v;
    }
    return band;
}

static void on_bri_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    apply_brightness((uint8_t)lv_slider_get_value(sl));
    if (s_bri_val_lbl) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(s_bri_val_lbl, buf);
    }
}

static void on_bri_release(lv_event_t *e)
{
    (void)e;
    persist_ui_prefs();
}

static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    apply_volume((uint8_t)lv_slider_get_value(sl));
    if (s_vol_val_lbl) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(s_vol_val_lbl, buf);
    }
}

static void on_vol_release(lv_event_t *e)
{
    (void)e;
    persist_ui_prefs();
    toast("音量已保存");
}

static void sys_label_set_if_changed(lv_obj_t *lbl, char *last, size_t last_sz, const char *neu)
{
    if (!lbl || !neu || !last) {
        return;
    }
    if (strncmp(last, neu, last_sz) == 0) {
        return;
    }
    strncpy(last, neu, last_sz - 1);
    last[last_sz - 1] = '\0';
    lv_label_set_text(lbl, neu);
}

static void refresh_sys_live_fields(void)
{
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%uh %um %us", (unsigned)(up_s / 3600),
                  (unsigned)((up_s % 3600) / 60), (unsigned)(up_s % 60));
    sys_label_set_if_changed(s_sys_up_lbl, s_sys_last_up, sizeof(s_sys_last_up), buf);

    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    std::snprintf(buf, sizeof(buf), "%u KB", (unsigned)(dram_free / 1024));
    sys_label_set_if_changed(s_sys_dram_lbl, s_sys_last_dram, sizeof(s_sys_last_dram), buf);

    size_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    std::snprintf(buf, sizeof(buf), "%u KB", (unsigned)(ps_free / 1024));
    sys_label_set_if_changed(s_sys_psram_lbl, s_sys_last_psram, sizeof(s_sys_last_psram), buf);

    std::snprintf(buf, sizeof(buf), "%u", (unsigned)uxTaskGetNumberOfTasks());
    sys_label_set_if_changed(s_sys_tasks_lbl, s_sys_last_tasks, sizeof(s_sys_last_tasks), buf);
}

static lv_obj_t *sys_kv_row(lv_obj_t *parent, const char *key, const char *val_init, lv_obj_t **out_val)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 28);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    set_font(k);
    lv_obj_set_style_text_color(k, UI_TEXT_MUTED, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val_init ? val_init : "-");
    lv_obj_set_width(v, 200);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    set_font(v);
    lv_obj_set_style_text_color(v, UI_TEXT, 0);
    if (out_val) {
        *out_val = v;
    }
    return row;
}

static void sys_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_sys_up_lbl) {
        return;
    }
    refresh_sys_live_fields();
}

static void stop_sys_monitor(void)
{
    if (s_sys_timer) {
        lv_timer_delete(s_sys_timer);
        s_sys_timer = nullptr;
    }
    s_sys_up_lbl = nullptr;
    s_sys_dram_lbl = nullptr;
    s_sys_psram_lbl = nullptr;
    s_sys_tasks_lbl = nullptr;
    s_sys_last_up[0] = '\0';
    s_sys_last_dram[0] = '\0';
    s_sys_last_psram[0] = '\0';
    s_sys_last_tasks[0] = '\0';
}

static void close_settings_detail(void)
{
    stop_sys_monitor();
    if (!s_settings_detail) {
        return;
    }
    lv_obj_delete(s_settings_detail);
    s_settings_detail = nullptr;
    s_bri_val_lbl = nullptr;
    s_vol_val_lbl = nullptr;
    if (s_tabbar && s_visible) {
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *settings_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_style_bg_color(card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (title && title[0]) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        set_font(t);
        lv_obj_set_style_text_color(t, UI_TEXT_SUB, 0);
    }
    return card;
}

static void open_settings_detail(set_page_id_t page)
{
    close_settings_detail();
    if (!s_root) {
        return;
    }
    load_audio_prefs();

    s_settings_detail = lv_obj_create(s_root);
    lv_obj_add_flag(s_settings_detail, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_settings_detail, lv_pct(100), lv_pct(100));
    lv_obj_align(s_settings_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_settings_detail, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_settings_detail, 0, 0);
    lv_obj_set_style_pad_all(s_settings_detail, 0, 0);
    lv_obj_set_flex_flow(s_settings_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_settings_detail, LV_OBJ_FLAG_SCROLLABLE);

    const char *title = "设置";
    if (page == SET_PAGE_DISP) {
        title = "显示与声音";
    } else if (page == SET_PAGE_SYS) {
        title = "设备信息";
    } else if (page == SET_PAGE_RESET) {
        title = "重置";
    }
    (void)ui_make_nav_bar(s_settings_detail, UI_NAV_H, title,
                          [](lv_event_t *) { close_settings_detail(); });

    lv_obj_t *body = lv_obj_create(s_settings_detail);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_color(body, UI_BG, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(body, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_right(body, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_top(body, NL_UI_SETTINGS_DETAIL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(body, NL_UI_SETTINGS_DETAIL_PAD, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    if (page == SET_PAGE_DISP) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_row(body, 12, 0);

        char pct[12];
        std::snprintf(pct, sizeof(pct), "%d%%", s_brightness);
        lv_obj_t *bri_band = audio_ctrl_band(body, "亮度", pct, &s_bri_val_lbl);
        lv_obj_t *bri_sl = lv_slider_create(bri_band);
        ui_style_slider(bri_sl);
        lv_slider_set_range(bri_sl, 5, 100);
        lv_slider_set_value(bri_sl, s_brightness, LV_ANIM_OFF);
        lv_obj_add_event_cb(bri_sl, on_bri_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(bri_sl, on_bri_release, LV_EVENT_RELEASED, nullptr);

        std::snprintf(pct, sizeof(pct), "%d%%", s_volume);
        lv_obj_t *vol_band = audio_ctrl_band(body, "音量", pct, &s_vol_val_lbl);
        lv_obj_t *vol_sl = lv_slider_create(vol_band);
        ui_style_slider(vol_sl);
        lv_slider_set_range(vol_sl, 0, 100);
        lv_slider_set_value(vol_sl, s_volume, LV_ANIM_OFF);
        lv_obj_add_event_cb(vol_sl, on_vol_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(vol_sl, on_vol_release, LV_EVENT_RELEASED, nullptr);
        lv_obj_t *hint = lv_label_create(vol_band);
        lv_label_set_text(hint, "本机扬声器音量");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);
    } else if (page == SET_PAGE_SYS) {
        (void)nodelio_nvs_load(&s_cfg);
        load_audio_prefs();
        const nodelio_board_t *b = nodelio_board_get();
        esp_chip_info_t chip {};
        esp_chip_info(&chip);
        char line[128];

        lv_obj_t *dev = settings_card(body, "设备");
        lv_obj_set_style_pad_row(dev, 4, 0);
        std::snprintf(line, sizeof(line), "%s", b ? b->info.board_id : "?");
        sys_kv_row(dev, "板型", line, nullptr);
        {
            std::string ua = SystemInfo::GetUserAgent();
            sys_kv_row(dev, "固件", ua.c_str(), nullptr);
        }
        {
            std::string mac = SystemInfo::GetMacAddress();
            sys_kv_row(dev, "MAC", mac.c_str(), nullptr);
        }
        sys_kv_row(dev, "设备 ID", s_cfg.device_id[0] ? s_cfg.device_id : "未注册", nullptr);
        sys_kv_row(dev, "账号", s_cfg.owner[0] ? s_cfg.owner : "-", nullptr);
        sys_kv_row(dev, "服务器", s_cfg.server_url[0] ? s_cfg.server_url : "未配置", nullptr);
        sys_kv_row(dev, "WiFi", s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "未配置", nullptr);
        {
            wifi_ap_record_t ap {};
            bool wifi_ok = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
            sys_kv_row(dev, "链路", wifi_ok ? "已连接" : "未连接", nullptr);
        }
        sys_kv_row(dev, "云", nodelio_mqtt_panel_connected() ? "MQTT 在线" : "离线", nullptr);
        std::snprintf(line, sizeof(line), "v%d",
                      (s_have_layout && s_layout) ? s_layout->layout_version : 0);
        sys_kv_row(dev, "布局", line, nullptr);
        std::snprintf(line, sizeof(line), "%d%% / %d%%", s_brightness, s_volume);
        sys_kv_row(dev, "亮/音", line, nullptr);

        lv_obj_t *run = settings_card(body, "运行状态");
        lv_obj_set_style_pad_row(run, 4, 0);
        std::snprintf(line, sizeof(line), "%s x%d", SystemInfo::GetChipModelName().c_str(),
                      (int)chip.cores);
        sys_kv_row(run, "芯片", line, nullptr);
        std::snprintf(line, sizeof(line), "%u MB",
                      (unsigned)(SystemInfo::GetFlashSize() / (1024 * 1024)));
        sys_kv_row(run, "Flash", line, nullptr);
        sys_kv_row(run, "运行", "-", &s_sys_up_lbl);
        sys_kv_row(run, "任务", "-", &s_sys_tasks_lbl);
        sys_kv_row(run, "DRAM", "-", &s_sys_dram_lbl);
        sys_kv_row(run, "PSRAM", "-", &s_sys_psram_lbl);
        std::snprintf(line, sizeof(line), "%u KB",
                      (unsigned)(SystemInfo::GetMinimumFreeHeapSize() / 1024));
        sys_kv_row(run, "堆最低", line, nullptr);
        refresh_sys_live_fields();
        s_sys_timer = lv_timer_create(sys_timer_cb, 1000, nullptr);
        if (s_sys_timer) {
            lv_timer_set_repeat_count(s_sys_timer, -1);
        }
    } else if (page == SET_PAGE_RESET) {
        lv_obj_t *card = settings_card(body, "恢复出厂");
        lv_obj_t *warn = lv_label_create(card);
        lv_label_set_text(warn, "将清空 WiFi、服务器凭证与布局缓存，设备会立即重启。");
        lv_obj_set_width(warn, lv_pct(100));
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        set_font(warn);
        lv_obj_set_style_text_color(warn, UI_DANGER_FG, 0);

        (void)ui_make_text_btn(card, "确认重置并重启", 1, on_factory_confirm);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint, "取消请点左上角返回");
        set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    }

    if (s_tabbar) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_settings_detail);
}

static void settings_tile(lv_obj_t *parent, const char *glyph, const char *title, const char *sub,
                          uint32_t accent, lv_event_cb_t cb)
{
    (void)ui_make_settings_tile(parent, glyph, title, sub, accent, cb);
}

static void fill_settings(void)
{
    if (!s_settings) {
        return;
    }
    close_settings_detail();
    lv_obj_clean(s_settings);
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_settings, UI_GAP_LG, 0);
    lv_obj_set_style_pad_column(s_settings, UI_GAP_LG, 0);
    lv_obj_set_style_pad_left(s_settings, NL_UI_SETTINGS_PAD_H, 0);
    lv_obj_set_style_pad_right(s_settings, NL_UI_SETTINGS_PAD_H, 0);
    lv_obj_set_style_pad_top(s_settings, 20, 0);
    lv_obj_set_style_pad_bottom(s_settings, 20, 0);
    lv_obj_set_style_bg_color(s_settings, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(s_settings, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings, LV_SCROLLBAR_MODE_OFF);

    load_audio_prefs();
    char bri_sub[32];
    std::snprintf(bri_sub, sizeof(bri_sub), "亮度 %d%% · 音量 %d%%", s_brightness, s_volume);

    settings_tile(s_settings, "显", "显示与声音", bri_sub, UI_C_ACCENT,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_DISP); });
    settings_tile(s_settings, "连", "连接", "扫描 WiFi / 服务器 / 绑定", UI_C_WARN,
                  [](lv_event_t *) { open_connect(); });
    settings_tile(s_settings, "刷", "刷新布局", "重新拉取首页控件", UI_C_INFO, [](lv_event_t *) {
        toast("正在刷新…");
        nodelio_layout_request_sync();
    });
    settings_tile(s_settings, "机", "设备信息", "内存 · 运行时间", UI_C_PURPLE,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_SYS); });
    settings_tile(s_settings, "复", "重置", "清空凭证并重启", UI_C_DANGER,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_RESET); });
}

static void eq_anim_exec(void *obj, int32_t v)
{
    lv_obj_set_height(static_cast<lv_obj_t *>(obj), v);
}

static void stop_eq_anims(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_eq_bars[i]) {
            lv_anim_delete(s_eq_bars[i], eq_anim_exec);
            lv_obj_set_height(s_eq_bars[i], 10);
            lv_obj_set_style_bg_color(s_eq_bars[i], UI_TEXT_MUTED, 0);
        }
    }
}

static void start_eq_anims(chat_phase_t phase)
{
    stop_eq_anims();
    if (phase != CHAT_LISTEN && phase != CHAT_SPEAK) {
        return;
    }
    const bool listen = (phase == CHAT_LISTEN);
    const uint32_t color = listen ? UI_C_ACCENT : UI_C_ACCENT_SOFT;
    const int32_t hi = listen ? 36 : 28;
    const uint32_t period = listen ? 480 : 320;
    for (int i = 0; i < 3; i++) {
        if (!s_eq_bars[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(color), 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_eq_bars[i]);
        lv_anim_set_values(&a, 8, hi - (i * 4));
        lv_anim_set_duration(&a, period + (uint32_t)i * 70);
        lv_anim_set_reverse_duration(&a, period + (uint32_t)i * 50);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, eq_anim_exec);
        lv_anim_set_delay(&a, (uint32_t)i * 90);
        lv_anim_start(&a);
    }
}

static void apply_chat_phase(chat_phase_t phase, const char *label)
{
    s_chat_phase = phase;
    if (label && label[0]) {
        utf8_copy(s_phase_text, sizeof(s_phase_text), label);
    } else {
        const char *def = "说唤醒词开始";
        if (phase == CHAT_LISTEN) {
            def = "聆听中";
        } else if (phase == CHAT_SPEAK) {
            def = "说话中";
        } else if (phase == CHAT_CONNECT) {
            def = "连接中";
        }
        utf8_copy(s_phase_text, sizeof(s_phase_text), def);
    }
    if (s_phase_lbl) {
        lv_label_set_text(s_phase_lbl, s_phase_text);
        uint32_t c = UI_C_TEXT_MUTED;
        if (phase == CHAT_LISTEN || phase == CHAT_SPEAK) {
            c = UI_C_ACCENT;
        } else if (phase == CHAT_CONNECT) {
            c = UI_C_WARN;
        }
        lv_obj_set_style_text_color(s_phase_lbl, lv_color_hex(c), 0);
    }
    start_eq_anims(phase);
}

static chat_phase_t phase_from_status(const char *status)
{
    if (!status) {
        return CHAT_IDLE;
    }
    if (strstr(status, "聆听") || strstr(status, "Listening") || strstr(status, "listening")) {
        return CHAT_LISTEN;
    }
    if (strstr(status, "说话") || strstr(status, "Speaking") || strstr(status, "speaking")) {
        return CHAT_SPEAK;
    }
    /* 仅「连接中 / 登录…」，勿匹配「连接成功 / 已连接」 */
    if (strstr(status, "连接中") || strstr(status, "Connecting") || strstr(status, "登录")) {
        return CHAT_CONNECT;
    }
    if (strstr(status, "待命") || strstr(status, "Standby") || strstr(status, "standby")) {
        return CHAT_IDLE;
    }
    return CHAT_IDLE; /* 非相位文案 → 走 caption */
}

static bool status_is_phase(const char *status)
{
    if (!status) {
        return false;
    }
    return strstr(status, "聆听") || strstr(status, "说话") || strstr(status, "待命") ||
           strstr(status, "连接中") || strstr(status, "Listening") || strstr(status, "Speaking") ||
           strstr(status, "Standby") || strstr(status, "Connecting") || strstr(status, "登录");
}

/** 状态栏时钟 "HH:MM" —— 不写进对话上区 caption */
static bool status_is_clock(const char *status)
{
    if (!status || strlen(status) < 4 || strlen(status) > 5) {
        return false;
    }
    /* 允许 H:MM / HH:MM */
    const char *colon = strchr(status, ':');
    if (!colon || colon == status || colon[1] == '\0' || colon[2] == '\0' || colon[3] != '\0') {
        return false;
    }
    for (const char *p = status; *p; p++) {
        if (p != colon && (*p < '0' || *p > '9')) {
            return false;
        }
    }
    return true;
}

static void scroll_chat_to_end(void)
{
    if (!s_chat_band || !lv_obj_is_valid(s_chat_band)) {
        return;
    }
    if (s_chat_lbl && lv_obj_is_valid(s_chat_lbl)) {
        lv_obj_update_layout(s_chat_lbl);
    }
    lv_obj_update_layout(s_chat_band);
    /* 视口高度为 0 时不要滚，否则会把整段字滚没 */
    if (lv_obj_get_content_height(s_chat_band) <= 0) {
        return;
    }
    lv_coord_t bottom = lv_obj_get_scroll_bottom(s_chat_band);
    if (bottom > 0) {
        lv_obj_scroll_to_y(s_chat_band, lv_obj_get_scroll_top(s_chat_band) + bottom, LV_ANIM_OFF);
    }
}

static void fill_chat(void)
{
    if (!s_chat) {
        return;
    }
    stop_eq_anims();
    lv_obj_clean(s_chat);
    s_phase_lbl = nullptr;
    s_chat_band = nullptr;
    s_chat_lbl = nullptr;
    s_eq_bars[0] = s_eq_bars[1] = s_eq_bars[2] = nullptr;

    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_chat, 16, 0);
    lv_obj_set_style_pad_row(s_chat, 10, 0);
    lv_obj_set_style_bg_color(s_chat, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);

    /* 上区：相位 + 波形，无底板色块 */
    lv_obj_t *stage = lv_obj_create(s_chat);
    lv_obj_remove_style_all(stage);
    lv_obj_set_width(stage, lv_pct(100));
    lv_obj_set_height(stage, 88);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(stage, 8, 0);
    lv_obj_set_style_pad_row(stage, 8, 0);
    lv_obj_set_flex_flow(stage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stage, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eq_row = lv_obj_create(stage);
    lv_obj_remove_style_all(eq_row);
    lv_obj_set_size(eq_row, 72, 40);
    lv_obj_set_style_bg_opa(eq_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(eq_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(eq_row, 8, 0);
    lv_obj_clear_flag(eq_row, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        s_eq_bars[i] = lv_obj_create(eq_row);
        lv_obj_remove_style_all(s_eq_bars[i]);
        lv_obj_set_size(s_eq_bars[i], 10, 10);
        lv_obj_set_style_radius(s_eq_bars[i], 4, 0);
        lv_obj_set_style_bg_color(s_eq_bars[i], UI_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    s_phase_lbl = lv_label_create(stage);
    set_font(s_phase_lbl);
    lv_obj_set_style_text_align(s_phase_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* 下区：height=1 + flex_grow 占满剩余高度并可滚动（勿用 min_height=0 压成 0） */
    s_chat_band = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_chat_band);
    lv_obj_set_width(s_chat_band, lv_pct(100));
    lv_obj_set_height(s_chat_band, 1);
    lv_obj_set_flex_grow(s_chat_band, 1);
    lv_obj_set_style_bg_color(s_chat_band, UI_CHAT_BAND, 0);
    lv_obj_set_style_bg_opa(s_chat_band, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_chat_band, 12, 0);
    lv_obj_set_style_pad_all(s_chat_band, 14, 0);
    lv_obj_set_style_pad_bottom(s_chat_band, 20, 0);
    lv_obj_set_scroll_dir(s_chat_band, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_band, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_chat_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_chat_band, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    s_chat_lbl = lv_label_create(s_chat_band);
    lv_label_set_text(s_chat_lbl, s_chat_text[0] ? s_chat_text : "对话会出现在这里");
    lv_obj_set_width(s_chat_lbl, lv_pct(100));
    lv_label_set_long_mode(s_chat_lbl, LV_LABEL_LONG_WRAP);
    set_font(s_chat_lbl);
    lv_obj_set_style_text_color(s_chat_lbl, UI_TEXT_SUB, 0);
    lv_obj_set_style_text_opa(s_chat_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_line_space(s_chat_lbl, 6, 0);

    apply_chat_phase(s_chat_phase, s_phase_text[0] ? s_phase_text : nullptr);
    scroll_chat_to_end();
}

/* —— tabs / shell —— */

static void show_page(int tab)
{
    s_active_tab = tab;
    s_want_chat = (tab == 1);
    if (tab != 2) {
        s_want_settings = false;
    }
    close_forms();
    if (s_home) {
        if (tab == 0) {
            lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_chat) {
        if (tab == 1) {
            lv_obj_clear_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
            scroll_chat_to_end();
        } else {
            lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_settings) {
        if (tab == 2) {
            lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (s_tab_btns[i]) {
            ui_tab_set_checked(s_tab_btns[i], i == tab);
        }
    }
}

static void apply_visible_locked(void)
{
    if (!s_root) {
        return;
    }
    if (s_want_settings) {
        show_page(2);
        open_connect();
    } else if (s_want_chat) {
        show_page(1);
    } else {
        show_page(0);
    }
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    if (s_fab) {
        lv_obj_add_flag(s_fab, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_root);
    s_visible = true;
}

static lv_obj_t *mk_tab(lv_obj_t *bar, const char *txt, int id)
{
    lv_obj_t *b = ui_make_tab(bar, txt);
    ui_tab_set_checked(b, false);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
            int id = (int)(intptr_t)lv_event_get_user_data(e);
            show_page(id);
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return b;
}

static void build_shell_on_lvgl(void * /*user_data*/)
{
    s_building = false;
    if (s_ready) {
        apply_visible_locked();
        return;
    }

    ui_styles_init();

    s_root = lv_obj_create(lv_layer_top());
    if (!s_root) {
        ESP_LOGE(TAG, "root create fail");
        return;
    }
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_root, ui_font(), 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* top chrome：左品牌 · 中时钟 · 右 WiFi/云 + 布局版本 */
    s_status_bar = lv_obj_create(s_root);
    lv_obj_t *bar = s_status_bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), NL_UI_STATUS_H);
    lv_obj_set_style_bg_color(bar, UI_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_brand_lbl = lv_label_create(bar);
    lv_label_set_text(s_brand_lbl, "NodeLio");
    set_font(s_brand_lbl);
    lv_obj_set_style_text_color(s_brand_lbl, UI_ACCENT, 0);
    lv_obj_add_flag(s_brand_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_brand_lbl, 16);
    lv_obj_add_event_cb(s_brand_lbl, on_brand_click, LV_EVENT_CLICKED, nullptr);

    s_clock_lbl = lv_label_create(bar);
    lv_label_set_text(s_clock_lbl, "--:--");
    s_clock_last[0] = '\0';
    set_font(s_clock_lbl);
    lv_obj_set_style_text_color(s_clock_lbl, UI_TEXT_SUB, 0);

    s_status_lbl = lv_label_create(bar);
    lv_label_set_text(s_status_lbl, "v0");
    lv_obj_set_width(s_status_lbl, 36);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    set_font(s_status_lbl);
    lv_obj_set_style_text_color(s_status_lbl, UI_STATUS_CHROME, 0);

    s_cloud_lbl = lv_label_create(bar);
    lv_label_set_text(s_cloud_lbl, LV_SYMBOL_UPLOAD);
    set_icon_font(s_cloud_lbl);
    lv_obj_set_style_text_color(s_cloud_lbl, UI_TEXT_MUTED, 0);

    s_wifi_lbl = lv_label_create(bar);
    lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI);
    set_icon_font(s_wifi_lbl);
    lv_obj_set_style_text_color(s_wifi_lbl, UI_TEXT_MUTED, 0);
    status_bar_relayout();

    /* body */
    s_body = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    s_home = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_home, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);

    s_chat = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_chat, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);

    s_settings = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_settings, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);

    /* bottom tabs — custom, not lv_tabview */
    s_tabbar = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_tabbar);
    lv_obj_set_size(s_tabbar, lv_pct(100), NL_UI_TAB_H);
    lv_obj_set_style_bg_opa(s_tabbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_tabbar, 4, 0);
    lv_obj_set_style_pad_column(s_tabbar, 4, 0);
    lv_obj_set_flex_flow(s_tabbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tabbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_SCROLLABLE);

    s_tab_btns[0] = mk_tab(s_tabbar, "首页", 0);
    s_tab_btns[1] = mk_tab(s_tabbar, "对话", 1);
    s_tab_btns[2] = mk_tab(s_tabbar, "设置", 2);

    /* 开机优先加载本地缓存布局：首帧立即渲染完整面板，无需等待 WiFi/HTTP 注册 */
    if (!s_have_layout) {
        (void)nodelio_layout_cache_mount();
        char *cached = nullptr;
        int cver = 0;
        if (nodelio_layout_cache_load(&cached, nullptr, &cver) == ESP_OK && cached) {
            if (!s_layout) {
                s_layout = static_cast<nodelio_panel_layout_t *>(
                    heap_caps_malloc(sizeof(*s_layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!s_layout) {
                    s_layout = static_cast<nodelio_panel_layout_t *>(malloc(sizeof(*s_layout)));
                }
            }
            if (s_layout && nodelio_layout_parse(cached, s_layout) == ESP_OK) {
                s_have_layout = true;
                ESP_LOGI(TAG, "boot loaded cached layout v%d for instant render",
                         s_layout->layout_version);
            }
            free(cached);
        }
    }

    fill_home();
    fill_chat();
    fill_settings();
    build_connect();
    /* WiFi 扫描已并入「连接」页；不再单独建 WiFi 页，避免覆盖 s_ta_ssid 等指针 */

    s_ready = true;
    apply_visible_locked();
    start_status_poll();
    if (!s_activity_tmr) {
        s_activity_tmr = lv_timer_create(
            [](lv_timer_t *) {
                for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev;
                     indev = lv_indev_get_next(indev)) {
                    if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
                        lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
                        Board::GetInstance().NoteUserActivity();
                        break;
                    }
                }
            },
            250, nullptr);
    }
    ESP_LOGI(TAG, "shell ready (puhui+grid+kb)");
}

static void create_fab_locked(void)
{
    if (s_fab) {
        return;
    }
    /* FAB 仅作兜底；开机后 overlay 常驻盖住白底，平时隐藏 */
    s_fab = lv_button_create(lv_layer_top());
    lv_obj_set_size(s_fab, 52, 52);
    lv_obj_align(s_fab, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_set_style_radius(s_fab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_fab, UI_ACCENT, 0);
    lv_obj_set_style_shadow_width(s_fab, 0, 0);
    lv_obj_add_flag(s_fab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *fl = lv_label_create(s_fab);
    set_font(fl);
    lv_label_set_text(fl, "家");
    lv_obj_set_style_text_color(fl, UI_ON_TILE, 0);
    lv_obj_center(fl);
    lv_obj_add_event_cb(s_fab, [](lv_event_t *) { nodelio_overlay_show_chat(); }, LV_EVENT_CLICKED,
                        nullptr);
    s_fab_ready = true;
}

esp_err_t nodelio_overlay_init(void)
{
    if (s_fab_ready) {
        return ESP_OK;
    }
    (void)nodelio_nvs_init();
    (void)nodelio_nvs_load(&s_cfg);
    DisplayLockGuard lock(disp());
    if (!lock) {
        return ESP_FAIL;
    }
    create_fab_locked();
    ESP_LOGI(TAG, "FAB ready");
    return ESP_OK;
}

void nodelio_overlay_show_chat(void)
{
    s_want_settings = false;
    s_want_chat = true;
    if (s_visible && s_ready) {
        DisplayLockGuard lock(disp());
        if (lock) {
            show_page(1);
        }
        return;
    }
    nodelio_overlay_show(false);
}

static void utf8_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = 0;
    while (src[n] && n + 1 < dst_len) {
        n++;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void append_chat_line(const char *role, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    const char *tag = "助手";
    if (role && strcmp(role, "user") == 0) {
        tag = "我";
    } else if (role && strcmp(role, "system") == 0) {
        tag = "系统";
    }
    char line[192];
    std::snprintf(line, sizeof(line), "%s：%s", tag, text);
    if (s_chat_text[0] == '\0') {
        utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    size_t a = strlen(s_chat_text);
    size_t b = strlen(line);
    if (a + 1 + b + 1 <= sizeof(s_chat_text)) {
        s_chat_text[a] = '\n';
        memcpy(s_chat_text + a + 1, line, b + 1);
        return;
    }
    size_t max_old = (b + 2 < sizeof(s_chat_text)) ? (sizeof(s_chat_text) - 2 - b) : 0;
    if (max_old == 0) {
        utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    const char *p = s_chat_text;
    if (a > max_old) {
        p = s_chat_text + (a - max_old);
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    size_t keep = strlen(p);
    if (keep + 1 + b + 1 > sizeof(s_chat_text)) {
        utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    memmove(s_chat_text, p, keep);
    s_chat_text[keep] = '\n';
    memcpy(s_chat_text + keep + 1, line, b + 1);
}

void nodelio_overlay_on_chat_locked(const char *role, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    /* 正文只进下区历史，不再复写上区 */
    append_chat_line(role, text);
    if (s_chat_lbl) {
        lv_label_set_text(s_chat_lbl, s_chat_text);
        scroll_chat_to_end();
    }
}

void nodelio_overlay_on_status_locked(const char *status)
{
    if (!status || !status[0] || status_is_clock(status)) {
        return;
    }
    /* 只跟会话相位；检查更新/登录等杂讯不进对话页 */
    if (status_is_phase(status)) {
        apply_chat_phase(phase_from_status(status), status);
    }
}

void nodelio_overlay_show(bool force_settings)
{
    if (!s_fab_ready) {
        (void)nodelio_overlay_init();
    }
    s_want_settings = force_settings;
    if (force_settings) {
        s_want_chat = false;
    }

    if (s_ready) {
        DisplayLockGuard lock(disp());
        if (!lock) {
            return;
        }
        apply_visible_locked();
        return;
    }
    if (s_building) {
        return;
    }
    s_building = true;
    lv_async_call(build_shell_on_lvgl, nullptr);
}

void nodelio_overlay_hide(void)
{
    /* 不再露出小智白底：只关子弹、留在首页 */
    if (!s_ready || !s_root) {
        return;
    }
    DisplayLockGuard lock(disp());
    if (!lock) {
        return;
    }
    close_forms();
    show_page(0);
}

bool nodelio_overlay_visible(void)
{
    return s_visible;
}

/* —— panel_ui.h API —— */

extern "C" esp_err_t nodelio_panel_ui_start(const nodelio_nvs_config_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    }
    return nodelio_overlay_init();
}

extern "C" esp_err_t nodelio_panel_ui_apply_layout(const nodelio_panel_layout_t *layout)
{
    if (!layout) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_layout) {
        s_layout = static_cast<nodelio_panel_layout_t *>(
            heap_caps_malloc(sizeof(*s_layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_layout) {
            s_layout = static_cast<nodelio_panel_layout_t *>(malloc(sizeof(*s_layout)));
        }
    }
    if (!s_layout) {
        return ESP_ERR_NO_MEM;
    }
    *s_layout = *layout;
    s_have_layout = true;
    s_cloud_ok = true; /* 拉到 layout 说明 NodeLio 服务可达 */
    if (s_ready && s_home) {
        DisplayLockGuard lock(disp());
        if (lock) {
            fill_home();
            char msg[20];
            std::snprintf(msg, sizeof(msg), "v%d", layout->layout_version);
            refresh_status_icons(s_wifi_ok, true, msg);
        }
    }
    ESP_LOGI(TAG, "layout v%d cached", layout->layout_version);
    return ESP_OK;
}

extern "C" void nodelio_panel_ui_set_status(bool wifi_ok, bool cloud_ok, const char *msg)
{
    if (!s_ready) {
        s_wifi_ok = wifi_ok;
        s_cloud_ok = cloud_ok;
        return;
    }
    DisplayLockGuard lock(disp());
    if (!lock) {
        return;
    }
    refresh_status_icons(wifi_ok, cloud_ok, msg);
}

extern "C" void nodelio_panel_ui_note_activity(void)
{
    Board::GetInstance().NoteUserActivity();
}

extern "C" bool nodelio_panel_ui_screen_on(void)
{
    return s_screen_on;
}

extern "C" void nodelio_panel_ui_set_screen_cb(nodelio_panel_ui_screen_cb_t cb)
{
    s_screen_cb = cb;
}

extern "C" void nodelio_panel_ui_notify_screen(bool on)
{
    if (on == s_screen_on) {
        return;
    }
    s_screen_on = on;
    if (s_activity_tmr) {
        if (on) {
            lv_timer_resume(s_activity_tmr);
        } else {
            lv_timer_pause(s_activity_tmr);
        }
    }
    status_poll_set_period(on);
    if (s_screen_cb) {
        s_screen_cb(on);
    }
}

extern "C" void nodelio_panel_ui_toast(const char *text) { toast(text); }
extern "C" void nodelio_panel_ui_toast_defer(const char *text) { toast(text); }
extern "C" void nodelio_panel_ui_set_voice_hint(const char *hint)
{
    DisplayLockGuard lock(disp());
    if (lock) {
        nodelio_overlay_on_status_locked(hint ? hint : "待命");
    }
}
extern "C" void nodelio_panel_ui_append_chat(const char *role, const char *text)
{
    DisplayLockGuard lock(disp());
    if (lock) {
        nodelio_overlay_on_chat_locked(role, text);
    }
}
extern "C" void nodelio_panel_ui_stream_assistant(const char *delta_or_full, bool, bool done)
{
    if (!done || !delta_or_full) {
        return;
    }
    DisplayLockGuard lock(disp());
    if (lock) {
        nodelio_overlay_on_chat_locked("assistant", delta_or_full);
    }
}
extern "C" const nodelio_panel_layout_t *nodelio_panel_ui_get_layout(void)
{
    return s_have_layout ? s_layout : nullptr;
}

extern "C" bool nodelio_panel_ui_device_online(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return true;
    }
    nl_dev_cache_t *d = find_dev(device_id);
    if (!d) {
        return true; /* snapshot 未列 → 不拦 */
    }
    return d->online;
}

extern "C" void nodelio_panel_ui_snapshot_begin(void)
{
    s_dev_n = 0;
}

extern "C" void nodelio_panel_ui_snapshot_add(const char *device_id, const char *name, bool online,
                                              const char *power_state)
{
    ensure_devs_cache();
    if (!s_devs || !device_id || !device_id[0] || s_dev_n >= NL_DEV_CACHE_MAX) {
        return;
    }
    nl_dev_cache_t *d = &s_devs[s_dev_n++];
    std::memset(d, 0, sizeof(*d));
    std::snprintf(d->id, sizeof(d->id), "%s", device_id);
    if (name && name[0]) {
        std::snprintf(d->name, sizeof(d->name), "%s", name);
    }
    d->online = online;
    if (power_state && power_state[0]) {
        std::snprintf(d->power, sizeof(d->power), "%s", power_state);
    }
}

extern "C" void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index,
                                                      const char *ch_name, const char *state)
{
    nl_dev_cache_t *d = find_dev(device_id);
    if (!d || !state || channel_index < 0 || d->ch_n >= NL_CH_PER_DEV) {
        return;
    }
    d->ch_idx[d->ch_n] = (int16_t)channel_index;
    if (ch_name && ch_name[0]) {
        std::snprintf(d->ch_name[d->ch_n], sizeof(d->ch_name[0]), "%s", ch_name);
    }
    std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", state);
    d->ch_n++;
}

extern "C" void nodelio_panel_ui_snapshot_end(void)
{
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(disp());
    if (!lock) {
        return;
    }
    refresh_home_tiles();
}

extern "C" void nodelio_panel_ui_apply_reachability(const char *const *device_ids, const bool *online,
                                                    int n)
{
    nodelio_panel_ui_snapshot_begin();
    if (device_ids && online && n > 0) {
        for (int i = 0; i < n; i++) {
            nodelio_panel_ui_snapshot_add(device_ids[i], "", online[i], "");
        }
    }
    nodelio_panel_ui_snapshot_end();
}

extern "C" int nodelio_panel_ui_get_dev_count(void)
{
    return s_dev_n;
}

extern "C" const nl_dev_cache_t *nodelio_panel_ui_get_dev(int index)
{
    ensure_devs_cache();
    if (!s_devs || index < 0 || index >= s_dev_n) {
        return nullptr;
    }
    return &s_devs[index];
}
