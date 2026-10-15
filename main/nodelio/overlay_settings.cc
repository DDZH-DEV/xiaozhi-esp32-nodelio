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
    if (s_tabbar && s_visible) {
        lv_obj_clear_flag(s_tabbar, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_restore_ai_drawer_if_session();
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
        sys_kv_row(dev, "唤醒词", "你好小智", nullptr);
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
    } else if (page == SET_PAGE_RESET) {
        lv_obj_t *card = settings_card(body, "恢复出厂");
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_pad_row(card, 16, 0);
        lv_obj_set_style_pad_bottom(card, 28, 0);

        lv_obj_t *warn = lv_label_create(card);
        lv_label_set_text(warn, "将清空 WiFi、服务器凭证与布局缓存，设备会立即重启。");
        lv_obj_set_width(warn, lv_pct(100));
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        nl_ov_set_font(warn);
        lv_obj_set_style_text_color(warn, UI_DANGER_FG, 0);

        lv_obj_t *btn = ui_make_text_btn(card, "确认重置并重启", 1, on_factory_confirm);
        lv_obj_set_style_margin_top(btn, 8, 0);
        lv_obj_set_style_margin_bottom(btn, 10, 0);

        lv_obj_t *hint = lv_label_create(card);
        lv_label_set_text(hint, "取消请点左上角返回");
        lv_obj_set_width(hint, lv_pct(100));
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(hint, 4, 0);
        lv_obj_set_style_pad_bottom(hint, 8, 0);
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
    lv_obj_set_flex_flow(s_settings, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_settings, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_settings, 10, 0);
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
    settings_tile(s_settings, "连", "网络与连接", "扫描 WiFi / 服务器 / 绑定", UI_C_WARN, nullptr,
                  [](lv_event_t *) { nl_ov_open_connect(); });
    settings_tile(s_settings, "刷", "刷新布局", "重新拉取首页控件", UI_C_INFO, nullptr, [](lv_event_t *) {
        nl_ov_toast("正在刷新…");
        nodelio_layout_request_sync();
    });
    settings_tile(s_settings, "机", "设备信息", "唤醒词「你好小智」· 状态", UI_C_PURPLE, nullptr,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_SYS); });
    settings_tile(s_settings, "复", "恢复出厂", "清空凭证并重启", UI_C_DANGER, nullptr,
                  [](lv_event_t *) { open_settings_detail(SET_PAGE_RESET); });
}
