#pragma once

#include "nodelio_overlay.h"
#include "panel_ui.h"
#include "ui_theme.h"
#include "wifi_provision.h"

#include "display.h"

#include <esp_err.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cstdint>
#include <cstddef>

#define GRID_GAP UI_GAP

typedef enum {
    TILE_SWITCH = 0,
    TILE_SCENE,
    TILE_GPIO,
    TILE_PWM,
} tile_kind_t;

enum chat_phase_t : uint8_t { CHAT_IDLE = 0, CHAT_LISTEN, CHAT_SPEAK, CHAT_CONNECT };

enum { TILE_CHILD_ICON = 1, TILE_CHILD_TITLE = 2, TILE_CHILD_HINT = 3, TILE_CHILD_CAPSULE = 4 };

typedef enum {
    SET_PAGE_DISP = 1,
    SET_PAGE_SYS = 2,
    SET_PAGE_RESET = 3,
    SET_PAGE_VOICE = 4,
    SET_PAGE_POWER = 5,
} set_page_id_t;

#define NL_CHAT_SHOW_LINES 5
#define NL_CHAT_SHOW_CAP 1024
#define NL_CHAT_CAP 3072

/* —— shared state (defined in nodelio_overlay.cc) —— */
extern nodelio_nvs_config_t s_cfg;
extern nodelio_panel_layout_t *s_layout;
extern bool s_have_layout;
extern bool s_fab_ready;
extern bool s_ready;
extern bool s_visible;
extern bool s_building;
extern bool s_want_settings;
extern bool s_want_chat;
extern bool s_https;
extern bool s_wifi_ok;
extern bool s_cloud_ok;

extern lv_obj_t *s_fab;
extern lv_obj_t *s_root;
extern lv_obj_t *s_body;
extern lv_obj_t *s_home;
extern lv_obj_t *s_eq_bars[3];
extern lv_obj_t *s_ai_drawer;
extern lv_obj_t *s_ai_drawer_status;
extern lv_obj_t *s_ai_drawer_body;
extern lv_obj_t *s_ai_drawer_text;
extern lv_timer_t *s_ai_hide_timer;
extern char s_ai_show_text[NL_CHAT_SHOW_CAP];
extern lv_obj_t *s_settings;
extern lv_obj_t *s_tabbar;
extern lv_obj_t *s_tab_btns[2];
extern lv_obj_t *s_brand_lbl;
extern lv_obj_t *s_clock_lbl;
extern lv_obj_t *s_status_lbl;
extern lv_obj_t *s_wifi_lbl;
extern lv_obj_t *s_cloud_lbl;
extern lv_obj_t *s_status_bar;
extern esp_timer_handle_t s_status_tmr;
extern lv_timer_t *s_activity_tmr;
extern char s_clock_last[8];
extern bool s_screen_on;
extern nodelio_panel_ui_screen_cb_t s_screen_cb;
extern nodelio_panel_ui_net_cb_t s_net_cb;

extern lv_obj_t *s_settings_detail;
extern lv_obj_t *s_bri_val_lbl;
extern lv_obj_t *s_vol_val_lbl;
extern uint8_t s_brightness;
extern uint8_t s_volume;
extern lv_timer_t *s_sys_timer;
extern lv_obj_t *s_sys_up_lbl;
extern lv_obj_t *s_sys_dram_lbl;
extern lv_obj_t *s_sys_psram_lbl;
extern lv_obj_t *s_sys_tasks_lbl;
extern char s_sys_last_up[40];
extern char s_sys_last_dram[24];
extern char s_sys_last_psram[24];
extern char s_sys_last_tasks[16];

extern lv_obj_t *s_connect;
extern lv_obj_t *s_connect_scroll;
extern lv_obj_t *s_kb;
extern lv_obj_t *s_ta_host;
extern lv_obj_t *s_ta_port;
extern lv_obj_t *s_ta_owner;
extern lv_obj_t *s_proto_lbl;
extern lv_obj_t *s_ta_ssid;
extern lv_obj_t *s_ta_pass;
extern lv_obj_t *s_wifi_list;
extern lv_obj_t *s_scan_btn_lbl;
extern nodelio_wifi_ap_t s_scan_aps[NL_WIFI_SCAN_MAX];
extern int s_scan_n;
extern bool s_wifi_scanning;
extern int s_grid_ox;
extern int s_grid_oy;
extern int s_active_tab;

extern chat_phase_t s_chat_phase;
extern char s_chat_text[NL_CHAT_CAP];

extern nl_dev_cache_t *s_devs;
extern int s_dev_n;
extern lv_style_transition_dsc_t s_tile_trans;
extern bool s_tile_trans_inited;

/* —— shell helpers —— */
Display *nl_ov_disp();
void nl_ov_utf8_copy(char *dst, size_t dst_len, const char *src);
void nl_ov_set_font(lv_obj_t *obj);
void nl_ov_set_icon_font(lv_obj_t *obj);
const lv_font_t *nl_ov_tile_icon_font(bool is_row);
void nl_ov_toast(const char *msg);
void nl_ov_refresh_status_icons(bool wifi_ok, bool cloud_ok, const char *msg);
void nl_ov_status_poll_set_period(bool awake);
void nl_ov_show_page(int tab);
void nl_ov_close_forms(void);
void nl_ov_ensure_devs_cache(void);

/* —— home tiles —— */
void nl_ov_fill_home(void);
void nl_ov_refresh_home_tiles(void);
nl_dev_cache_t *nl_ov_find_dev(const char *device_id);
/** 本地乐观写缓存（不刷 LVGL）；语音会话中 panel/command 成功后用，免等 MQTT snapshot */
void nl_ov_apply_bind_state(const nl_bind_t *bind, const char *value);
/** channel 为负=设备级；秒小于等于 0 清除；内部确保本地秒表 tick */
void nl_ov_set_countdown(const char *device_id, int channel_index, int seconds);
/** 读绑定磁贴当前剩余秒（无则 0） */
int nl_ov_widget_countdown(const nl_widget_t *w);
void nl_ov_on_brand_click(lv_event_t *e);

/* —— connect —— */
void nl_ov_build_connect(void);
void nl_ov_open_connect(void);
void nl_ov_close_connect(void);
void nl_ov_kb_set_visible(bool on);

/* —— settings —— */
void nl_ov_fill_settings(void);
void nl_ov_close_settings_detail(void);
void nl_ov_stop_sys_monitor(void);

/* —— ai drawer —— */
void nl_ov_create_ai_drawer(lv_obj_t *parent);
void nl_ov_hide_ai_drawer(void);
void nl_ov_restore_ai_drawer_if_session(void);
bool nl_ov_ai_session_active(void);
/** 告别后抑制再弹（… / listening / restore），直到用户再说话或重新建链 */
bool nl_ov_ai_farewell_closing(void);
void nl_ov_show_ai_drawer_locked(chat_phase_t phase, const char *status_text, const char *msg_text);
/** 弹出/切相位：有记录则保留最近几句，无记录才用唤醒提示（禁止每轮清空） */
void nl_ov_show_ai_drawer_session(chat_phase_t phase);

/* —— misc —— */
const lv_font_t *nl_ov_ui_font(void);
void nl_ov_do_factory_reset(void);
