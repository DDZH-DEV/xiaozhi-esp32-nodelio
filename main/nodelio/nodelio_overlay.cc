#include "nodelio_overlay.h"
#include "overlay_priv.h"

#include "application.h"
#include "board_info.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
#include "nl_url_util.h"
#include "nodelio_bridge.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "offline_cmd.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_provision.h"
#if __has_include("nodelio_build_stamp.h")
#include "nodelio_build_stamp.h"
#endif
#ifndef NODELIO_BUILD_STAMP
#define NODELIO_BUILD_STAMP "0.0.0"
#endif
#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "display.h"
#include "lvgl_font.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_manager.h"

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <lvgl.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_overlay"

nodelio_nvs_config_t s_cfg;
nodelio_panel_layout_t *s_layout;
bool s_have_layout;
bool s_fab_ready;
bool s_ready;
bool s_visible;
bool s_skill_ui_active;
bool s_building;
bool s_want_settings;
bool s_want_chat;
bool s_https;
bool s_wifi_ok;
bool s_cloud_ok;

lv_obj_t *s_fab;
lv_obj_t *s_root;
lv_obj_t *s_body;
lv_obj_t *s_home;
lv_obj_t *s_eq_bars[3];
lv_obj_t *s_ai_drawer;           /* 底部居中 AI 悬浮胶囊 */
lv_obj_t *s_ai_drawer_status;    /* 状态小标题（如 "小智在听…" / "小智回答"） */
lv_obj_t *s_ai_drawer_body;      /* 正文滚动容器 */
lv_obj_t *s_ai_drawer_text;      /* 对话正文：最近 5 句 */
lv_obj_t *s_ai_mini_fab;         /* 最小化圆钮（仅图标） */
lv_timer_t *s_ai_hide_timer;     /* 会话闲置自动收起定时器 */
char s_ai_show_text[NL_CHAT_SHOW_CAP]; /* 抽屉展示缓冲 */
lv_obj_t *s_settings;
lv_obj_t *s_tabbar;
lv_obj_t *s_tab_btns[NL_TAB_N]; /* 0=首页 1=应用 2=设置 */
lv_obj_t *s_brand_lbl;
lv_obj_t *s_clock_lbl;
lv_obj_t *s_status_lbl;
lv_obj_t *s_wifi_lbl;
lv_obj_t *s_cloud_lbl;
lv_obj_t *s_status_bar;
esp_timer_handle_t s_status_tmr;
lv_timer_t *s_activity_tmr;
char s_clock_last[8];
bool s_screen_on = true;
nodelio_panel_ui_screen_cb_t s_screen_cb;
nodelio_panel_ui_net_cb_t s_net_cb;

lv_obj_t *s_settings_detail;
lv_obj_t *s_settings_detail_body;
lv_obj_t *s_bri_val_lbl;
lv_obj_t *s_vol_val_lbl;
uint8_t s_brightness = 80;
uint8_t s_volume = 70;
lv_timer_t *s_sys_timer;
lv_obj_t *s_sys_up_lbl;
lv_obj_t *s_sys_dram_lbl;
lv_obj_t *s_sys_psram_lbl;
lv_obj_t *s_sys_tasks_lbl;
char s_sys_last_up[40];
char s_sys_last_dram[24];
char s_sys_last_psram[24];
char s_sys_last_tasks[16];

lv_obj_t *s_connect;
lv_obj_t *s_connect_scroll;
lv_obj_t *s_kb;
lv_obj_t *s_ta_host;
lv_obj_t *s_ta_port;
lv_obj_t *s_ta_owner;
lv_obj_t *s_proto_lbl;
lv_obj_t *s_ta_ssid;
lv_obj_t *s_ta_pass;
lv_obj_t *s_wifi_list;
lv_obj_t *s_scan_btn_lbl;
nodelio_wifi_ap_t s_scan_aps[NL_WIFI_SCAN_MAX];
int s_scan_n;
bool s_wifi_scanning;
int s_grid_ox;
int s_grid_oy;
int s_active_tab; /* NL_TAB_HOME / APPS / SET */

chat_phase_t s_chat_phase = CHAT_IDLE;
char s_chat_text[NL_CHAT_CAP];

nl_dev_cache_t *s_devs = nullptr;
int s_dev_n = 0;

void nl_ov_ensure_devs_cache()
{
    if (!s_devs) {
        s_devs = (nl_dev_cache_t *)heap_caps_calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_devs) {
            s_devs = (nl_dev_cache_t *)calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t));
        }
    }
}
lv_style_transition_dsc_t s_tile_trans;
bool s_tile_trans_inited;

Display *nl_ov_disp()
{
    return Board::GetInstance().GetDisplay();
}

static const lv_font_t *ui_font_base(void)
{
    return &font_puhui_14_1;
}

const lv_font_t *nl_ov_ui_font_compact(void)
{
    return ui_font_base();
}

static const lv_font_t *ui_font_large(void)
{
    Display *d = nl_ov_disp();
    if (!d) {
        return ui_font_base();
    }
    Theme *th = d->GetTheme();
    if (!th) {
        return ui_font_base();
    }
    std::shared_ptr<LvglFont> tf = th->GetTextFont();
    if (!tf || tf->font() == nullptr) {
        return ui_font_base();
    }
    return tf->font();
}

const lv_font_t *nl_ov_ui_font(void)
{
    /* 0=14px 普惠；1/2=资源真字库（约 20px），禁止 transform 放大点阵 */
    return nodelio_text_scale() == 0 ? ui_font_base() : ui_font_large();
}

const lv_font_t *nl_ov_ui_font_title(void)
{
    return nl_ov_ui_font();
}

const lv_font_t *nl_ov_read_font(void)
{
    return ui_font_large();
}

static int32_t ui_line_space(void)
{
    switch (nodelio_text_scale()) {
    case 2:
        return 8;
    case 1:
        return 4;
    default:
        return 0;
    }
}

static int32_t ui_letter_space(void)
{
    return nodelio_text_scale() >= 2 ? 1 : 0;
}

int nl_ov_ui_line_box(void)
{
    const lv_font_t *f = nl_ov_ui_font();
    const int32_t lh = f ? lv_font_get_line_height(f) : 18;
    return (int)(lh + ui_line_space());
}

int nl_ov_ui_font_line_h(void)
{
    const lv_font_t *f = nl_ov_ui_font();
    return f ? (int)lv_font_get_line_height(f) : 18;
}

int nl_ov_wrapped_text_h(const char *str, int max_w)
{
    const lv_font_t *font = nl_ov_ui_font();
    const int font_h = nl_ov_ui_font_line_h();
    const int lsp = (int)ui_line_space();
    if (!str || !str[0] || !font || max_w < 8) {
        return font_h;
    }
    lv_point_t sz;
    lv_text_get_size(&sz, str, font, ui_letter_space(), lsp, max_w, LV_TEXT_FLAG_NONE);
    if (sz.y <= font_h + 1) {
        return font_h;
    }
    return font_h * 2 + lsp;
}

void nl_ov_set_font(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_text_font(obj, nl_ov_ui_font(), 0);
    lv_obj_set_style_text_line_space(obj, ui_line_space(), 0);
    lv_obj_set_style_text_letter_space(obj, ui_letter_space(), 0);
}

void nl_ov_set_font_title(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_text_font(obj, nl_ov_ui_font_title(), 0);
    lv_obj_set_style_text_line_space(obj, ui_line_space() > 4 ? 4 : ui_line_space(), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
}

static void status_bar_relayout(void);

static void apply_font_if(lv_obj_t *obj)
{
    if (obj) {
        nl_ov_set_font(obj);
    }
}

static void retarget_font_tree(lv_obj_t *obj, const lv_font_t *from, const lv_font_t *to)
{
    if (!obj) {
        return;
    }
    static const lv_style_selector_t k_sel[] = {0, LV_PART_TEXTAREA_PLACEHOLDER};
    for (lv_style_selector_t sel : k_sel) {
        lv_style_value_t v;
        if (lv_obj_get_local_style_prop(obj, LV_STYLE_TEXT_FONT, &v, sel) == LV_STYLE_RES_FOUND &&
            v.ptr == from) {
            lv_obj_set_style_text_font(obj, to, sel);
        }
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        retarget_font_tree(lv_obj_get_child(obj, i), from, to);
    }
}

extern "C" void nodelio_overlay_retarget_font(const void *from_font, const void *to_font)
{
    const lv_font_t *from = static_cast<const lv_font_t *>(from_font);
    const lv_font_t *to = static_cast<const lv_font_t *>(to_font);
    if (!from || !to || from == to) {
        return;
    }
    /* 首页在 layer_top；小智底屏的 screen 也单独设过 text font。两边都要改完再销毁旧字库。 */
    retarget_font_tree(lv_screen_active(), from, to);
    retarget_font_tree(lv_layer_top(), from, to);
}

void nl_ov_apply_text_scale(void)
{
    if (s_root) {
        lv_obj_set_style_text_font(s_root, nl_ov_ui_font(), 0);
    }
    apply_font_if(s_brand_lbl);
    apply_font_if(s_clock_lbl);
    apply_font_if(s_status_lbl);
    status_bar_relayout();
    for (int i = 0; i < NL_TAB_N; i++) {
        lv_obj_t *btn = s_tab_btns[i];
        if (!btn) {
            continue;
        }
        lv_obj_set_style_text_font(btn, nl_ov_ui_font(), 0);
        uint32_t n = lv_obj_get_child_count(btn);
        for (uint32_t c = 0; c < n; c++) {
            lv_obj_t *ch = lv_obj_get_child(btn, c);
            if (ch && lv_obj_check_type(ch, &lv_label_class)) {
                nl_ov_set_font(ch);
            }
        }
    }
    apply_font_if(s_ai_drawer_status);
    apply_font_if(s_ai_drawer_text);
    if (s_have_layout && s_home) {
        nl_ov_fill_home();
    }
    nl_ov_apps_relayout();
    nl_ov_settings_reapply_text_scale();
    nodelio_skill_result_reapply_text_scale();
}

void nl_ov_set_icon_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    }
}

/** 首页磁贴矢量图标字库（非横条全用 30px 超大矢量图标，横条用 20px） */
const lv_font_t *nl_ov_tile_icon_font(bool is_row)
{
    return is_row ? &font_material_symbols_20_4 : &font_material_symbols_30_4;
}

/* 字体视觉重心偏上：统一 +1；左右留白对称 */
#define NL_STATUS_Y 1
#define NL_STATUS_LEFT_PAD 12
#define NL_STATUS_RIGHT_PAD (-12)

/** 顶栏左挂：与设置页「构建号」、OTA 同一套 NODELIO_BUILD_STAMP */
static void fill_build_stamp(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    std::snprintf(out, out_len, "V%s", NODELIO_BUILD_STAMP);
}

static void status_bar_relayout(void)
{
    if (s_brand_lbl) {
        lv_obj_align(s_brand_lbl, LV_ALIGN_LEFT_MID, NL_STATUS_LEFT_PAD, NL_STATUS_Y);
    }
    if (s_status_lbl && s_brand_lbl) {
        lv_obj_align_to(s_status_lbl, s_brand_lbl, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }
    /* 右：时钟 · WiFi · 云，时钟贴图标只留一点空 */
    if (s_cloud_lbl) {
        lv_obj_align(s_cloud_lbl, LV_ALIGN_RIGHT_MID, NL_STATUS_RIGHT_PAD, NL_STATUS_Y);
    }
    if (s_wifi_lbl && s_cloud_lbl) {
        lv_obj_align_to(s_wifi_lbl, s_cloud_lbl, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
    if (s_clock_lbl && s_wifi_lbl) {
        lv_obj_align_to(s_clock_lbl, s_wifi_lbl, LV_ALIGN_OUT_LEFT_MID, -4, 0);
    } else if (s_clock_lbl) {
        lv_obj_align(s_clock_lbl, LV_ALIGN_RIGHT_MID, NL_STATUS_RIGHT_PAD - 40, NL_STATUS_Y);
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
    nodelio_skill_daily_clock_tick();
}

/** 按 RSSI 选格数；阈值与 WifiBoard::GetNetworkStateIcon 一致。未关联不读 AP info。 */
static const char *wifi_status_glyph(bool wifi_ok)
{
    if (!wifi_ok || nodelio_wifi_is_asleep()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }
    int rssi = WifiManager::GetInstance().GetRssi();
    if (rssi >= -65 || rssi == 0) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

void nl_ov_refresh_status_icons(bool wifi_ok, bool cloud_ok, const char *msg)
{
    s_wifi_ok = wifi_ok;
    s_cloud_ok = cloud_ok;
    if (s_wifi_lbl) {
        lv_obj_set_style_text_font(s_wifi_lbl, &font_material_symbols_20_4, 0);
        lv_label_set_text(s_wifi_lbl, wifi_status_glyph(wifi_ok));
        lv_obj_set_style_text_color(s_wifi_lbl, wifi_ok ? UI_ACCENT : UI_TEXT_MUTED, 0);
    }
    if (s_cloud_lbl) {
        /* 无独立云符号：UPLOAD≈云同步 */
        lv_label_set_text(s_cloud_lbl, LV_SYMBOL_UPLOAD);
        nl_ov_set_icon_font(s_cloud_lbl);
        lv_obj_set_style_text_color(s_cloud_lbl, cloud_ok ? UI_ACCENT : UI_TEXT_MUTED, 0);
    }
    /* msg 不再刷布局版本；左挂编译戳开机写死，避免断网变 v0 */
    (void)msg;
    refresh_clock_lbl();
    status_bar_relayout();
}

static void status_poll_cb(void *arg)
{
    (void)arg;
    if (!s_ready || !s_screen_on) {
        return;
    }
    /* 说话/建链时跳过状态栏刷新，减轻 RGB 全刷与 Opus 抢 PSRAM */
    if (nodelio_voice_session_active()) {
        return;
    }
    /* 勿 esp_wifi_sta_get_ap_info：未关联时 IDF 会刷 Haven't to connect…，像抢网 */
    bool wifi_ok = nodelio_wifi_has_ip();
    bool cloud_ok = nodelio_mqtt_panel_connected();
    if (!cloud_ok && s_cfg.mqtt_server[0] == '\0') {
        /* 未配 MQTT 时用最近业务成功作「云」 */
        cloud_ok = s_cloud_ok;
    }
    nodelio_panel_ui_set_status(wifi_ok, cloud_ok, nullptr);
}

void nl_ov_status_poll_set_period(bool awake)
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
    nl_ov_status_poll_set_period(true);
    status_poll_cb(nullptr);
}

void nl_ov_toast(const char *msg)
{
    if (!nl_ov_disp()) {
        return;
    }
    /* 小智状态栏在墙板底下，设置页看不见。提示挂在最上层。msg 空则只收起。 */
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    static lv_obj_t *toast;
    static lv_obj_t *toast_lbl;
    static lv_timer_t *hide_tmr;
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
        lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -(NL_UI_TAB_H + 16));
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
        toast_lbl = lv_label_create(toast);
        lv_label_set_long_mode(toast_lbl, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(toast_lbl);
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
    const lv_font_t *font = lv_obj_get_style_text_font(toast_lbl, LV_PART_MAIN);
    const int32_t max_inner = LV_HOR_RES - 120;
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
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -(NL_UI_TAB_H + 16));
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    if (hide_tmr) {
        lv_timer_del(hide_tmr);
        hide_tmr = nullptr;
    }
    hide_tmr = lv_timer_create(
        [](lv_timer_t *t) {
            (void)t;
            if (toast) {
                lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
            }
            hide_tmr = nullptr;
        },
        2500, nullptr);
    if (hide_tmr) {
        lv_timer_set_repeat_count(hide_tmr, 1);
    }
}

bool nl_ov_ai_session_active(void)
{
    auto state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateListening || state == kDeviceStateSpeaking;
}

/** 会话进行中：抽屉保持可见并置顶（切 Tab / 关表单后仍找得到小智） */
void nl_ov_restore_ai_drawer_if_session(void)
{
    if (!s_ai_drawer || s_skill_ui_active || !nl_ov_ai_session_active() ||
        nl_ov_ai_farewell_closing() || nl_ov_ai_user_dismissed()) {
        return;
    }
    if (s_ai_hide_timer) {
        lv_timer_del(s_ai_hide_timer);
        s_ai_hide_timer = nullptr;
    }
    if (nl_ov_ai_is_minimized()) {
        nl_ov_ai_raise_mini_fab();
        return;
    }
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_drawer);
}

void nl_ov_close_forms(void)
{
    nl_ov_kb_set_visible(false);
    /* 勿在此藏 AI 抽屉：切首页/设置会走 close_forms，会话中应继续露着 */
    nl_ov_close_settings_detail();
    if (s_connect) {
        lv_obj_add_flag(s_connect, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tabbar && s_visible) {
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_restore_ai_drawer_if_session();
}
static bool nl_ov_obj_shown(lv_obj_t *o)
{
    return o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static bool nl_ov_obj_scrollable(lv_obj_t *o)
{
    return o && lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/** 优先最上层浮层：设置内页 → 连接 → 应用列表 → 设置首页 → 首页启动台 */
static lv_obj_t *nl_ov_find_voice_scroll_target(void)
{
    /* 浮层盖住下层时不穿透滚下层 */
    if (nl_ov_obj_shown(s_settings_detail)) {
        return nl_ov_obj_scrollable(s_settings_detail_body) ? s_settings_detail_body : nullptr;
    }
    if (nl_ov_obj_shown(s_connect)) {
        return nl_ov_obj_scrollable(s_connect_scroll) ? s_connect_scroll : nullptr;
    }
    lv_obj_t *apps = nl_ov_apps_scroll_obj();
    if (apps) {
        return apps;
    }
    if (s_active_tab == NL_TAB_SET && nl_ov_obj_shown(s_settings) &&
        nl_ov_obj_scrollable(s_settings)) {
        return s_settings;
    }
    if (s_active_tab == NL_TAB_HOME && nl_ov_obj_shown(s_home) && nl_ov_obj_scrollable(s_home)) {
        return s_home;
    }
    return nullptr;
}

esp_err_t nl_ov_voice_scroll_peek(int dir)
{
    if (dir == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    lv_obj_t *t = nl_ov_find_voice_scroll_target();
    if (!t) {
        return ESP_ERR_NOT_FOUND;
    }
    if (dir < 0) {
        return lv_obj_get_scroll_top(t) > 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return lv_obj_get_scroll_bottom(t) > 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void nl_ov_voice_scroll_apply(int dir)
{
    if (dir == 0) {
        return;
    }
    lv_obj_t *t = nl_ov_find_voice_scroll_target();
    if (!t) {
        return;
    }
    lv_coord_t room = (dir < 0) ? lv_obj_get_scroll_top(t) : lv_obj_get_scroll_bottom(t);
    if (room <= 0) {
        return;
    }
    lv_coord_t step = lv_obj_get_content_height(t);
    if (step < 120) {
        step = 200;
    } else {
        step = (step * 3) / 4;
    }
    if (step > room) {
        step = room;
    }
    lv_coord_t y = lv_obj_get_scroll_y(t);
    if (dir < 0) {
        y -= step;
        if (y < 0) {
            y = 0;
        }
    } else {
        y += step;
    }
    lv_obj_scroll_to_y(t, y, LV_ANIM_ON);
}

void nl_ov_show_page(int tab)
{
    s_active_tab = tab;
    s_want_chat = false;
    if (tab != NL_TAB_SET) {
        s_want_settings = false;
    }
    nl_ov_close_forms();
    if (s_home) {
        if (tab == NL_TAB_HOME) {
            lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
            /* 应用页刷新后可能 invalidate 钉选；回首页再拉一次（无定时器） */
            nl_ov_home_skills_ensure_fetched();
        } else {
            lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        }
    }
    nl_ov_apps_set_visible(tab == NL_TAB_APPS);
    if (s_settings) {
        if (tab == NL_TAB_SET) {
            lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < NL_TAB_N; i++) {
        if (s_tab_btns[i]) {
            ui_tab_set_checked(s_tab_btns[i], i == tab);
        }
    }
    if (tab == NL_TAB_APPS) {
        nl_ov_apps_ensure();
    }
    /* 切 Tab 不再藏小智；听/说中保持底部胶囊 */
    nl_ov_restore_ai_drawer_if_session();
    /* 播放器/曲库与 s_root 同层：切页后抬回最前，避免点到应用卡 */
    nodelio_skill_result_layout();
    nodelio_skill_result_raise();
}

void nl_ov_workspace_reserve(int right, int bottom) {
    const auto* p = nodelio_display_profile();
    if (!p->nav_width || !s_body)
        return;
    const int w = p->width - p->nav_width - right;
    const int h = p->height - NL_UI_STATUS_H - bottom;
    bool home_changed = s_home && (lv_obj_get_width(s_home) != w || lv_obj_get_height(s_home) != h);
    const bool settings_w_changed = s_settings && lv_obj_get_width(s_settings) != w;
    if (s_home)
        lv_obj_set_size(s_home, w, h);
    if (s_settings)
        lv_obj_set_size(s_settings, w, h);
    if (s_settings_detail) {
        lv_obj_set_size(s_settings_detail, w, h);
        lv_obj_align(s_settings_detail, LV_ALIGN_TOP_LEFT, p->nav_width, NL_UI_STATUS_H);
    }
    nl_ov_apps_reserve_bottom(bottom);
    if (home_changed && s_home && s_have_layout && !s_building) {
        lv_obj_update_layout(s_home);
        nl_ov_fill_home();
    }
    if (settings_w_changed && s_settings && !s_building) {
        lv_obj_update_layout(s_settings);
        nl_ov_settings_sync_columns();
    }
}

static bool s_boot_bl_revealed = false;

static void reveal_boot_backlight(void)
{
    if (s_boot_bl_revealed) {
        return;
    }
    s_boot_bl_revealed = true;
    auto *bl = Board::GetInstance().GetBacklight();
    if (bl) {
        bl->RestoreBrightness();
    }
}

/* 持 LVGL 锁：先把首页刷进 RGB FB，再开背光，避免亮着垃圾帧。 */
static void reveal_boot_backlight_after_frame(void)
{
    if (s_boot_bl_revealed) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_refr_now(disp);
    }
    reveal_boot_backlight();
}

static void apply_visible_locked(void)
{
    if (!s_root) {
        return;
    }
    if (s_want_settings) {
        nl_ov_show_page(NL_TAB_SET);
    } else {
        nl_ov_show_page(0);
        if (s_want_chat) {
            nl_ov_show_ai_drawer_session(CHAT_LISTEN);
        }
    }
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    if (s_fab) {
        lv_obj_add_flag(s_fab, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(s_root);
    nodelio_skill_result_raise();
    s_visible = true;
    /* 必须在 root 置顶之后再开连接页，否则会被不透明的 s_root 盖住 */
    if (s_want_settings) {
        nl_ov_open_connect();
    }
}

static lv_obj_t *mk_tab(lv_obj_t *bar, const char *txt, int id)
{
    lv_obj_t *b = ui_make_tab(bar, txt);
    ui_tab_set_checked(b, false);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
            int id = (int)(intptr_t)lv_event_get_user_data(e);
            nl_ov_show_page(id);
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)id);
    return b;
}

static void build_shell_on_lvgl(void * /*user_data*/)
{
    s_building = false;
    if (s_ready) {
        apply_visible_locked();
        reveal_boot_backlight_after_frame();
        return;
    }

    ui_styles_init();

    s_root = lv_obj_create(lv_layer_top());
    if (!s_root) {
        ESP_LOGE(TAG, "root create fail");
        reveal_boot_backlight_after_frame();
        return;
    }
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_root, nl_ov_ui_font(), 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_pad_row(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    /* top chrome：左品牌+编译戳 · 右 时钟/WiFi/云 */
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
    nl_ov_set_font(s_brand_lbl);
    lv_obj_set_style_text_color(s_brand_lbl, UI_ACCENT, 0);
    lv_obj_add_flag(s_brand_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_brand_lbl, 16);
    lv_obj_add_event_cb(s_brand_lbl, nl_ov_on_brand_click, LV_EVENT_CLICKED, nullptr);

    s_clock_lbl = lv_label_create(bar);
    lv_label_set_text(s_clock_lbl, "--:--");
    s_clock_last[0] = '\0';
    nl_ov_set_font(s_clock_lbl);
    lv_obj_set_style_text_color(s_clock_lbl, UI_TEXT_SUB, 0);

    char build_stamp[40];
    fill_build_stamp(build_stamp, sizeof(build_stamp));
    s_status_lbl = lv_label_create(bar);
    lv_label_set_text(s_status_lbl, build_stamp);
    lv_label_set_long_mode(s_status_lbl, LV_LABEL_LONG_CLIP);
    nl_ov_set_font(s_status_lbl);
    lv_obj_set_style_text_color(s_status_lbl, UI_STATUS_CHROME, 0);

    s_cloud_lbl = lv_label_create(bar);
    lv_label_set_text(s_cloud_lbl, LV_SYMBOL_UPLOAD);
    nl_ov_set_icon_font(s_cloud_lbl);
    lv_obj_set_style_text_color(s_cloud_lbl, UI_TEXT_MUTED, 0);

    s_wifi_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(s_wifi_lbl, &font_material_symbols_20_4, 0);
    lv_label_set_text(s_wifi_lbl, MATERIAL_SYMBOLS_WIFI_OFF);
    lv_obj_set_style_text_color(s_wifi_lbl, UI_TEXT_MUTED, 0);
    status_bar_relayout();

    // Wide panels use a left rail; square panels retain their bottom tabs.
    const auto* profile = nodelio_display_profile();
    lv_obj_t* workspace = s_root;
    if (profile->nav_width) {
        workspace = lv_obj_create(s_root);
        lv_obj_remove_style_all(workspace);
        lv_obj_set_width(workspace, lv_pct(100));
        lv_obj_set_flex_grow(workspace, 1);
        lv_obj_set_flex_flow(workspace, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(workspace, LV_OBJ_FLAG_SCROLLABLE);
    }
    /* body */
    s_body = lv_obj_create(workspace);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1);
    if (profile->nav_width) {
        lv_obj_set_width(s_body, 0);
        lv_obj_set_height(s_body, lv_pct(100));
    }
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    s_home = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_home, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);

    s_settings = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_settings, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_HIDDEN);

    /* bottom tabs — custom, not lv_tabview */
    s_tabbar = lv_obj_create(workspace);
    lv_obj_remove_style_all(s_tabbar);
    lv_obj_set_size(s_tabbar, lv_pct(100), NL_UI_TAB_H);
    lv_obj_set_style_bg_opa(s_tabbar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_tabbar, 4, 0);
    lv_obj_set_style_pad_column(s_tabbar, 4, 0);
    lv_obj_set_flex_flow(s_tabbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tabbar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_SCROLLABLE);

    s_tab_btns[0] = mk_tab(s_tabbar, "首页", NL_TAB_HOME);
    s_tab_btns[1] = mk_tab(s_tabbar, "应用", NL_TAB_APPS);
    s_tab_btns[2] = mk_tab(s_tabbar, "设置", NL_TAB_SET);

    if (profile->nav_width) {
        lv_obj_move_to_index(s_tabbar, 0);
        lv_obj_set_size(s_tabbar, profile->nav_width, lv_pct(100));
        lv_obj_set_flex_flow(s_tabbar, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_tabbar, UI_GAP, 0);
        for (auto* button : s_tab_btns) {
            lv_obj_set_flex_grow(button, 0);
            lv_obj_set_size(button, lv_pct(100), UI_BTN_H);
            lv_obj_set_ext_click_area(button, 0);
        }
    }

    /* 全局 AI 对话浮层抽屉（唤醒词触发，不依赖 Tab，挂在 lv_layer_top()） */
    nl_ov_create_ai_drawer(lv_layer_top());

    /* 无云 snapshot 时先灌上次局域网 IP，本地口令可走 DIOL */
    nodelio_panel_ui_lan_cache_load();

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
                nodelio_offline_cmd_rebuild_from_layout(s_layout);
                ESP_LOGI(TAG, "boot loaded cached layout v%d for instant render",
                         s_layout->layout_version);
            }
            free(cached);
        }
    }

    nl_ov_fill_home();
    if (s_have_layout && s_layout && s_layout->layout_version > 0) {
        nodelio_panel_ui_mark_home_filled(s_layout->layout_version);
    }
    nl_ov_fill_settings();
    nl_ov_build_apps();
    nl_ov_build_connect();
    /* WiFi 扫描已并入「连接」页；不再单独建 WiFi 页，避免覆盖 s_ta_ssid 等指针 */

    s_ready = true;
    apply_visible_locked();
    reveal_boot_backlight_after_frame();
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
    nl_ov_set_font(fl);
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
    nodelio_conn_hist_t history{};
    (void)nodelio_conn_hist_load(&history);
    if (s_cfg.volume <= 100) {
        s_volume = s_cfg.volume;
    }
    Board::GetInstance().SetIdleSleepSeconds((int)nodelio_idle_sleep_seconds());
    Board::GetInstance().SetIdleWifiSeconds((int)nodelio_idle_wifi_seconds());
    Board::GetInstance().SetPowerSaveEnabled(nodelio_power_save_enabled());
    /* AudioCodec::Start 读的是 Settings/audio；面板音量在 nodelio NVS，开机对齐一次 */
    if (auto *codec = Board::GetInstance().GetAudioCodec()) {
        codec->SetOutputVolumeTransient((int)s_volume);
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return ESP_FAIL;
    }
    create_fab_locked();
    ESP_LOGI(TAG, "FAB ready");
    /* 建壳失败也不让背光一直灭 */
    lv_timer_create(
        [](lv_timer_t *t) {
            lv_timer_delete(t);
            if (!s_boot_bl_revealed) {
                ESP_LOGW(TAG, "boot backlight fallback (shell not ready)");
                reveal_boot_backlight();
            }
        },
        2000, nullptr);
    return ESP_OK;
}

void nodelio_overlay_show_chat(void)
{
    s_want_settings = false;
    s_want_chat = true;

    auto &app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    /* 仅 idle/播报中才真正建链开麦；已在听/说只展开抽屉，勿 Toggle（会关通道） */
    const bool need_start = (state == kDeviceStateIdle || state == kDeviceStateNotifying);
    const chat_phase_t phase = need_start ? CHAT_CONNECT : CHAT_LISTEN;

    if (s_visible && s_ready) {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_show_ai_drawer_session(phase);
        }
    } else {
        nodelio_overlay_show(false);
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_show_ai_drawer_session(phase);
        }
    }

    if (need_start) {
        app.ToggleChatState();
    }
}

void nodelio_overlay_show_connecting(void)
{
    s_want_settings = false;
    s_want_chat = true;
    if (s_visible && s_ready) {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_show_ai_drawer_session(CHAT_CONNECT);
        }
        return;
    }
    nodelio_overlay_show(false);
}
void nl_ov_utf8_copy(char *dst, size_t dst_len, const char *src)
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
        DisplayLockGuard lock(nl_ov_disp());
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
    /* 同步建壳：开机首帧即为曜夜首页，避免 async 空隙露出小智顶栏 */
    DisplayLockGuard lock(nl_ov_disp());
    if (lock) {
        build_shell_on_lvgl(nullptr);
    } else {
        s_building = false;
        lv_async_call(build_shell_on_lvgl, nullptr);
        s_building = true;
    }
}

void nodelio_overlay_hide(void)
{
    /* 不再露出小智白底：只关子弹、留在首页 */
    if (!s_ready || !s_root) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_close_forms();
    nl_ov_show_page(0);
}

bool nodelio_overlay_visible(void)
{
    return s_visible;
}

