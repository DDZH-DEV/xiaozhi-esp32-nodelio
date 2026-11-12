#include "overlay_priv.h"
#include "skill_result_ui.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "ota.h"
#include "sdkconfig.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
#include "media_player.h"
#include "nvs_config.h"
#include "offline_cmd.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include "xz_activation.h"
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

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
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

#define TAG "nl_set"

static lv_timer_t *s_sys_timer;
static lv_obj_t *s_sys_up_lbl;
static lv_obj_t *s_sys_dram_lbl;
static lv_obj_t *s_sys_psram_lbl;
static lv_obj_t *s_sys_tasks_lbl;
static char s_sys_last_up[40];
static char s_sys_last_dram[24];
static char s_sys_last_psram[24];
static char s_sys_last_tasks[16];

static lv_obj_t *s_idle_slp_lbl;
static lv_obj_t *s_wifi_slp_lbl;
static lv_obj_t *s_bye_slp_lbl;
static lv_obj_t *s_listen_idle_lbl;
static lv_obj_t *s_media_pause_lbl;
static lv_obj_t *s_media_duck_lbl;
static lv_obj_t *s_ai_w_lbl;
static lv_obj_t *s_ai_bd_lbl;
static lv_obj_t *s_ai_sh_lbl;
static lv_obj_t *s_voice_mode_row;
static lv_obj_t *s_voice_custom_wrap;
static lv_obj_t *s_voice_url_ta;

static void on_factory_confirm(lv_event_t *e)
{
    (void)e;
    nl_ov_do_factory_reset();
}

static void on_ui_mode_chip(lv_event_t *e)
{
    intptr_t mode = (intptr_t)lv_event_get_user_data(e);
    nl_ov_commit_ui_mode((uint8_t)mode);
}

static void on_text_scale_chip(lv_event_t *e)
{
    intptr_t scale = (intptr_t)lv_event_get_user_data(e);
    nl_ov_commit_text_scale((uint8_t)scale);
}

static void on_status_bar_mode_chip(lv_event_t *e)
{
    intptr_t mode = (intptr_t)lv_event_get_user_data(e);
    nl_ov_commit_status_bar_mode((uint8_t)mode);
}

static void on_screenshot_fab_chip(lv_event_t *e)
{
    intptr_t en = (intptr_t)lv_event_get_user_data(e);
    nl_ov_commit_screenshot_fab((uint8_t)en);
}

static lv_obj_t *s_card_rad_lbl = nullptr;

static void on_card_radius_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (s_card_rad_lbl) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%dpx", v);
        lv_label_set_text(s_card_rad_lbl, buf);
    }
}

static void on_card_radius_release(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    nl_ov_commit_card_radius((uint8_t)v);
}

static void on_idle_sleep_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (s_idle_slp_lbl) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ds", v);
        lv_label_set_text(s_idle_slp_lbl, buf);
    }
}

static void on_wifi_sleep_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (s_wifi_slp_lbl) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ds", v);
        lv_label_set_text(s_wifi_slp_lbl, buf);
    }
}

static void on_bye_sleep_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (s_bye_slp_lbl) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ds", v);
        lv_label_set_text(s_bye_slp_lbl, buf);
    }
}

static void on_listen_idle_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int v = (int)lv_slider_get_value(sl);
    if (s_listen_idle_lbl) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ds", v);
        lv_label_set_text(s_listen_idle_lbl, buf);
    }
}

static void on_listen_idle_release(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    char num[12];
    std::snprintf(num, sizeof(num), "%d", (int)lv_slider_get_value(sl));
    (void)nodelio_panel_ui_set_pref("listen_idle_s", num, nullptr, 0);
}

static void on_sleep_release(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    /* user_data：1=idle 屏 2=bye 3=idle 断网 */
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(sl);
    char num[12];
    std::snprintf(num, sizeof(num), "%d", v);
    const char *key = (which == 3) ? "idle_wifi_s" : (which == 2) ? "bye_sleep_s" : "idle_sleep_s";
    (void)nodelio_panel_ui_set_pref(key, num, nullptr, 0);
}

static lv_obj_t *audio_ctrl_band(lv_obj_t *parent, const char *title, const char *val, lv_obj_t **out_val)
{
    lv_obj_t *band = lv_obj_create(parent);
    lv_obj_remove_style_all(band);
    lv_obj_set_width(band, lv_pct(100));
    lv_obj_set_height(band, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(band, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(band, UI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(band, 1, 0);
    lv_obj_set_style_border_color(band, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(band, LV_OPA_80, 0);
    lv_obj_set_style_pad_left(band, 18, 0);
    lv_obj_set_style_pad_right(band, 18, 0);
    lv_obj_set_style_pad_top(band, 12, 0);
    lv_obj_set_style_pad_bottom(band, 12, 0);
    lv_obj_set_style_pad_row(band, 8, 0);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(band);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, title);
    nl_ov_set_font(t);
    lv_obj_set_style_text_color(t, UI_TEXT_DIM, 0);

    lv_obj_t *v = lv_label_create(head);
    lv_label_set_text(v, val);
    nl_ov_set_font(v);
    lv_obj_set_style_text_color(v, UI_ACCENT, 0);
    if (out_val) {
        *out_val = v;
    }
    return band;
}

static void on_bri_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    nl_ov_apply_brightness((uint8_t)lv_slider_get_value(sl));
    if (s_bri_val_lbl) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(s_bri_val_lbl, buf);
    }
}

static void on_bri_release(lv_event_t *e)
{
    (void)e;
    nl_ov_persist_ui_prefs(false);
}

static void on_vol_changed(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    nl_ov_apply_volume((uint8_t)lv_slider_get_value(sl));
    if (s_vol_val_lbl) {
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(s_vol_val_lbl, buf);
    }
}

static void on_vol_release(lv_event_t *e)
{
    (void)e;
    nl_ov_persist_ui_prefs(true);
}

static void sys_label_set_if_changed(lv_obj_t *lbl, char *last, size_t last_sz, const char *neu)
{
    if (!lbl || !neu || !last || last_sz == 0) {
        return;
    }
    if (std::strcmp(last, neu) == 0) {
        return;
    }
    /* memcpy+显式 NUL，避免 strncpy/snprintf 对共享大 buf 内联出 truncation -Werror */
    size_t n = std::strlen(neu);
    if (n >= last_sz) {
        n = last_sz - 1;
    }
    std::memcpy(last, neu, n);
    last[n] = '\0';
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
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *k = lv_label_create(row);
    lv_label_set_text(k, key);
    nl_ov_set_font(k);
    lv_obj_set_style_text_color(k, UI_TEXT_MUTED, 0);
    lv_obj_set_style_min_width(k, LV_SIZE_CONTENT, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val_init ? val_init : "-");
    /* 占满标签右侧剩余宽度，禁止写死 200 导致宽屏仍裁 URL */
    lv_obj_set_width(v, 10);
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_style_min_width(v, 0, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    nl_ov_set_font(v);
    lv_obj_set_style_text_color(v, UI_TEXT, 0);
    if (out_val) {
        *out_val = v;
    }
    return row;
}

static void sys_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_screen_on || !s_sys_up_lbl) {
        return;
    }
    refresh_sys_live_fields();
}

void nl_ov_stop_sys_monitor(void)
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

void nl_ov_close_settings_detail(void)
{
    nl_ov_stop_sys_monitor();
    nl_ov_kb_set_visible(false);
    if (!s_settings_detail) {
        return;
    }
    lv_obj_delete(s_settings_detail);
    s_settings_detail = nullptr;
    s_settings_detail_body = nullptr;
    s_bri_val_lbl = nullptr;
    s_vol_val_lbl = nullptr;
    s_idle_slp_lbl = nullptr;
    s_wifi_slp_lbl = nullptr;
    s_bye_slp_lbl = nullptr;
    s_listen_idle_lbl = nullptr;
    s_media_pause_lbl = nullptr;
    s_media_duck_lbl = nullptr;
    s_ai_w_lbl = nullptr;
    s_ai_bd_lbl = nullptr;
    s_ai_sh_lbl = nullptr;
    s_voice_mode_row = nullptr;
    s_voice_custom_wrap = nullptr;
    s_voice_url_ta = nullptr;
    nl_ov_home_flush_pending_state();
    if (s_tabbar && s_visible) {
        nl_ov_nav_chrome_sync();
    }
    nl_ov_restore_ai_drawer_if_session();
    /* 详情关掉后技能卡从对半宽收回默认宽。 */
    nodelio_skill_result_layout();
}

static lv_obj_t *settings_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    /* 高度跟内容走；否则默认矮框会裁掉按钮/作者等行 */
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
    ui_style_card(card);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    if (title && title[0]) {
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, title);
        nl_ov_set_font(t);
        lv_obj_set_style_text_color(t, UI_TEXT_SUB, 0);
    }
    return card;
}

static set_page_id_t s_open_set_page = SET_PAGE_DISP;

void nl_ov_open_settings_detail(set_page_id_t page)
{
    /* 连接页盖住设置入口。未盖住时点穿会再叠一层子页。 */
    if (s_connect && !lv_obj_has_flag(s_connect, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    /* 同页重建（改 chip/开关后刷新选中态）保留滚动，避免 P4 长列表一改就回顶。 */
    lv_coord_t keep_y = 0;
    if (s_settings_detail && s_settings_detail_body && s_open_set_page == page) {
        keep_y = lv_obj_get_scroll_y(s_settings_detail_body);
    }
    s_open_set_page = page;
    nl_ov_close_settings_detail();
    if (!s_root) {
        return;
    }
    nl_ov_load_audio_prefs();

    /* 挂在 lv_layer_top() 防止状态栏被 move_foreground 推到底部 */
    s_settings_detail = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_settings_detail, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_settings_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    nodelio_skill_result_layout();
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
    } else if (page == SET_PAGE_VOICE) {
        title = "语音助手";
    } else if (page == SET_PAGE_POWER) {
        title = "省电";
    } else if (page == SET_PAGE_MEDIA) {
        title = "播放器";
    } else if (page == SET_PAGE_CTRL) {
        title = "控家";
    } else if (page == SET_PAGE_DIAGNOSTICS) {
        title = "硬件自检";
    }
    (void)ui_make_nav_bar(s_settings_detail, UI_NAV_H, title,
                          [](lv_event_t *) { nl_ov_close_settings_detail(); });

    lv_obj_t *body = lv_obj_create(s_settings_detail);
    s_settings_detail_body = body;
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
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);
#if CONFIG_BOARD_TYPE_SILA_P4C5_62
    /* 逻辑宽约 1200、再软件/PPA 转 90°。惯性会在松手后连续整页重绘，手感就是一串卡帧。 */
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
#endif

    if (page == SET_PAGE_DIAGNOSTICS) {
        nl_ov_build_diagnostics(body);
    } else if (page == SET_PAGE_DISP) {
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
        lv_obj_t *bri_hint = lv_label_create(bri_band);
        lv_label_set_text(bri_hint, "滑动调节屏幕背光亮度");
        nl_ov_set_font(bri_hint);
        lv_obj_set_style_text_color(bri_hint, UI_TEXT_TIP, 0);

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
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);

        static const char *k_notice_snd[] = {"关闭", "轻响", "成功", "警示"};
        static const char *k_notice_val[] = {"0", "1", "2", "3"};
        const int snd = s_cfg.notice_sound > 3 ? 1 : s_cfg.notice_sound;
        lv_obj_t *snd_band = audio_ctrl_band(body, "提醒声音", k_notice_snd[snd], nullptr);
        lv_obj_t *snd_row = lv_obj_create(snd_band);
        lv_obj_remove_style_all(snd_row);
        lv_obj_set_width(snd_row, lv_pct(100));
        lv_obj_set_height(snd_row, 44);
        lv_obj_set_flex_flow(snd_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(snd_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(snd_row, 8, 0);
        lv_obj_clear_flag(snd_row, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 4; i++) {
            const bool on = snd == i;
            lv_obj_t *chip = lv_obj_create(snd_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t *e) {
                    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                        return;
                    }
                    const int want = (int)(intptr_t)lv_event_get_user_data(e);
                    (void)nodelio_panel_ui_set_pref("notice_sound", k_notice_val[want], nullptr, 0);
                    nl_ov_open_settings_detail(SET_PAGE_DISP);
                },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lbl = lv_label_create(chip);
            nl_ov_set_font(lbl);
            lv_label_set_text(lbl, k_notice_snd[i]);
            lv_obj_set_style_text_color(lbl, on ? UI_ON_TILE : UI_TEXT_SUB, 0);
            lv_obj_center(lbl);
        }
        lv_obj_t *snd_hint = lv_label_create(snd_band);
        lv_label_set_text(snd_hint, "进线、待确认和告警亮屏时播放；点一下可试听");
        nl_ov_set_font(snd_hint);
        lv_obj_set_style_text_color(snd_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(snd_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(snd_hint, lv_pct(100));

        lv_obj_t *rep_row = lv_obj_create(snd_band);
        lv_obj_remove_style_all(rep_row);
        lv_obj_set_width(rep_row, lv_pct(100));
        lv_obj_set_height(rep_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rep_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(rep_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(rep_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *rep_lab = lv_label_create(rep_row);
        lv_label_set_text(rep_lab, "持续提醒");
        nl_ov_set_font(rep_lab);
        lv_obj_set_style_text_color(rep_lab, UI_TEXT, 0);
        lv_obj_t *rep_sw = lv_switch_create(rep_row);
        ui_style_switch(rep_sw);
        if (s_cfg.notice_repeat) {
            lv_obj_add_state(rep_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            rep_sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("notice_repeat", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t *rep_hint = lv_label_create(snd_band);
        lv_label_set_text(rep_hint, "打开后，待处理的进线或告警大约每 12 秒再响一次，直到被处理");
        nl_ov_set_font(rep_hint);
        lv_obj_set_style_text_color(rep_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(rep_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(rep_hint, lv_pct(100));

        auto *codec = Board::GetInstance().GetAudioCodec();
        if (codec && codec->output_channels() > 1) {
            static const char *k_route[] = {"左右一起", "仅左", "仅右"};
            const int route = codec->output_route();
            const char *route_name =
                route >= 0 && route < 3 ? k_route[route] : k_route[0];
            lv_obj_t *route_band = audio_ctrl_band(body, "扬声器输出", route_name, nullptr);
            lv_obj_t *route_row = lv_obj_create(route_band);
            lv_obj_remove_style_all(route_row);
            lv_obj_set_width(route_row, lv_pct(100));
            lv_obj_set_height(route_row, 44);
            lv_obj_set_flex_flow(route_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(route_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(route_row, 8, 0);
            lv_obj_clear_flag(route_row, LV_OBJ_FLAG_SCROLLABLE);
            for (int i = 0; i < 3; i++) {
                const bool on = route == i;
                lv_obj_t *chip = lv_obj_create(route_row);
                lv_obj_remove_style_all(chip);
                lv_obj_set_flex_grow(chip, 1);
                lv_obj_set_height(chip, 44);
                lv_obj_set_style_radius(chip, 12, 0);
                lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
                lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
                lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(
                    chip,
                    [](lv_event_t *e) {
                        if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                            return;
                        }
                        const int want = (int)(intptr_t)lv_event_get_user_data(e);
                        auto *c = Board::GetInstance().GetAudioCodec();
                        if (c) {
                            c->SetOutputRoute(want);
                        }
                        Board::GetInstance().NoteUserActivity();
                        nl_ov_toast(want == 1 ? "仅左扬声器" : (want == 2 ? "仅右扬声器" : "左右一起"));
                        nl_ov_open_settings_detail(SET_PAGE_DISP);
                    },
                    LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_t *lbl = lv_label_create(chip);
                nl_ov_set_font(lbl);
                lv_label_set_text(lbl, k_route[i]);
                lv_obj_set_style_text_color(lbl, on ? UI_ON_TILE : UI_TEXT_SUB, 0);
                lv_obj_center(lbl);
            }
            lv_obj_t *route_hint = lv_label_create(route_band);
            lv_label_set_text(route_hint, "先在硬件自检分别试听左右，确认两只喇叭都有声后再用");
            nl_ov_set_font(route_hint);
            lv_obj_set_style_text_color(route_hint, UI_TEXT_TIP, 0);
        }

        const uint8_t mode_cur = s_cfg.ui_mode == 1 ? 1 : 0;
        lv_obj_t *mode_band = audio_ctrl_band(body, "外观", nl_ov_ui_mode_name(mode_cur), nullptr);
        lv_obj_t *mode_row = lv_obj_create(mode_band);
        lv_obj_remove_style_all(mode_row);
        lv_obj_set_width(mode_row, lv_pct(100));
        lv_obj_set_height(mode_row, 44);
        lv_obj_set_flex_flow(mode_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(mode_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(mode_row, 8, 0);
        lv_obj_clear_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
        static const char *k_mode[] = {"浅色", "深色"};
        for (int i = 0; i < 2; i++) {
            const bool on = mode_cur == (uint8_t)i;
            lv_obj_t *chip = lv_obj_create(mode_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(chip, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(chip, 1, LV_STATE_PRESSED);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(chip, on_ui_mode_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, k_mode[i]);
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }

        const uint8_t cur = nodelio_text_scale();
        lv_obj_t *txt_band = audio_ctrl_band(body, "文字大小", nl_ov_text_scale_name(cur), nullptr);
        lv_obj_t *row = lv_obj_create(txt_band);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 44);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        static const char *k_scale[] = {"标准", "较大"};
        for (int i = 0; i < 2; i++) {
            const bool on = cur == (uint8_t)i;
            lv_obj_t *chip = lv_obj_create(row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(chip, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(chip, 1, LV_STATE_PRESSED);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(chip, on_text_scale_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, k_scale[i]);
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }
        lv_obj_t *txt_hint = lv_label_create(txt_band);
        lv_label_set_text(txt_hint, "远距离阅读可调大");
        nl_ov_set_font(txt_hint);
        lv_obj_set_style_text_color(txt_hint, UI_TEXT_TIP, 0);

        const uint8_t sb_cur = s_cfg.status_bar_mode > 2 ? 0 : s_cfg.status_bar_mode;
        static const char *k_sb_names[] = {"自动隐藏", "常显", "常隐"};
        lv_obj_t *sb_band = audio_ctrl_band(body, "顶部状态栏", k_sb_names[sb_cur], nullptr);
        lv_obj_t *sb_row = lv_obj_create(sb_band);
        lv_obj_remove_style_all(sb_row);
        lv_obj_set_width(sb_row, lv_pct(100));
        lv_obj_set_height(sb_row, 44);
        lv_obj_set_flex_flow(sb_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(sb_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(sb_row, 8, 0);
        lv_obj_clear_flag(sb_row, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < 3; i++) {
            const bool on = sb_cur == (uint8_t)i;
            lv_obj_t *chip = lv_obj_create(sb_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(chip, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(chip, 1, LV_STATE_PRESSED);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(chip, on_status_bar_mode_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, k_sb_names[i]);
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }
        lv_obj_t *sb_hint = lv_label_create(sb_band);
        lv_label_set_text(sb_hint, "截图不想带顶栏时可设为常隐");
        nl_ov_set_font(sb_hint);
        lv_obj_set_style_text_color(sb_hint, UI_TEXT_TIP, 0);

        const bool fab_cur = s_cfg.screenshot_fab_en != 0;
        lv_obj_t *fab_band = audio_ctrl_band(body, "截图悬浮图标", fab_cur ? "显示" : "隐藏", nullptr);
        lv_obj_t *fab_row = lv_obj_create(fab_band);
        lv_obj_remove_style_all(fab_row);
        lv_obj_set_width(fab_row, lv_pct(100));
        lv_obj_set_height(fab_row, 44);
        lv_obj_set_flex_flow(fab_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(fab_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(fab_row, 8, 0);
        lv_obj_clear_flag(fab_row, LV_OBJ_FLAG_SCROLLABLE);
        static const char *k_fab_opt[] = {"显示", "隐藏"};
        for (int i = 0; i < 2; i++) {
            const bool on = (i == 0 && fab_cur) || (i == 1 && !fab_cur);
            lv_obj_t *chip = lv_obj_create(fab_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(chip, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(chip, 1, LV_STATE_PRESSED);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(chip, on_screenshot_fab_chip, LV_EVENT_CLICKED,
                                (void *)(intptr_t)(i == 0 ? 1 : 0));
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, k_fab_opt[i]);
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }
        lv_obj_t *fab_hint = lv_label_create(fab_band);
        lv_label_set_text(fab_hint, "屏幕右侧浮动相机图标，可拖动位置，点按即截屏");
        nl_ov_set_font(fab_hint);
        lv_obj_set_style_text_color(fab_hint, UI_TEXT_TIP, 0);

        char rad_str[16];
        std::snprintf(rad_str, sizeof(rad_str), "%dpx", nodelio_card_radius_px());
        lv_obj_t *rad_band = audio_ctrl_band(body, "卡片圆角", rad_str, &s_card_rad_lbl);
        lv_obj_t *rad_sl = lv_slider_create(rad_band);
        ui_style_slider(rad_sl);
        lv_slider_set_range(rad_sl, 0, 24);
        lv_slider_set_value(rad_sl, nodelio_card_radius_px(), LV_ANIM_OFF);
        lv_obj_add_event_cb(rad_sl, on_card_radius_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(rad_sl, on_card_radius_release, LV_EVENT_RELEASED, nullptr);
        lv_obj_t *rad_hint = lv_label_create(rad_band);
        lv_label_set_text(rad_hint,
                          nodelio_display_profile()->nav_width
                              ? "0=应用卡直角贴边无缝；>0 为圆角与边距"
                              : "拖动调节全屏卡片与磁贴圆角弧度 (0-24px)");
        nl_ov_set_font(rad_hint);
        lv_obj_set_style_text_color(rad_hint, UI_TEXT_TIP, 0);
    } else if (page == SET_PAGE_SYS) {
        nl_ov_load_audio_prefs();
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
        /* 构建号：与顶栏、OTA 同一套；改 nodelio/sila 源码才涨 */
        {
            std::snprintf(line, sizeof(line), "V%s", NODELIO_BUILD_STAMP);
            sys_kv_row(dev, "构建号", line, nullptr);
        }
        {
            const esp_app_desc_t *app = esp_app_get_description();
            if (app) {
                /* IDF __DATE__ 是 "Oct 20 2026"，转成数字日期避免中英文混排 */
                static const char *k_mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                char mmm[4] = {};
                int day = 0, year = 0, mon = 0, hh = 0, mm = 0, ss = 0;
                if (std::sscanf(app->date, "%3s %d %d", mmm, &day, &year) == 3) {
                    for (int i = 0; i < 12; ++i) {
                        if (std::strcmp(mmm, k_mon[i]) == 0) {
                            mon = i + 1;
                            break;
                        }
                    }
                }
                std::sscanf(app->time, "%d:%d:%d", &hh, &mm, &ss);
                if (mon > 0 && year > 0) {
                    std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d", year, mon,
                                  day, hh, mm, ss);
                } else {
                    std::snprintf(line, sizeof(line), "%s %s", app->date, app->time);
                }
            } else {
                std::snprintf(line, sizeof(line), "-");
            }
            sys_kv_row(dev, "编译时间", line, nullptr);
        }
        sys_kv_row(dev, "作者", "Dev-SL", nullptr);
        sys_kv_row(dev, "唤醒词", "你好小智", nullptr);
        sys_kv_row(dev, "小智云", s_cfg.xiaozhi_en ? "已启用" : "已关闭", nullptr);
        if (nodelio_power_save_enabled()) {
            std::snprintf(line, sizeof(line), "屏%ds · 网%ds · 退%d", (int)s_cfg.idle_sleep_s,
                          (int)s_cfg.idle_wifi_s, (int)s_cfg.goodbye_sleep_s);
        } else {
            std::snprintf(line, sizeof(line), "屏%ds · 不断网 · 退%d", (int)s_cfg.idle_sleep_s,
                          (int)s_cfg.goodbye_sleep_s);
        }
        sys_kv_row(dev, "省电", line, nullptr);
        {
            std::string mac = SystemInfo::GetMacAddress();
            sys_kv_row(dev, "MAC", mac.c_str(), nullptr);
        }
        sys_kv_row(dev, "设备 ID", s_cfg.device_id[0] ? s_cfg.device_id : "未注册", nullptr);
        sys_kv_row(dev, "账号", s_cfg.owner[0] ? s_cfg.owner : "-", nullptr);
        sys_kv_row(dev, "服务器", s_cfg.server_url[0] ? s_cfg.server_url : "未配置", nullptr);
        sys_kv_row(dev, "WiFi", s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "未配置", nullptr);
        {
            sys_kv_row(dev, "链路", nodelio_wifi_has_ip() ? "已连接" : "未连接", nullptr);
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
    } else if (page == SET_PAGE_POWER) {
        lv_obj_set_style_pad_row(body, 12, 0);

        lv_obj_t *ps_card = settings_card(body, "空闲断网");
        lv_obj_set_style_pad_all(ps_card, 16, 0);
        lv_obj_set_style_pad_row(ps_card, 12, 0);

        lv_obj_t *ps_row = lv_obj_create(ps_card);
        lv_obj_remove_style_all(ps_row);
        lv_obj_set_width(ps_row, lv_pct(100));
        lv_obj_set_height(ps_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ps_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ps_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(ps_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ps_lab = lv_label_create(ps_row);
        lv_label_set_text(ps_lab, "允许省电");
        nl_ov_set_font(ps_lab);
        lv_obj_set_style_text_color(ps_lab, UI_TEXT, 0);

        lv_obj_t *ps_sw = lv_switch_create(ps_row);
        ui_style_switch(ps_sw);
        if (s_cfg.power_save_en) {
            lv_obj_add_state(ps_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ps_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            ps_sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("power_save", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t *ps_hint = lv_label_create(ps_card);
        lv_label_set_text(ps_hint, "只控制空闲后是否断 WiFi/MQTT；息屏仍按下方时间");
        nl_ov_set_font(ps_hint);
        lv_obj_set_style_text_color(ps_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(ps_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ps_hint, lv_pct(100));

        if (!nodelio_display_profile()->network_sleep) {
            ui_set_enabled(ps_sw, false);
            lv_obj_remove_state(ps_sw, LV_STATE_CHECKED);
            lv_label_set_text(ps_hint, "此板当前只关闭背光并降低刷新，保持 Hosted 网络连接");
        }

        lv_obj_t *w_row = lv_obj_create(ps_card);
        lv_obj_remove_style_all(w_row);
        lv_obj_set_width(w_row, lv_pct(100));
        lv_obj_set_height(w_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(w_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(w_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(w_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *w_lab = lv_label_create(w_row);
        lv_label_set_text(w_lab, "息屏语音唤醒");
        nl_ov_set_font(w_lab);
        lv_obj_set_style_text_color(w_lab, UI_TEXT, 0);

        lv_obj_t *w_sw = lv_switch_create(w_row);
        ui_style_switch(w_sw);
        if (s_cfg.wake_on_sleep) {
            lv_obj_add_state(w_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(w_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            w_sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("wake_on_sleep", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *w_hint = lv_label_create(ps_card);
        lv_label_set_text(w_hint, "开：息屏可随时语音唤醒(较耗电)；关：息屏停麦克风，触屏/按键唤醒(最省电)");
        nl_ov_set_font(w_hint);
        lv_obj_set_style_text_color(w_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(w_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(w_hint, lv_pct(100));

        char sec[16];
        std::snprintf(sec, sizeof(sec), "%ds", (int)s_cfg.idle_sleep_s);
        lv_obj_t *idle_band = audio_ctrl_band(body, "空闲息屏", sec, &s_idle_slp_lbl);
        lv_obj_t *idle_sl = lv_slider_create(idle_band);
        ui_style_slider(idle_sl);
        lv_slider_set_range(idle_sl, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX);
        lv_slider_set_value(idle_sl, s_cfg.idle_sleep_s, LV_ANIM_OFF);
        lv_obj_add_event_cb(idle_sl, on_idle_sleep_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(idle_sl, on_sleep_release, LV_EVENT_RELEASED, (void *)(intptr_t)1);
        lv_obj_t *idle_hint = lv_label_create(idle_band);
        lv_label_set_text(idle_hint, "无操作多久后关背光（与断网时刻独立）");
        nl_ov_set_font(idle_hint);
        lv_obj_set_style_text_color(idle_hint, UI_TEXT_TIP, 0);

        std::snprintf(sec, sizeof(sec), "%ds", (int)s_cfg.idle_wifi_s);
        lv_obj_t *wifi_band = audio_ctrl_band(body, "空闲断网", sec, &s_wifi_slp_lbl);
        lv_obj_t *wifi_sl = lv_slider_create(wifi_band);
        ui_style_slider(wifi_sl);
        lv_slider_set_range(wifi_sl, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX);
        lv_slider_set_value(wifi_sl, s_cfg.idle_wifi_s, LV_ANIM_OFF);
        lv_obj_add_event_cb(wifi_sl, on_wifi_sleep_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(wifi_sl, on_sleep_release, LV_EVENT_RELEASED, (void *)(intptr_t)3);
        lv_obj_t *wifi_hint = lv_label_create(wifi_band);
        lv_label_set_text(wifi_hint, "无操作多久后停 WiFi/MQTT（可晚于息屏，触摸更快唤醒）");
        nl_ov_set_font(wifi_hint);
        lv_obj_set_style_text_color(wifi_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(wifi_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(wifi_hint, lv_pct(100));

        if (!nodelio_display_profile()->network_sleep) {
            lv_obj_add_flag(wifi_band, LV_OBJ_FLAG_HIDDEN);
        }
        std::snprintf(sec, sizeof(sec), "%ds", (int)s_cfg.goodbye_sleep_s);
        lv_obj_t *bye_band = audio_ctrl_band(body, "退下后延迟息屏", sec, &s_bye_slp_lbl);
        lv_obj_t *bye_sl = lv_slider_create(bye_band);
        ui_style_slider(bye_sl);
        lv_slider_set_range(bye_sl, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX);
        lv_slider_set_value(bye_sl, s_cfg.goodbye_sleep_s, LV_ANIM_OFF);
        lv_obj_add_event_cb(bye_sl, on_bye_sleep_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(bye_sl, on_sleep_release, LV_EVENT_RELEASED, (void *)(intptr_t)2);
        lv_obj_t *bye_hint = lv_label_create(bye_band);
        lv_label_set_text(bye_hint, "小智说完再见后，多久关屏（触摸会再延一段）");
        nl_ov_set_font(bye_hint);
        lv_obj_set_style_text_color(bye_hint, UI_TEXT_TIP, 0);
    } else if (page == SET_PAGE_CTRL) {
        lv_obj_set_style_pad_row(body, 12, 0);
        lv_obj_t *card = settings_card(body, "控家模块");
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 12, 0);

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lab = lv_label_create(row);
        lv_label_set_text(lab, "允许控家模块");
        nl_ov_set_font(lab);
        lv_obj_set_style_text_color(lab, UI_TEXT, 0);

        lv_obj_t *sw = lv_switch_create(row);
        ui_style_switch(sw);
        if (nodelio_controls_enabled()) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("controls", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint, "关闭后首页不显示灯与继电器，开灯口令会提示未启用");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, lv_pct(100));
    } else if (page == SET_PAGE_MEDIA) {
        lv_obj_set_style_pad_row(body, 12, 0);

        lv_obj_t *ms_card = settings_card(body, "播放中息屏");
        lv_obj_set_style_pad_all(ms_card, 16, 0);
        lv_obj_set_style_pad_row(ms_card, 12, 0);

        lv_obj_t *ms_row = lv_obj_create(ms_card);
        lv_obj_remove_style_all(ms_row);
        lv_obj_set_width(ms_row, lv_pct(100));
        lv_obj_set_height(ms_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ms_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(ms_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(ms_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ms_lab = lv_label_create(ms_row);
        lv_label_set_text(ms_lab, "允许息屏");
        nl_ov_set_font(ms_lab);
        lv_obj_set_style_text_color(ms_lab, UI_TEXT, 0);

        lv_obj_t *ms_sw = lv_switch_create(ms_row);
        ui_style_switch(ms_sw);
        if (s_cfg.media_sleep_en) {
            lv_obj_add_state(ms_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ms_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            ms_sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("media_sleep", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t *ms_hint = lv_label_create(ms_card);
        lv_label_set_text(ms_hint, "关背光不停播；播放中不会断网，避免音乐中断");
        nl_ov_set_font(ms_hint);
        lv_obj_set_style_text_color(ms_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(ms_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ms_hint, lv_pct(100));

        char sec[16];
        if (!s_cfg.media_pause_s) {
            s_cfg.media_pause_s = NODELIO_MEDIA_PAUSE_DEFAULT;
        }
        std::snprintf(sec, sizeof(sec), "%ds", (int)s_cfg.media_pause_s);
        lv_obj_t *pause_band = audio_ctrl_band(body, "暂停后关闭播放器", sec, &s_media_pause_lbl);
        lv_obj_t *pause_sl = lv_slider_create(pause_band);
        ui_style_slider(pause_sl);
        lv_slider_set_range(pause_sl, NODELIO_MEDIA_PAUSE_MIN, NODELIO_MEDIA_PAUSE_MAX);
        lv_slider_set_value(pause_sl, s_cfg.media_pause_s, LV_ANIM_OFF);
        lv_obj_add_event_cb(
            pause_sl,
            [](lv_event_t *e) {
                lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
                int v = (int)lv_slider_get_value(sl);
                if (s_media_pause_lbl) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%ds", v);
                    lv_label_set_text(s_media_pause_lbl, buf);
                }
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(
            pause_sl,
            [](lv_event_t *e) {
                char num[12];
                std::snprintf(num, sizeof(num), "%d",
                              (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
                (void)nodelio_panel_ui_set_pref("media_pause_s", num, nullptr, 0);
            },
            LV_EVENT_RELEASED, nullptr);
        lv_obj_t *pause_hint = lv_label_create(pause_band);
        lv_label_set_text(pause_hint, "暂停后多久自动关掉播放条并释放会话");
        nl_ov_set_font(pause_hint);
        lv_obj_set_style_text_color(pause_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(pause_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(pause_hint, lv_pct(100));

        if (!s_cfg.media_wake_duck_pct) {
            s_cfg.media_wake_duck_pct = NODELIO_MEDIA_WAKE_DUCK_DEFAULT;
        }
        char duck[16];
        std::snprintf(duck, sizeof(duck), "%d%%", (int)s_cfg.media_wake_duck_pct);
        lv_obj_t *duck_band = audio_ctrl_band(body, "唤醒时音量", duck, &s_media_duck_lbl);
        lv_obj_t *duck_sl = lv_slider_create(duck_band);
        ui_style_slider(duck_sl);
        lv_slider_set_range(duck_sl, NODELIO_MEDIA_WAKE_DUCK_MIN, NODELIO_MEDIA_WAKE_DUCK_MAX);
        lv_slider_set_value(duck_sl, s_cfg.media_wake_duck_pct, LV_ANIM_OFF);
        lv_obj_add_event_cb(
            duck_sl,
            [](lv_event_t *e) {
                lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
                int v = (int)lv_slider_get_value(sl);
                if (s_media_duck_lbl) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d%%", v);
                    lv_label_set_text(s_media_duck_lbl, buf);
                }
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(
            duck_sl,
            [](lv_event_t *e) {
                char num[12];
                std::snprintf(num, sizeof(num), "%d",
                              (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
                (void)nodelio_panel_ui_set_pref("media_wake_duck_pct", num, nullptr, 0);
            },
            LV_EVENT_RELEASED, nullptr);
        lv_obj_t *duck_hint = lv_label_create(duck_band);
        lv_label_set_text(duck_hint, "电台/播客开着听唤醒词时保留的音量；100% 不压低，唤醒可能变差");
        nl_ov_set_font(duck_hint);
        lv_obj_set_style_text_color(duck_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(duck_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(duck_hint, lv_pct(100));
    } else if (page == SET_PAGE_VOICE) {
        lv_obj_set_style_pad_row(body, 12, 0);

        lv_obj_t *card = settings_card(body, "小智云对话");
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 12, 0);

        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lab = lv_label_create(row);
        lv_label_set_text(lab, "启用小智");
        nl_ov_set_font(lab);
        lv_obj_set_style_text_color(lab, UI_TEXT, 0);

        lv_obj_t *sw = lv_switch_create(row);
        ui_style_switch(sw);
        if (s_cfg.xiaozhi_en) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("xiaozhi", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint,
                          "关闭后不再连接语音云，唤醒不等公网。"
                          "改开关后约 3 秒重启生效。");
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);

        lv_obj_t *barge_row = lv_obj_create(card);
        lv_obj_remove_style_all(barge_row);
        lv_obj_set_width(barge_row, lv_pct(100));
        lv_obj_set_height(barge_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(barge_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(barge_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(barge_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *barge_lab = lv_label_create(barge_row);
        lv_label_set_text(barge_lab, "语音打断");
        nl_ov_set_font(barge_lab);
        lv_obj_set_style_text_color(barge_lab, UI_TEXT, 0);

        lv_obj_t *barge_sw = lv_switch_create(barge_row);
        ui_style_switch(barge_sw);
        if (s_cfg.voice_barge_in) {
            lv_obj_add_state(barge_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(barge_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            barge_sw,
            [](lv_event_t *e) {
                bool on = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
                (void)nodelio_panel_ui_set_pref("voice_barge_in", on ? "on" : "off", nullptr, 0);
            },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *barge_hint = lv_label_create(card);
        lv_label_set_text(barge_hint,
                          "开启后小智说话时可直接开口打断（免唤醒词）。"
                          "喇叭回采可能误触，不适可关。即时生效。");
        lv_obj_set_width(barge_hint, lv_pct(100));
        lv_label_set_long_mode(barge_hint, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(barge_hint);
        lv_obj_set_style_text_color(barge_hint, UI_TEXT_TIP, 0);

        /* —— 官方 / 自定义小智语音服务器 —— */
        lv_obj_t *srv_card = settings_card(body, "语音服务器");
        lv_obj_set_style_pad_all(srv_card, 16, 0);
        lv_obj_set_style_pad_row(srv_card, 12, 0);

        /* 禁止在 LVGL(PSRAM) 栈上 Settings/nvs_open；只用 RAM 缓存 */
        char stored_ota[160] = {};
        nodelio_xz_voice_get_ota_url(stored_ota, sizeof(stored_ota));
        const bool is_official = stored_ota[0] == '\0' || std::strcmp(stored_ota, CONFIG_OTA_URL) == 0;
        const std::string cur_voice = is_official ? std::string(CONFIG_OTA_URL) : std::string(stored_ota);

        s_voice_mode_row = lv_obj_create(srv_card);
        lv_obj_remove_style_all(s_voice_mode_row);
        lv_obj_set_width(s_voice_mode_row, lv_pct(100));
        lv_obj_set_height(s_voice_mode_row, 44);
        lv_obj_set_flex_flow(s_voice_mode_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_voice_mode_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_voice_mode_row, 8, 0);
        lv_obj_clear_flag(s_voice_mode_row, LV_OBJ_FLAG_SCROLLABLE);

        s_voice_custom_wrap = lv_obj_create(srv_card);
        lv_obj_remove_style_all(s_voice_custom_wrap);
        lv_obj_set_width(s_voice_custom_wrap, lv_pct(100));
        lv_obj_set_height(s_voice_custom_wrap, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_voice_custom_wrap, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(s_voice_custom_wrap, 8, 0);
        lv_obj_clear_flag(s_voice_custom_wrap, LV_OBJ_FLAG_SCROLLABLE);
        if (is_official) {
            lv_obj_add_flag(s_voice_custom_wrap, LV_OBJ_FLAG_HIDDEN);
        }

        for (int i = 0; i < 2; i++) {
            const bool on = (i == 0) ? is_official : !is_official;
            lv_obj_t *chip = lv_obj_create(s_voice_mode_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t *e) {
                    const int v = (int)(intptr_t)lv_event_get_user_data(e);
                    if (!s_voice_mode_row) {
                        return;
                    }
                    const uint32_t n = lv_obj_get_child_count(s_voice_mode_row);
                    for (uint32_t i = 0; i < n; i++) {
                        lv_obj_t *c = lv_obj_get_child(s_voice_mode_row, (int32_t)i);
                        const bool sel = (int)i == v;
                        lv_obj_set_style_bg_color(c, sel ? UI_ACCENT : UI_CAPSULE_OFF, 0);
                        lv_obj_t *lab = lv_obj_get_child(c, 0);
                        if (lab) {
                            lv_obj_set_style_text_color(lab, sel ? UI_ON_TILE : UI_TEXT, 0);
                        }
                    }
                    lv_obj_set_user_data(s_voice_mode_row, (void *)(intptr_t)v);
                    if (s_voice_custom_wrap) {
                        if (v == 0) {
                            lv_obj_add_flag(s_voice_custom_wrap, LV_OBJ_FLAG_HIDDEN);
                            nl_ov_kb_set_visible(false);
                        } else {
                            lv_obj_clear_flag(s_voice_custom_wrap, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, i == 0 ? "官方" : "自定义");
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }
        lv_obj_set_user_data(s_voice_mode_row, (void *)(intptr_t)(is_official ? 0 : 1));

        lv_obj_t *url_lbl = lv_label_create(s_voice_custom_wrap);
        lv_label_set_text(url_lbl, "OTA 地址");
        nl_ov_set_font(url_lbl);
        lv_obj_set_style_text_color(url_lbl, UI_TEXT_DIM, 0);

        s_voice_url_ta = lv_textarea_create(s_voice_custom_wrap);
        lv_textarea_set_one_line(s_voice_url_ta, true);
        lv_textarea_set_max_length(s_voice_url_ta, 160);
        lv_textarea_set_placeholder_text(s_voice_url_ta, "http://ip:8003/xiaozhi/ota/");
        if (!is_official) {
            lv_textarea_set_text(s_voice_url_ta, cur_voice.c_str());
        }
        lv_obj_set_width(s_voice_url_ta, lv_pct(100));
        lv_obj_set_style_pad_ver(s_voice_url_ta, 10, 0);
        lv_obj_set_style_pad_hor(s_voice_url_ta, 12, 0);
        lv_obj_set_style_bg_color(s_voice_url_ta, UI_FIELD, 0);
        lv_obj_set_style_bg_opa(s_voice_url_ta, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_voice_url_ta, 2, 0);
        lv_obj_set_style_border_color(s_voice_url_ta, UI_BORDER, 0);
        lv_obj_set_style_radius(s_voice_url_ta, UI_RADIUS_SM, 0);
        nl_ov_set_font(s_voice_url_ta);
        lv_obj_set_style_text_color(s_voice_url_ta, UI_TEXT, 0);
        lv_obj_set_style_bg_color(s_voice_url_ta, UI_FIELD_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(s_voice_url_ta, UI_ACCENT, LV_STATE_FOCUSED);
        lv_obj_set_scroll_dir(s_voice_url_ta, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(s_voice_url_ta, LV_SCROLLBAR_MODE_OFF);
        nl_ov_kb_wire_textarea(s_voice_url_ta);

        lv_obj_t *srv_hint = lv_label_create(s_voice_custom_wrap);
        lv_label_set_text(srv_hint, "填自建小智的 OTA 地址，不是 NodeLio 控家地址。");
        lv_obj_set_width(srv_hint, lv_pct(100));
        lv_label_set_long_mode(srv_hint, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(srv_hint);
        lv_obj_set_style_text_color(srv_hint, UI_TEXT_TIP, 0);

        /* 未绑定小智：常驻激活码（三块 sila 屏共用本页） */
        {
            const bool has_mqtt = nodelio_xz_voice_has_mqtt();
            char act_code[32] = {};
            char act_msg[96] = {};
            const bool has_code =
                nodelio_xz_activation_get(act_code, sizeof(act_code), act_msg, sizeof(act_msg));
            if (!has_mqtt && has_code) {
                lv_obj_t *act_title = lv_label_create(srv_card);
                lv_label_set_text(act_title, "小智激活码");
                nl_ov_set_font(act_title);
                lv_obj_set_style_text_color(act_title, UI_TEXT_DIM, 0);

                lv_obj_t *act_code_lbl = lv_label_create(srv_card);
                lv_label_set_text(act_code_lbl, act_code);
                nl_ov_set_font_title(act_code_lbl);
                lv_obj_set_style_text_color(act_code_lbl, UI_ACCENT, 0);
                lv_obj_set_style_text_letter_space(act_code_lbl, 4, 0);
                lv_obj_set_width(act_code_lbl, lv_pct(100));

                lv_obj_t *act_hint = lv_label_create(srv_card);
                lv_label_set_text(act_hint, act_msg[0] ? act_msg
                                                       : "请在小智 App / 控制台绑定此设备");
                lv_obj_set_width(act_hint, lv_pct(100));
                lv_label_set_long_mode(act_hint, LV_LABEL_LONG_WRAP);
                nl_ov_set_font(act_hint);
                lv_obj_set_style_text_color(act_hint, UI_TEXT_TIP, 0);
            } else if (!has_mqtt) {
                lv_obj_t *need = lv_label_create(srv_card);
                lv_label_set_text(need, "尚未拿到激活码，点下方刷新");
                lv_obj_set_width(need, lv_pct(100));
                lv_label_set_long_mode(need, LV_LABEL_LONG_WRAP);
                nl_ov_set_font(need);
                lv_obj_set_style_text_color(need, UI_TEXT_TIP, 0);
            }

            if (!has_mqtt) {
                lv_obj_t *refresh_btn =
                    ui_make_text_btn(srv_card, "刷新激活码", 1, [](lv_event_t *) {
                        Application::GetInstance().RequestVoiceActivationRefresh(false);
                    });
                lv_obj_set_style_margin_top(refresh_btn, 4, 0);
            }
        }

        lv_obj_t *save_btn = ui_make_text_btn(srv_card, "保存并重连", 0, [](lv_event_t *) {
            const int mode =
                s_voice_mode_row ? (int)(intptr_t)lv_obj_get_user_data(s_voice_mode_row) : 0;
            nl_ov_kb_set_visible(false);
            if (mode == 0) {
                Application::GetInstance().RequestVoiceServerApply(true);
                return;
            }
            const char *raw = s_voice_url_ta ? lv_textarea_get_text(s_voice_url_ta) : "";
            if (!raw || !raw[0]) {
                nl_ov_toast("请填写 OTA 地址");
                return;
            }
            if (strncmp(raw, "http://", 7) != 0 && strncmp(raw, "https://", 8) != 0) {
                nl_ov_toast("地址须以 http:// 或 https:// 开头");
                return;
            }
            Application::GetInstance().RequestVoiceServerApply(false, raw);
        });
        lv_obj_set_style_margin_top(save_btn, 4, 0);

        lv_obj_t *off_hint = lv_label_create(srv_card);
        lv_label_set_text(off_hint, "官方为 api.tenclass.net。保存后会重启面板。");
        lv_obj_set_width(off_hint, lv_pct(100));
        lv_label_set_long_mode(off_hint, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(off_hint);
        lv_obj_set_style_text_color(off_hint, UI_TEXT_TIP, 0);

        char sec[16];
        uint16_t lis = s_cfg.listen_idle_s ? s_cfg.listen_idle_s : NODELIO_LISTEN_IDLE_DEFAULT;
        std::snprintf(sec, sizeof(sec), "%ds", (int)lis);
        lv_obj_t *lis_band = audio_ctrl_band(body, "无对话自动退下", sec, &s_listen_idle_lbl);
        lv_obj_t *lis_sl = lv_slider_create(lis_band);
        ui_style_slider(lis_sl);
        lv_slider_set_range(lis_sl, NODELIO_LISTEN_IDLE_MIN, NODELIO_LISTEN_IDLE_MAX);
        lv_slider_set_value(lis_sl, lis, LV_ANIM_OFF);
        lv_obj_add_event_cb(lis_sl, on_listen_idle_changed, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(lis_sl, on_listen_idle_release, LV_EVENT_RELEASED, nullptr);
        lv_obj_t *lis_hint = lv_label_create(lis_band);
        lv_label_set_text(lis_hint, "唤醒后一直不说话，多久主动收起（10–30 秒）");
        nl_ov_set_font(lis_hint);
        lv_obj_set_style_text_color(lis_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(lis_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lis_hint, lv_pct(100));

        char ai_txt[16];
        const int ai_w = (int)nodelio_ai_drawer_width();
        std::snprintf(ai_txt, sizeof(ai_txt), "%d", ai_w);
        lv_obj_t *ai_w_band = audio_ctrl_band(body, "对话框宽度", ai_txt, &s_ai_w_lbl);
        lv_obj_t *ai_w_sl = lv_slider_create(ai_w_band);
        ui_style_slider(ai_w_sl);
        lv_slider_set_range(ai_w_sl, NODELIO_AI_W_MIN, nodelio_ai_drawer_width_max());
        lv_slider_set_value(ai_w_sl, ai_w, LV_ANIM_OFF);
        lv_obj_add_event_cb(
            ai_w_sl,
            [](lv_event_t *e) {
                int v = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
                if (s_ai_w_lbl) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", v);
                    lv_label_set_text(s_ai_w_lbl, buf);
                }
                s_cfg.ai_w = (uint16_t)v;
                nodelio_nvs_cache_put_preferences(&s_cfg);
                nl_ov_ai_apply_metrics();
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(
            ai_w_sl,
            [](lv_event_t *e) {
                char num[12];
                std::snprintf(num, sizeof(num), "%d",
                              (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
                (void)nodelio_panel_ui_set_pref("ai_drawer_w", num, nullptr, 0);
            },
            LV_EVENT_RELEASED, nullptr);

        const int ai_bd = (int)nodelio_ai_border_width();
        std::snprintf(ai_txt, sizeof(ai_txt), "%d", ai_bd);
        lv_obj_t *ai_bd_band = audio_ctrl_band(body, "边框粗细", ai_txt, &s_ai_bd_lbl);
        lv_obj_t *ai_bd_sl = lv_slider_create(ai_bd_band);
        ui_style_slider(ai_bd_sl);
        lv_slider_set_range(ai_bd_sl, 0, NODELIO_AI_BD_MAX);
        lv_slider_set_value(ai_bd_sl, ai_bd, LV_ANIM_OFF);
        lv_obj_add_event_cb(
            ai_bd_sl,
            [](lv_event_t *e) {
                int v = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
                if (s_ai_bd_lbl) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", v);
                    lv_label_set_text(s_ai_bd_lbl, buf);
                }
                s_cfg.ai_bd_w = (uint8_t)v;
                nodelio_nvs_cache_put_preferences(&s_cfg);
                nl_ov_ai_apply_metrics();
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(
            ai_bd_sl,
            [](lv_event_t *e) {
                char num[12];
                std::snprintf(num, sizeof(num), "%d",
                              (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
                (void)nodelio_panel_ui_set_pref("ai_border_w", num, nullptr, 0);
            },
            LV_EVENT_RELEASED, nullptr);

        const int ai_sh = (int)nodelio_ai_shadow_width();
        std::snprintf(ai_txt, sizeof(ai_txt), "%d", ai_sh);
        lv_obj_t *ai_sh_band = audio_ctrl_band(body, "阴影", ai_txt, &s_ai_sh_lbl);
        lv_obj_t *ai_sh_sl = lv_slider_create(ai_sh_band);
        ui_style_slider(ai_sh_sl);
        lv_slider_set_range(ai_sh_sl, 0, NODELIO_AI_SH_MAX);
        lv_slider_set_value(ai_sh_sl, ai_sh, LV_ANIM_OFF);
        lv_obj_add_event_cb(
            ai_sh_sl,
            [](lv_event_t *e) {
                int v = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
                if (s_ai_sh_lbl) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%d", v);
                    lv_label_set_text(s_ai_sh_lbl, buf);
                }
                s_cfg.ai_sh_w = (uint8_t)v;
                nodelio_nvs_cache_put_preferences(&s_cfg);
                nl_ov_ai_apply_metrics();
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(
            ai_sh_sl,
            [](lv_event_t *e) {
                char num[12];
                std::snprintf(num, sizeof(num), "%d",
                              (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e)));
                (void)nodelio_panel_ui_set_pref("ai_shadow_w", num, nullptr, 0);
            },
            LV_EVENT_RELEASED, nullptr);

        const int preset = (int)nodelio_ai_border_preset();
        lv_obj_t *color_band = audio_ctrl_band(body, "边框色", "", nullptr);
        lv_obj_t *color_row = lv_obj_create(color_band);
        lv_obj_remove_style_all(color_row);
        lv_obj_set_width(color_row, lv_pct(100));
        lv_obj_set_height(color_row, 44);
        lv_obj_set_flex_flow(color_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(color_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(color_row, 8, 0);
        lv_obj_clear_flag(color_row, LV_OBJ_FLAG_SCROLLABLE);
        static const char *k_border[] = {"默认", "暖灰", "绿色"};
        for (int i = 0; i < 3; i++) {
            const bool on = preset == i;
            lv_obj_t *chip = lv_obj_create(color_row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_flex_grow(chip, 1);
            lv_obj_set_height(chip, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, on ? UI_ACCENT : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(chip, UI_EXT_CLICK);
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t *e) {
                    int v = (int)(intptr_t)lv_event_get_user_data(e);
                    char num[8];
                    std::snprintf(num, sizeof(num), "%d", v);
                    (void)nodelio_panel_ui_set_pref("ai_border_c", num, nullptr, 0);
                    lv_obj_t *row = lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e));
                    const uint32_t n = lv_obj_get_child_count(row);
                    for (uint32_t i = 0; i < n; i++) {
                        lv_obj_t *c = lv_obj_get_child(row, (int32_t)i);
                        const bool sel = (int)i == v;
                        lv_obj_set_style_bg_color(c, sel ? UI_ACCENT : UI_CAPSULE_OFF, 0);
                        lv_obj_t *lab = lv_obj_get_child(c, 0);
                        if (lab) {
                            lv_obj_set_style_text_color(lab, sel ? UI_ON_TILE : UI_TEXT, 0);
                        }
                    }
                },
                LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *lab = lv_label_create(chip);
            nl_ov_set_font(lab);
            lv_label_set_text(lab, k_border[i]);
            lv_obj_set_style_text_color(lab, on ? UI_ON_TILE : UI_TEXT, 0);
            lv_obj_center(lab);
        }
    } else if (page == SET_PAGE_RESET) {
        lv_obj_t *card = settings_card(body, "恢复出厂");
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 16, 0);

        lv_obj_t *warn = lv_label_create(card);
        lv_label_set_text(warn, "将清空 WiFi、服务器凭证与布局缓存，设备会立即重启。");
        lv_obj_set_width(warn, lv_pct(100));
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(warn);
        lv_obj_set_style_text_color(warn, UI_DANGER_FG, 0);

        /* 按钮放 body 上，避免再被卡片裁切 */
        lv_obj_t *btn = ui_make_text_btn(body, "确认重置并重启", 1, on_factory_confirm);
        lv_obj_set_style_margin_top(btn, 4, 0);

        lv_obj_t *hint = lv_label_create(body);
        lv_label_set_text(hint, "取消请点左上角返回");
        lv_obj_set_width(hint, lv_pct(100));
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (s_tabbar && !nodelio_display_profile()->nav_width) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_raise_chrome();
    if (keep_y > 0 && s_settings_detail_body) {
        lv_obj_update_layout(s_settings_detail_body);
        lv_obj_scroll_to_y(s_settings_detail_body, keep_y, LV_ANIM_OFF);
    }
}

static void settings_tile(lv_obj_t *parent, const char *glyph, const char *title, const char *sub,
                          uint32_t accent, const char *right_icon, lv_event_cb_t cb)
{
#if CONFIG_BOARD_TYPE_SILA_P4C5_508
    const bool force_single_col = false;
#else
    const bool force_single_col =
        nodelio_display_profile()->nav_width != 0 && nodelio_skill_result_card_visible();
#endif
    const char* symbol = material_symbols_get_utf8(glyph);
    auto* tile = ui_make_settings_tile(parent, symbol ? symbol : MATERIAL_SYMBOLS_EXPLORE, title,
                                       sub, accent, right_icon, cb, force_single_col);
    auto* icon = lv_obj_get_child(lv_obj_get_child(tile, 0), 0);
#if CONFIG_BOARD_TYPE_SILA_P4C5_508
    lv_obj_set_style_text_font(icon, &font_material_symbols_30_4, 0);
#else
    lv_obj_set_style_text_font(icon, &font_material_symbols_20_4, 0);
#endif
}

#if !CONFIG_BOARD_TYPE_SILA_P4C5_508
static void hub_section_seal(lv_obj_t *sec)
{
    if (!sec) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(sec);
    if (n == 0) {
        return;
    }
    lv_obj_t *last = lv_obj_get_child(sec, n - 1);
    if (last) {
        lv_obj_set_style_border_width(last, 0, 0);
    }
}
#endif

static int s_hub_host_w = -1;
static int s_hub_single = -1;

static void fill_settings_hub(void)
{
    if (!s_settings) {
        return;
    }
    const lv_coord_t keep_y = lv_obj_get_scroll_y(s_settings);
    lv_obj_clean(s_settings);
#if CONFIG_BOARD_TYPE_SILA_P4C5_508
    /* 508：两列磁贴墙。 */
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    const int set_gap = nodelio_text_scale() == 1 ? 12 : 10;
    lv_obj_set_style_pad_row(s_settings, set_gap, 0);
    lv_obj_set_style_pad_column(s_settings, set_gap, 0);
    lv_obj_set_style_pad_left(s_settings, 16, 0);
    lv_obj_set_style_pad_right(s_settings, 16, 0);
    lv_obj_set_style_pad_top(s_settings, 28, 0);
    lv_obj_set_style_pad_bottom(s_settings, 54, 0);
#else
    /* 分组列表：常用 / 连接 / 系统，单列全宽行。 */
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    const int set_gap = nodelio_text_scale() == 1 ? 16 : 14;
    lv_obj_set_style_pad_row(s_settings, set_gap, 0);
    lv_obj_set_style_pad_column(s_settings, 0, 0);
    const bool is_wide = nodelio_display_profile()->nav_width > 0;
    lv_obj_set_style_pad_left(s_settings, 16, 0);
    lv_obj_set_style_pad_right(s_settings, 16, 0);
    lv_obj_set_style_pad_top(s_settings, is_wide ? 16 : 28, 0);
    lv_obj_set_style_pad_bottom(s_settings, is_wide ? 20 : 54, 0);
#endif
    lv_obj_set_style_bg_color(s_settings, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(s_settings, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_settings, LV_OBJ_FLAG_SCROLL_ELASTIC);

    nl_ov_load_audio_prefs();
    char bri_sub[48];
    std::snprintf(bri_sub, sizeof(bri_sub), "%d%% · %d%% · %s", s_brightness, s_volume,
                  nl_ov_ui_mode_name(s_cfg.ui_mode));

#if CONFIG_BOARD_TYPE_SILA_P4C5_508
    lv_obj_t *host = s_settings;
#else
    lv_obj_t *sec = ui_make_settings_section(s_settings, "常用");
    lv_obj_t *host = sec;
#endif
    settings_tile(host, "brightness_6", "显示与声音", bri_sub, UI_C_INFO, nullptr,
                  [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_DISP); });
    settings_tile(host, "home", "控家",
                  nodelio_controls_enabled() ? "已开启" : "已关闭", UI_C_SUCCESS, nullptr,
                  [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_CTRL); });
    {
        char pwr_sub[40];
        if (nodelio_power_save_enabled()) {
            std::snprintf(pwr_sub, sizeof(pwr_sub), "%ds · 网 %ds",
                          (int)nodelio_idle_sleep_seconds(), (int)nodelio_idle_wifi_seconds());
        } else {
            std::snprintf(pwr_sub, sizeof(pwr_sub), "%ds · 不断网",
                          (int)nodelio_idle_sleep_seconds());
        }
        settings_tile(host, "power_settings_new", "省电", pwr_sub, UI_C_WARN, nullptr,
                      [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_POWER); });
    }
    {
        char media_sub[40];
        std::snprintf(media_sub, sizeof(media_sub), "%s · %d%%",
                      nodelio_media_sleep_enabled() ? "可息屏" : "常亮",
                      (int)nodelio_media_wake_duck_pct());
        settings_tile(host, "music_note", "播放器", media_sub, UI_C_PURPLE, nullptr,
                      [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_MEDIA); });
    }
#if !CONFIG_BOARD_TYPE_SILA_P4C5_508
    hub_section_seal(sec);
    sec = ui_make_settings_section(s_settings, "连接");
    host = sec;
#endif
    settings_tile(host, "wifi", "网络与连接", "WiFi · 服务器", 0x00B4D8, nullptr,
                  [](lv_event_t*) { nl_ov_open_connect(); });
    settings_tile(
        host, "refresh", "刷新布局", "重新拉取", 0xF2994A, nullptr,
        [](lv_event_t*) { (void)nodelio_panel_ui_set_pref("layout_sync", "", nullptr, 0); });
    {
        char voice_sub[40];
        std::snprintf(voice_sub, sizeof(voice_sub), "%s · %ds",
                      nodelio_xiaozhi_enabled() ? "小智" : "无云",
                      (int)nodelio_listen_idle_seconds());
        settings_tile(host, "mic", "语音助手", voice_sub, 0x8E44AD, nullptr,
                      [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_VOICE); });
    }
#if !CONFIG_BOARD_TYPE_SILA_P4C5_508
    hub_section_seal(sec);
    sec = ui_make_settings_section(s_settings, "系统");
    host = sec;
#endif
#if CONFIG_FIRMWARE_OTA_ENABLED
    settings_tile(host, "cloud_download", "检查固件更新", "检查更新", 0x0284C7, nullptr,
                  [](lv_event_t*) {
                      char err[64];
                      if (nodelio_panel_ui_set_pref("fw_check", "", err, sizeof(err)) != ESP_OK &&
                          err[0]) {
                          nl_ov_toast(err);
                      }
                  });
#endif
    settings_tile(host, "info", "设备信息", "状态", 0x3D5A80, nullptr,
                  [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_SYS); });
    settings_tile(host, "build", "硬件自检", "触摸 · 麦", 0x5B5EA6, nullptr,
                  [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_DIAGNOSTICS); });
    settings_tile(host, "restart_alt", "恢复出厂", "清空并重启", UI_C_DANGER, nullptr,
                  [](lv_event_t*) { nl_ov_open_settings_detail(SET_PAGE_RESET); });
#if !CONFIG_BOARD_TYPE_SILA_P4C5_508
    hub_section_seal(sec);
#endif

    s_hub_host_w = (int)lv_obj_get_width(s_settings);
    s_hub_single =
        (nodelio_display_profile()->nav_width != 0 && nodelio_skill_result_card_visible()) ? 1 : 0;
    if (keep_y > 0) {
        lv_obj_update_layout(s_settings);
        lv_obj_scroll_to_y(s_settings, keep_y, LV_ANIM_OFF);
    }
}

void nl_ov_fill_settings(void)
{
    if (!s_settings) {
        return;
    }
    nl_ov_close_settings_detail();
    s_hub_host_w = -1;
    s_hub_single = -1;
    fill_settings_hub();
}

void nl_ov_settings_sync_columns(void)
{
    if (!s_settings || s_settings_detail) {
        return;
    }
    const int host = (int)lv_obj_get_width(s_settings);
    if (host <= 0) {
        return;
    }
    const int single =
        (nodelio_display_profile()->nav_width != 0 && nodelio_skill_result_card_visible()) ? 1 : 0;
    if (host > 0 && host == s_hub_host_w && single == s_hub_single &&
        lv_obj_get_child_count(s_settings) > 0) {
        return;
    }
    fill_settings_hub();
}

void nl_ov_settings_reapply_text_scale(void)
{
    const bool detail = s_settings_detail != nullptr;
    const set_page_id_t page = s_open_set_page;
    if (s_settings) {
        nl_ov_fill_settings();
    }
    if (detail) {
        nl_ov_open_settings_detail(page);
    }
}
