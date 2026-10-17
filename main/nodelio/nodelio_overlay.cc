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
#include "panel_layout.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_provision.h"
#include "nl_build_stamp.h"
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

const lv_font_t *nl_ov_ui_font(void)
{
    return &font_puhui_14_1;
}

void nl_ov_set_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, nl_ov_ui_font(), 0);
    }
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

/** 顶栏左挂：VYYYYMMDD（本机编译日；见 nl_build_stamp.h，不读 esp_app_desc） */
static void fill_build_stamp(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
#ifdef NL_BUILD_STAMP
    std::snprintf(out, out_len, "%s", NL_BUILD_STAMP);
#else
    std::snprintf(out, out_len, "V-");
#endif
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
}

void nl_ov_refresh_status_icons(bool wifi_ok, bool cloud_ok, const char *msg)
{
    s_wifi_ok = wifi_ok;
    s_cloud_ok = cloud_ok;
    if (s_wifi_lbl) {
        lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI);
        nl_ov_set_icon_font(s_wifi_lbl);
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
        nl_ov_ai_farewell_closing()) {
        return;
    }
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_drawer);
    if (s_ai_hide_timer) {
        lv_timer_del(s_ai_hide_timer);
        s_ai_hide_timer = nullptr;
    }
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
        nl_ov_apps_refresh();
    }
    /* 切 Tab 不再藏小智；听/说中保持底部胶囊 */
    nl_ov_restore_ai_drawer_if_session();
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
    lv_label_set_text(s_wifi_lbl, LV_SYMBOL_WIFI);
    nl_ov_set_icon_font(s_wifi_lbl);
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

    s_tab_btns[0] = mk_tab(s_tabbar, "首页", NL_TAB_HOME);
    s_tab_btns[1] = mk_tab(s_tabbar, "应用", NL_TAB_APPS);
    s_tab_btns[2] = mk_tab(s_tabbar, "设置", NL_TAB_SET);

    /* 全局 AI 对话浮层抽屉（唤醒词触发，不依赖 Tab，挂在 lv_layer_top()） */
    nl_ov_create_ai_drawer(lv_layer_top());

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

    nl_ov_fill_home();
    /* removed */
    nl_ov_fill_settings();
    nl_ov_build_apps();
    nl_ov_build_connect();
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
    Board::GetInstance().SetIdleSleepSeconds((int)nodelio_idle_sleep_seconds());
    Board::GetInstance().SetIdleWifiSeconds((int)nodelio_idle_wifi_seconds());
    Board::GetInstance().SetPowerSaveEnabled(nodelio_power_save_enabled());
    DisplayLockGuard lock(nl_ov_disp());
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
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            /* 有多轮记录时保留正文，勿每次 Listening 用占位语盖掉 */
            nl_ov_show_ai_drawer_session(CHAT_LISTEN);
        }
        return;
    }
    nodelio_overlay_show(false);
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

