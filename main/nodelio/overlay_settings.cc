#include "overlay_priv.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
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

#include <esp_app_desc.h>
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
#include <strings.h>

#define TAG "nl_set"

static void on_factory_confirm(lv_event_t *e)
{
    (void)e;
    nl_ov_do_factory_reset();
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
        /* permanent=false：不写 Settings/NVS（LVGL PSRAM 栈禁 flash） */
        bl->SetBrightness(pct, false);
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
        codec->SetOutputVolumeTransient((int)pct);
    }
}

static void persist_ui_prefs(void)
{
    /* s_cfg 为权威镜像；LVGL 禁止 flash，主任务落盘（含 audio/display Settings） */
    s_cfg.brightness = s_brightness;
    s_cfg.volume = s_volume;
    nodelio_nvs_cache_put(&s_cfg);
    nodelio_nvs_config_t snap = s_cfg;
    uint8_t bri = s_brightness;
    uint8_t vol = s_volume;
    Application::GetInstance().Schedule([snap, bri, vol]() {
        (void)nodelio_nvs_save(&snap);
        auto *bl = Board::GetInstance().GetBacklight();
        if (bl) {
            bl->SetBrightness(bri, true);
        }
        auto *codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->SetOutputVolume((int)vol);
        }
    });
}

static lv_obj_t *s_idle_slp_lbl;
static lv_obj_t *s_wifi_slp_lbl;
static lv_obj_t *s_bye_slp_lbl;
static lv_obj_t *s_listen_idle_lbl;
static lv_obj_t *s_media_pause_lbl;

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
    int v = (int)lv_slider_get_value(sl);
    if (v < NODELIO_LISTEN_IDLE_MIN) {
        v = NODELIO_LISTEN_IDLE_MIN;
    }
    if (v > NODELIO_LISTEN_IDLE_MAX) {
        v = NODELIO_LISTEN_IDLE_MAX;
    }
    s_cfg.listen_idle_s = (uint16_t)v;
    nodelio_nvs_cache_put(&s_cfg);
    nodelio_nvs_config_t snap = s_cfg;
    Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save(&snap); });
    nl_ov_toast("无对话退下时间已保存");
}

static void on_sleep_release(lv_event_t *e)
{
    lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
    int idle = s_cfg.idle_sleep_s ? (int)s_cfg.idle_sleep_s : NODELIO_IDLE_SLEEP_DEFAULT;
    int wifi = s_cfg.idle_wifi_s ? (int)s_cfg.idle_wifi_s : NODELIO_IDLE_WIFI_DEFAULT;
    int bye = s_cfg.goodbye_sleep_s ? (int)s_cfg.goodbye_sleep_s : NODELIO_BYE_SLEEP_DEFAULT;
    /* user_data：1=idle 屏 2=bye 3=idle 断网 */
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(sl);
    if (which == 1) {
        idle = v;
    } else if (which == 3) {
        wifi = v;
    } else {
        bye = v;
    }
    s_cfg.idle_sleep_s = (uint16_t)idle;
    s_cfg.idle_wifi_s = (uint16_t)wifi;
    s_cfg.goodbye_sleep_s = (uint16_t)bye;
    nodelio_nvs_cache_put(&s_cfg);
    Board::GetInstance().SetIdleSleepSeconds(idle);
    Board::GetInstance().SetIdleWifiSeconds(wifi);
    Board::GetInstance().NoteUserActivity();
    nodelio_nvs_config_t snap = s_cfg;
    Application::GetInstance().Schedule([snap]() {
        (void)nodelio_nvs_save(&snap);
    });
    nl_ov_toast(which == 3 ? "断网时间已保存" : "息屏时间已保存");
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
    nl_ov_toast("音量已保存");
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
    nl_ov_set_font(k);
    lv_obj_set_style_text_color(k, UI_TEXT_MUTED, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, val_init ? val_init : "-");
    lv_obj_set_width(v, 200);
    lv_label_set_long_mode(v, LV_LABEL_LONG_CLIP);
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
    if (!s_sys_up_lbl) {
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
    if (!s_settings_detail) {
        return;
    }
    lv_obj_delete(s_settings_detail);
    s_settings_detail = nullptr;
    s_bri_val_lbl = nullptr;
    s_vol_val_lbl = nullptr;
    s_idle_slp_lbl = nullptr;
    s_wifi_slp_lbl = nullptr;
    s_bye_slp_lbl = nullptr;
    s_listen_idle_lbl = nullptr;
    s_media_pause_lbl = nullptr;
    if (s_tabbar && s_visible) {
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_restore_ai_drawer_if_session();
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
    lv_obj_set_style_radius(card, 10, 0);
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

static void open_settings_detail(set_page_id_t page)
{
    nl_ov_close_settings_detail();
    if (!s_root) {
        return;
    }
    load_audio_prefs();

    /* 挂在 lv_layer_top() 防止状态栏被 move_foreground 推到底部 */
    s_settings_detail = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_settings_detail, LV_HOR_RES, LV_VER_RES - NL_UI_STATUS_H);
    lv_obj_align(s_settings_detail, LV_ALIGN_TOP_LEFT, 0, NL_UI_STATUS_H);
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
    }
    (void)ui_make_nav_bar(s_settings_detail, UI_NAV_H, title,
                          [](lv_event_t *) { nl_ov_close_settings_detail(); });

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
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);
    } else if (page == SET_PAGE_SYS) {
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
            const esp_app_desc_t *app = esp_app_get_description();
            if (app) {
                std::snprintf(line, sizeof(line), "%s %s", app->date, app->time);
            } else {
                std::snprintf(line, sizeof(line), "-");
            }
            sys_kv_row(dev, "编译时间", line, nullptr);
        }
        sys_kv_row(dev, "作者", "Dev-SL", nullptr);
        sys_kv_row(dev, "唤醒词", "你好小智", nullptr);
        sys_kv_row(dev, "小智云", s_cfg.xiaozhi_en ? "已启用" : "已关闭", nullptr);
        if (s_cfg.power_save_en) {
            std::snprintf(line, sizeof(line), "屏%ds · 网%ds · 退%d", (int)s_cfg.idle_sleep_s,
                          (int)s_cfg.idle_wifi_s, (int)s_cfg.goodbye_sleep_s);
        } else {
            std::snprintf(line, sizeof(line), "已关闭");
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

        lv_obj_t *ps_card = settings_card(body, "息屏断网");
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
        lv_obj_set_size(ps_sw, 52, 30);
        lv_obj_set_style_bg_color(ps_sw, lv_color_hex(UI_C_SLIDER_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ps_sw, UI_ACCENT,
                                  (lv_style_selector_t)((uint32_t)LV_PART_INDICATOR |
                                                       (uint32_t)LV_STATE_CHECKED));
        if (s_cfg.power_save_en) {
            lv_obj_add_state(ps_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ps_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            ps_sw,
            [](lv_event_t *e) {
                lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
                bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);
                s_cfg.power_save_en = on ? 1 : 0;
                nodelio_nvs_cache_put(&s_cfg);
                Board::GetInstance().SetPowerSaveEnabled(on);
                if (on) {
                    Board::GetInstance().NoteUserActivity();
                }
                nodelio_nvs_config_t snap = s_cfg;
                Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save(&snap); });
                nl_ov_toast(on ? "已开启：按下方时间息屏/断网" : "已关闭：保持亮屏与 WiFi");
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t *ps_hint = lv_label_create(ps_card);
        lv_label_set_text(ps_hint, "关闭后永不息屏、不断网（更耗电，适合调试）");
        nl_ov_set_font(ps_hint);
        lv_obj_set_style_text_color(ps_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(ps_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ps_hint, lv_pct(100));

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
        lv_obj_set_size(ms_sw, 52, 30);
        lv_obj_set_style_bg_color(ms_sw, lv_color_hex(UI_C_SLIDER_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ms_sw, UI_ACCENT,
                                  (lv_style_selector_t)((uint32_t)LV_PART_INDICATOR |
                                                       (uint32_t)LV_STATE_CHECKED));
        if (s_cfg.media_sleep_en) {
            lv_obj_add_state(ms_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ms_sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            ms_sw,
            [](lv_event_t *e) {
                lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
                bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);
                s_cfg.media_sleep_en = on ? 1 : 0;
                nodelio_nvs_cache_put(&s_cfg);
                nodelio_nvs_config_t snap = s_cfg;
                Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save(&snap); });
                nl_ov_toast(on ? "播放中可息屏，音乐继续" : "播放中保持亮屏");
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
                lv_obj_t *sl = (lv_obj_t *)lv_event_get_target(e);
                int v = (int)lv_slider_get_value(sl);
                if (v < NODELIO_MEDIA_PAUSE_MIN) {
                    v = NODELIO_MEDIA_PAUSE_MIN;
                }
                if (v > NODELIO_MEDIA_PAUSE_MAX) {
                    v = NODELIO_MEDIA_PAUSE_MAX;
                }
                s_cfg.media_pause_s = (uint16_t)v;
                nodelio_nvs_cache_put(&s_cfg);
                nodelio_nvs_config_t snap = s_cfg;
                Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save(&snap); });
                nl_ov_toast("暂停关闭时间已保存");
            },
            LV_EVENT_RELEASED, nullptr);
        lv_obj_t *pause_hint = lv_label_create(pause_band);
        lv_label_set_text(pause_hint, "暂停后多久自动关掉播放条并释放会话");
        nl_ov_set_font(pause_hint);
        lv_obj_set_style_text_color(pause_hint, UI_TEXT_TIP, 0);
        lv_label_set_long_mode(pause_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(pause_hint, lv_pct(100));
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
        lv_obj_set_size(sw, 52, 30);
        lv_obj_set_style_bg_color(sw, lv_color_hex(UI_C_SLIDER_TRACK), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, UI_ACCENT,
                                  (lv_style_selector_t)((uint32_t)LV_PART_INDICATOR |
                                                       (uint32_t)LV_STATE_CHECKED));
        if (s_cfg.xiaozhi_en) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        lv_obj_add_event_cb(
            sw,
            [](lv_event_t *e) {
                lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
                bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);
                s_cfg.xiaozhi_en = on ? 1 : 0;
                nodelio_nvs_cache_put(&s_cfg);
                nodelio_nvs_config_t snap = s_cfg;
                auto &app = Application::GetInstance();
                app.Schedule([snap]() { (void)nodelio_nvs_save(&snap); });
                if (on) {
                    app.MarkTransportSuspended(false);
                    app.ResumeTransport();
                    nl_ov_toast("已启用小智云");
                } else {
                    app.Schedule([]() {
                        auto &a = Application::GetInstance();
                        if (a.GetDeviceState() == kDeviceStateListening ||
                            a.GetDeviceState() == kDeviceStateSpeaking ||
                            a.GetDeviceState() == kDeviceStateConnecting) {
                            a.SetDeviceState(kDeviceStateIdle);
                        }
                        a.SuspendTransport();
                        a.MarkTransportSuspended(true);
                    });
                    nl_ov_toast("已关闭：唤醒走本地口令");
                }
            },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint,
                          "关闭后不再连接 api.tenclass.net，唤醒不等公网 MQTT；"
                          "NodeLio 控家与本地口令仍可用。云端不可用时建议关闭。");
        lv_obj_set_width(hint, lv_pct(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_TIP, 0);

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

    if (s_tabbar) {
        lv_obj_add_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    /* s_settings_detail 已挂在 lv_layer_top()，无需 move_foreground */
}

static void settings_tile(lv_obj_t *parent, const char *glyph, const char *title, const char *sub,
                          uint32_t accent, const char *right_icon, lv_event_cb_t cb)
{
    (void)ui_make_settings_tile(parent, glyph, title, sub, accent, right_icon, cb);
}

void nl_ov_fill_settings(void)
{
    if (!s_settings) {
        return;
    }
    nl_ov_close_settings_detail();
    lv_obj_clean(s_settings);
    /* 一行两列；卡内左右布局 */
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_settings, UI_GAP, 0);
    lv_obj_set_style_pad_column(s_settings, UI_GAP, 0);
    lv_obj_set_style_pad_left(s_settings, 16, 0);
    lv_obj_set_style_pad_right(s_settings, 16, 0);
    lv_obj_set_style_pad_top(s_settings, 16, 0);
    lv_obj_set_style_pad_bottom(s_settings, 20, 0);
    lv_obj_set_style_bg_color(s_settings, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(s_settings, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_settings, LV_SCROLLBAR_MODE_OFF);

    load_audio_prefs();
    char bri_sub[32];
    std::snprintf(bri_sub, sizeof(bri_sub), "亮度 %d%% · 音量 %d%%", s_brightness, s_volume);

    settings_tile(s_settings, "显", "显示与声音", bri_sub, UI_C_ACCENT, nullptr,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_DISP); });
    {
        char pwr_sub[48];
        if (nodelio_power_save_enabled()) {
            std::snprintf(pwr_sub, sizeof(pwr_sub), "屏 %ds · 网 %ds",
                          (int)nodelio_idle_sleep_seconds(), (int)nodelio_idle_wifi_seconds());
        } else {
            std::snprintf(pwr_sub, sizeof(pwr_sub), "已关闭 · 常亮联网");
        }
        settings_tile(s_settings, "电", "省电", pwr_sub, UI_C_WARN, nullptr,
                      [](lv_event_t *) { open_settings_detail(SET_PAGE_POWER); });
    }
    {
        char media_sub[48];
        std::snprintf(media_sub, sizeof(media_sub), "%s · 暂停 %ds",
                      nodelio_media_sleep_enabled() ? "播放可息屏" : "播放常亮",
                      (int)nodelio_media_pause_seconds());
        settings_tile(s_settings, "播", "播放器", media_sub, UI_C_INFO, nullptr,
                      [](lv_event_t *) { open_settings_detail(SET_PAGE_MEDIA); });
    }
    settings_tile(s_settings, "连", "网络与连接", "扫描 WiFi / 服务器 / 绑定", UI_C_WARN, nullptr,
                  [](lv_event_t *) { nl_ov_open_connect(); });
    settings_tile(s_settings, "刷", "刷新布局", "重新拉取首页控件", UI_C_INFO, nullptr, [](lv_event_t *) {
        nl_ov_toast("正在刷新…");
        nodelio_layout_request_sync();
    });
    {
        char voice_sub[48];
        std::snprintf(voice_sub, sizeof(voice_sub), "%s · 无对话 %ds",
                      nodelio_xiaozhi_enabled() ? "小智云已开" : "仅本地口令",
                      (int)nodelio_listen_idle_seconds());
        settings_tile(s_settings, "智", "语音助手", voice_sub, UI_C_ACCENT, nullptr,
                      [](lv_event_t *) { open_settings_detail(SET_PAGE_VOICE); });
    }
    settings_tile(s_settings, "更", "检查固件更新", "手工查小智云版本，有新版再升级", UI_C_INFO, nullptr,
                  [](lv_event_t *) { Application::GetInstance().RequestManualFirmwareCheck(); });
    settings_tile(s_settings, "机", "设备信息", "唤醒词「你好小智」· 状态", UI_C_PURPLE, nullptr,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_SYS); });
    settings_tile(s_settings, "复", "恢复出厂", "清空凭证并重启", UI_C_DANGER, nullptr,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_RESET); });
}
