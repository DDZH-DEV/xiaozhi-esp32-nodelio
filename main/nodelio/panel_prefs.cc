#include "overlay_priv.h"

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "media_player.h"
#include "nvs_config.h"
#include "offline_cmd.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
#include "ui_theme.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_prefs"

void nl_ov_load_audio_prefs(void)
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

static void pref_err(char *err, size_t err_len, const char *msg)
{
    if (err && err_len > 0 && msg) {
        std::snprintf(err, err_len, "%s", msg);
    }
}

void nl_ov_apply_brightness(uint8_t pct)
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

void nl_ov_apply_volume(uint8_t pct)
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

void nl_ov_persist_ui_prefs(bool volume_changed)
{
    /* s_cfg 为权威镜像；LVGL 禁止 flash，主任务落盘（含 audio/display Settings） */
    s_cfg.brightness = s_brightness;
    s_cfg.volume = s_volume;
    nodelio_nvs_cache_put_preferences(&s_cfg);
    nodelio_nvs_config_t snap = s_cfg;
    uint8_t bri = s_brightness;
    uint8_t vol = s_volume;
    Application::GetInstance().Schedule([snap, bri, vol, volume_changed]() {
        esp_err_t err = nodelio_nvs_save_preferences(&snap);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Preference save failed: %s", esp_err_to_name(err));
            nodelio_panel_ui_toast_defer("设置保存失败，请检查设备存储");
            return;
        }
        auto *bl = Board::GetInstance().GetBacklight();
        if (bl) {
            err = bl->SaveBrightness(bri);
            if (err != ESP_OK) {
                nodelio_panel_ui_toast_defer("亮度保存失败，请检查设备存储");
                return;
            }
        }
        auto *codec = Board::GetInstance().GetAudioCodec();
        if (codec && volume_changed) {
            codec->SetOutputVolume((int)vol);
        }
    });
}

void nl_ov_refresh_bri_vol_labels(void)
{
    if (!s_bri_val_lbl && !s_vol_val_lbl) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    char buf[12];
    if (s_bri_val_lbl) {
        std::snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(s_bri_val_lbl, buf);
    }
    if (s_vol_val_lbl) {
        std::snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(s_vol_val_lbl, buf);
    }
}

static bool parse_pct_0_100(const char *value, int *out, char *err, size_t err_len)
{
    if (!value || !value[0] || !out) {
        pref_err(err, err_len, "value 不能为空");
        return false;
    }
    char *end = nullptr;
    long v = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        pref_err(err, err_len, "value 须为 0–100 整数");
        return false;
    }
    if (v < 0 || v > 100) {
        pref_err(err, err_len, "value 须为 0–100");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool parse_on_off(const char *value, bool *out, char *err, size_t err_len)
{
    if (!value || !value[0] || !out) {
        pref_err(err, err_len, "value 不能为空");
        return false;
    }
    if (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0 || strcmp(value, "开") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0 || strcmp(value, "关") == 0) {
        *out = false;
        return true;
    }
    pref_err(err, err_len, "value 须为 on/off");
    return false;
}

static bool parse_int_range(const char *value, int min_v, int max_v, int *out, char *err,
                            size_t err_len)
{
    if (!value || !value[0] || !out) {
        pref_err(err, err_len, "value 不能为空");
        return false;
    }
    char *end = nullptr;
    long v = std::strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        pref_err(err, err_len, "value 须为整数秒");
        return false;
    }
    if (v < min_v || v > max_v) {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "须为 %d–%d 秒", min_v, max_v);
        pref_err(err, err_len, msg);
        return false;
    }
    *out = (int)v;
    return true;
}

void nl_ov_persist_cfg_async(void)
{
    nodelio_nvs_cache_put_preferences(&s_cfg);
    nodelio_nvs_config_t snap = s_cfg;
    Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save_preferences(&snap); });
}

const char *nl_ov_text_scale_name(uint8_t scale)
{
    static const char *k_name[] = {"标准", "较大"};
    if (scale > NODELIO_TEXT_SCALE_MAX) {
        scale = NODELIO_TEXT_SCALE_DEFAULT;
    }
    return k_name[scale];
}

static bool parse_text_scale(const char *value, uint8_t *out, char *err, size_t err_len)
{
    if (!value || !value[0] || !out) {
        pref_err(err, err_len, "文字档须为 0–2");
        return false;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "标准") == 0) {
        *out = 0;
        return true;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "较大") == 0 || strcmp(value, "2") == 0 || strcmp(value, "更大") == 0) {
        *out = 1;
        return true;
    }
    pref_err(err, err_len, "文字档须为 0–1（标准/较大）");
    return false;
}

const char *nl_ov_ui_mode_name(uint8_t mode)
{
    if (mode == 2) {
        return "沉浸";
    }
    return mode == 1 ? "深色" : "浅色";
}

static bool parse_ui_mode(const char *value, uint8_t *out, char *err, size_t err_len)
{
    if (!value || !value[0] || !out) {
        pref_err(err, err_len, "外观须为 0 浅色、1 深色或 2 沉浸");
        return false;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "浅色") == 0 || strcasecmp(value, "light") == 0) {
        *out = 0;
        return true;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "深色") == 0 || strcasecmp(value, "dark") == 0) {
        *out = 1;
        return true;
    }
#if CONFIG_BOARD_TYPE_SILA_P4C5_62 || CONFIG_BOARD_TYPE_SILA_P4C5_508
    if (strcmp(value, "2") == 0 || strcmp(value, "沉浸") == 0 || strcasecmp(value, "immersive") == 0) {
        *out = 2;
        return true;
    }
#endif
    pref_err(err, err_len, "外观须为 0 浅色、1 深色或 2 沉浸");
    return false;
}

void nl_ov_commit_ui_mode(uint8_t mode)
{
#if CONFIG_BOARD_TYPE_SILA_P4C5_62 || CONFIG_BOARD_TYPE_SILA_P4C5_508
    if (mode > 2) {
        mode = 0;
    }
#else
    mode = mode == 1 ? 1 : 0;
#endif
    if (s_cfg.ui_mode == mode && (uint8_t)ui_palette_mode() == mode) {
        return;
    }
    s_cfg.ui_mode = mode;
    ui_palette_set(mode);
    nl_ov_persist_cfg_async();
    Application::GetInstance().Schedule([mode]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        nl_ov_apply_appearance();
        nl_ov_toast(mode == 2 ? "外观 沉浸" : (mode == 1 ? "外观 深色" : "外观 浅色"));
    });
}

void nl_ov_commit_text_scale(uint8_t scale)
{
    if (scale > NODELIO_TEXT_SCALE_MAX) {
        scale = NODELIO_TEXT_SCALE_DEFAULT;
    }
    if (nodelio_text_scale() == scale) {
        s_cfg.text_scale = scale;
        return;
    }
    s_cfg.text_scale = scale;
    nl_ov_persist_cfg_async();
    char toast[32];
    std::snprintf(toast, sizeof(toast), "文字 %s", nl_ov_text_scale_name(scale));
    nl_ov_toast(toast);
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            nl_ov_apply_text_scale();
        }
    });
}

void nl_ov_commit_status_bar_mode(uint8_t mode)
{
    if (mode > 2) {
        mode = 0;
    }
    if (s_cfg.status_bar_mode == mode) {
        return;
    }
    s_cfg.status_bar_mode = mode;
    nl_ov_persist_cfg_async();
    nl_ov_status_bar_apply_mode();
    static const char *k_sb_names[] = {"自动隐藏", "常显", "常隐"};
    char toast[32];
    std::snprintf(toast, sizeof(toast), "状态栏 %s", k_sb_names[mode]);
    nl_ov_toast(toast);
    nl_ov_open_settings_detail(SET_PAGE_DISP);
}

void nl_ov_commit_screenshot_fab(uint8_t en)
{
    en = en ? 1 : 0;
    if (s_cfg.screenshot_fab_en == en) {
        return;
    }
    s_cfg.screenshot_fab_en = en;
    nl_ov_persist_cfg_async();
    nl_ov_shot_fab_apply_state();
    nl_ov_toast(en ? "已开启截图悬浮图标" : "已隐藏截图悬浮图标");
    nl_ov_open_settings_detail(SET_PAGE_DISP);
}

void nl_ov_commit_card_radius(uint8_t rad_px)
{
    rad_px = rad_px > 24 ? 12 : rad_px;
    if (s_cfg.card_radius == rad_px) {
        return;
    }
    s_cfg.card_radius = rad_px;
    nl_ov_persist_cfg_async();
    Application::GetInstance().Schedule([rad_px]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        nl_ov_apply_appearance();
        /* 宽屏 0px=应用卡贴边：须立刻重排技能卡宽高与圆角。 */
        nodelio_skill_result_layout();
        char toast[48];
        if (nodelio_display_profile()->nav_width && rad_px == 0) {
            std::snprintf(toast, sizeof(toast), "应用卡贴边（无圆角）");
        } else {
            std::snprintf(toast, sizeof(toast), "卡片圆角已设为 %dpx", (int)rad_px);
        }
        nl_ov_toast(toast);
        nl_ov_open_settings_detail(SET_PAGE_DISP);
    });
}

/** 小智 / 本地口令切换后内存占用不同，须重启才按新组合加载。
 * 两个开关常连着拨：合并成一次延迟重启，避免关一个重启、再开另一个又重启。 */
static esp_timer_handle_t s_voice_reboot_tmr;

static void voice_reboot_cb(void *arg)
{
    (void)arg;
    Application::GetInstance().Schedule([]() {
        nodelio_nvs_config_t snap;
        nodelio_nvs_cache_get(&snap);
        (void)nodelio_nvs_save_preferences(&snap);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    });
}

void nl_ov_persist_voice_and_reboot(void)
{
    nodelio_nvs_cache_put_preferences(&s_cfg);
    nodelio_nvs_config_t snap = s_cfg;
    Application::GetInstance().Schedule([snap]() { (void)nodelio_nvs_save_preferences(&snap); });

    if (!s_voice_reboot_tmr) {
        esp_timer_create_args_t args = {
            .callback = voice_reboot_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "nl_voice_rb",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_voice_reboot_tmr) != ESP_OK) {
            nl_ov_toast("语音助手已切换，正在重启…");
            Application::GetInstance().Schedule([]() {
                vTaskDelay(pdMS_TO_TICKS(400));
                esp_restart();
            });
            return;
        }
    }
    esp_timer_stop(s_voice_reboot_tmr);
    /* 3s：够关小智再开本地口令，或反过来 */
    (void)esp_timer_start_once(s_voice_reboot_tmr, 3000000ULL);
    nl_ov_toast("语音已改，3 秒后重启（可继续切换）");
}

void nl_ov_apply_sleep_board(void)
{
    Board::GetInstance().SetIdleSleepSeconds(
        s_cfg.idle_sleep_s ? (int)s_cfg.idle_sleep_s : NODELIO_IDLE_SLEEP_DEFAULT);
    Board::GetInstance().SetIdleWifiSeconds(
        s_cfg.idle_wifi_s ? (int)s_cfg.idle_wifi_s : NODELIO_IDLE_WIFI_DEFAULT);
    Board::GetInstance().NoteUserActivity();
}

/** 云端常猜错 idle_*：映射常见别名到规范 key（返回静态串，勿 free） */
static const char *normalize_pref_key(const char *key)
{
    if (!key || !key[0]) {
        return key;
    }
    if (strcasecmp(key, "idle_wifi_s") == 0 || strcasecmp(key, "idle_wifi") == 0 ||
        strcasecmp(key, "wifi_idle_s") == 0 || strcasecmp(key, "wifi_sleep_s") == 0 ||
        strcasecmp(key, "idle_disconnect_s") == 0 || strcasecmp(key, "disconnect_s") == 0 ||
        strcasecmp(key, "idle_net_s") == 0 || strcasecmp(key, "net_idle_s") == 0) {
        return "idle_wifi_s";
    }
    if (strcasecmp(key, "idle_sleep_s") == 0 || strcasecmp(key, "idle_sleep") == 0 ||
        strcasecmp(key, "screen_idle_s") == 0 || strcasecmp(key, "sleep_s") == 0) {
        return "idle_sleep_s";
    }
    if (strcasecmp(key, "bye_sleep_s") == 0 || strcasecmp(key, "bye_sleep") == 0 ||
        strcasecmp(key, "goodbye_sleep_s") == 0 || strcasecmp(key, "goodbye_s") == 0) {
        return "bye_sleep_s";
    }
    if (strcasecmp(key, "text_scale") == 0 || strcasecmp(key, "font_scale") == 0 ||
        strcasecmp(key, "txt_scl") == 0 || strcmp(key, "字号") == 0) {
        return "text_scale";
    }
    if (strcasecmp(key, "ui_mode") == 0 || strcasecmp(key, "appearance") == 0 ||
        strcasecmp(key, "theme") == 0 || strcmp(key, "外观") == 0) {
        return "ui_mode";
    }
    return key;
}

extern "C" esp_err_t nodelio_panel_ui_set_pref(const char *key, const char *value, char *err,
                                               size_t err_len)
{
    if (err && err_len > 0) {
        err[0] = '\0';
    }
    if (!key || !key[0]) {
        pref_err(err, err_len, "key 不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    if (!value) {
        pref_err(err, err_len, "value 不能为空");
        return ESP_ERR_INVALID_ARG;
    }
    key = normalize_pref_key(key);
    if (!nodelio_display_profile()->network_sleep &&
        (strcasecmp(key, "power_save") == 0 || strcasecmp(key, "idle_wifi_s") == 0)) {
        pref_err(err, err_len, "此板当前只息屏，保持网络连接");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (strcasecmp(key, "brightness") == 0) {
        int v = 0;
        if (!parse_pct_0_100(value, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        /* 下限钳 5：0–4 → 5，其余不静默夹 */
        uint8_t pct = (uint8_t)((v < 5) ? 5 : v);
        nl_ov_apply_brightness(pct);
        nl_ov_persist_ui_prefs(false);
        nl_ov_refresh_bri_vol_labels();
        char toast[32];
        std::snprintf(toast, sizeof(toast), "亮度 %d%%", (int)s_brightness);
        nl_ov_toast(toast);
        return ESP_OK;
    }
    if (strcasecmp(key, "volume") == 0) {
        int v = 0;
        if (!parse_pct_0_100(value, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        nl_ov_apply_volume((uint8_t)v);
        nl_ov_persist_ui_prefs(true);
        nl_ov_refresh_bri_vol_labels();
        char toast[32];
        std::snprintf(toast, sizeof(toast), "音量 %d%%", (int)s_volume);
        nl_ov_toast(toast);
        return ESP_OK;
    }
    if (strcasecmp(key, "notice_sound") == 0) {
        int v = 0;
        if (strcmp(value, "off") == 0 || strcmp(value, "关闭") == 0) {
            v = 0;
        } else if (strcmp(value, "popup") == 0 || strcmp(value, "轻响") == 0 || strcmp(value, "1") == 0) {
            v = 1;
        } else if (strcmp(value, "success") == 0 || strcmp(value, "成功") == 0 || strcmp(value, "2") == 0) {
            v = 2;
        } else if (strcmp(value, "alert") == 0 || strcmp(value, "警示") == 0 || strcmp(value, "3") == 0) {
            v = 3;
        } else if (strcmp(value, "0") == 0) {
            v = 0;
        } else {
            pref_err(err, err_len, "提示音须为 关闭/轻响/成功/警示");
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.notice_sound = (uint8_t)v;
        nl_ov_persist_cfg_async();
        if (v > 0) {
            nodelio_panel_ui_play_cue(v - 1);
        }
        nl_ov_toast(v == 0 ? "已关闭提醒声" : (v == 2 ? "提醒声：成功" : (v == 3 ? "提醒声：警示" : "提醒声：轻响")));
        return ESP_OK;
    }
    if (strcasecmp(key, "notice_repeat") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.notice_repeat = on ? 1 : 0;
        nl_ov_persist_cfg_async();
        nl_ov_toast(on ? "未处理的提醒会隔一会儿再响" : "提醒只响一次");
        return ESP_OK;
    }
    if (strcasecmp(key, "power_save") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.power_save_en = on ? 1 : 0;
        nl_ov_persist_cfg_async();
        Board::GetInstance().SetPowerSaveEnabled(on);
        if (on) {
            Board::GetInstance().NoteUserActivity();
        }
        nl_ov_toast(on ? "已开启空闲断网" : "已关闭空闲断网 · 息屏仍有效");
        return ESP_OK;
    }
    if (strcasecmp(key, "wake_on_sleep") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.wake_on_sleep = on ? 1 : 0;
        nl_ov_persist_cfg_async();
        Board::GetInstance().NoteUserActivity();
        nl_ov_toast(on ? "已开启息屏语音唤醒" : "已关闭息屏语音唤醒 · 触摸/按键唤醒");
        return ESP_OK;
    }
    if (strcasecmp(key, "idle_sleep_s") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX, &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.idle_sleep_s = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_apply_sleep_board();
        nl_ov_toast("息屏时间已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "idle_wifi_s") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX, &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.idle_wifi_s = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_apply_sleep_board();
        nl_ov_toast("断网时间已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "bye_sleep_s") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX, &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.goodbye_sleep_s = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_toast("退下后息屏时间已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "controls") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.controls_en = on ? 1 : 0;
        nl_ov_persist_cfg_async();
        /* 勿在 LVGL 回调内嵌套 Lock；统一延后刷首页 */
        Application::GetInstance().Schedule([]() {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_fill_home();
            }
        });
        nodelio_offline_cmd_rebuild_from_layout(s_layout);
        nl_ov_toast(on ? "已开启控家" : "已关闭控家 · 首页启动台");
        return ESP_OK;
    }
    if (strcasecmp(key, "xiaozhi") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t next = on ? 1 : 0;
        if (s_cfg.xiaozhi_en == next) {
            return ESP_OK;
        }
        s_cfg.xiaozhi_en = next;
        nl_ov_persist_voice_and_reboot();
        return ESP_OK;
    }
    if (strcasecmp(key, "voice_barge_in") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t next = on ? 1 : 0;
        if (s_cfg.voice_barge_in == next) {
            return ESP_OK;
        }
        s_cfg.voice_barge_in = next;
        nodelio_nvs_cache_put(&s_cfg);
        nl_ov_persist_cfg_async();
        nodelio_voice_barge_in_apply(true);
        nl_ov_toast(on ? "已开启语音打断" : "已关闭语音打断");
        return ESP_OK;
    }
    if (strcasecmp(key, "local_cmd") == 0) {
        /* MultiNet 已下线：忽略开关，清掉旧 NVS 位 */
        if (s_cfg.local_cmd_en) {
            s_cfg.local_cmd_en = 0;
            nl_ov_persist_cfg_async();
        }
        if (err && err_len) {
            std::snprintf(err, err_len, "本地口令已下线");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (strcasecmp(key, "listen_idle_s") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_LISTEN_IDLE_MIN, NODELIO_LISTEN_IDLE_MAX, &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.listen_idle_s = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_toast("无对话退下时间已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "media_sleep") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.media_sleep_en = on ? 1 : 0;
        nl_ov_persist_cfg_async();
        nl_ov_toast(on ? "播放中可息屏，音乐继续" : "播放中保持亮屏");
        return ESP_OK;
    }
    if (strcasecmp(key, "media_pause_s") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_MEDIA_PAUSE_MIN, NODELIO_MEDIA_PAUSE_MAX, &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.media_pause_s = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_toast("暂停关闭时间已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "media_wake_duck_pct") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_MEDIA_WAKE_DUCK_MIN, NODELIO_MEDIA_WAKE_DUCK_MAX, &v,
                             err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.media_wake_duck_pct = (uint8_t)v;
        nl_ov_persist_cfg_async();
        nodelio_media_wake_duck_refresh();
        nl_ov_toast("唤醒时音量已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "text_scale") == 0) {
        uint8_t scale = 0;
        if (!parse_text_scale(value, &scale, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        nl_ov_commit_text_scale(scale);
        return ESP_OK;
    }
    if (strcasecmp(key, "ui_mode") == 0) {
        uint8_t mode = 0;
        if (!parse_ui_mode(value, &mode, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        nl_ov_commit_ui_mode(mode);
        return ESP_OK;
    }
    if (strcasecmp(key, "status_bar_mode") == 0 || strcasecmp(key, "status_bar") == 0) {
        int v = 0;
        if (!parse_int_range(value, 0, 2, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        nl_ov_commit_status_bar_mode((uint8_t)v);
        return ESP_OK;
    }
    if (strcasecmp(key, "screenshot_fab") == 0 || strcasecmp(key, "shot_fab") == 0) {
        bool on = false;
        if (!parse_on_off(value, &on, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        nl_ov_commit_screenshot_fab(on ? 1 : 0);
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_w") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_AI_W_MIN, nodelio_ai_drawer_width_max(), &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_w = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话框宽度已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_h") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_AI_H_MIN, nodelio_ai_drawer_height_max(), &v, err,
                             err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_h = (uint16_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话框高度已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_lines") == 0 || strcasecmp(key, "ai_lines") == 0) {
        int v = 0;
        if (!parse_int_range(value, NODELIO_AI_LINES_MIN, NODELIO_AI_LINES_MAX, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_lines = (uint8_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话行数已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_border_w") == 0) {
        int v = 0;
        if (!parse_int_range(value, 0, NODELIO_AI_BD_MAX, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_bd_w = (uint8_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话框边框已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_shadow_w") == 0) {
        int v = 0;
        if (!parse_int_range(value, 0, NODELIO_AI_SH_MAX, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_sh_w = (uint8_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话框阴影已保存");
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_border_c") == 0) {
        int v = 0;
        if (!parse_int_range(value, 0, 2, &v, err, err_len)) {
            return ESP_ERR_INVALID_ARG;
        }
        s_cfg.ai_bd_i = (uint8_t)v;
        nl_ov_persist_cfg_async();
        nl_ov_ai_apply_metrics();
        nl_ov_toast("对话框边框色已保存");
        return ESP_OK;
    }
    /* 动作键：value 可空，只触发 */
    if (strcasecmp(key, "layout_sync") == 0) {
        nl_ov_toast("正在刷新布局…");
        nodelio_layout_request_sync();
        return ESP_OK;
    }
    if (strcasecmp(key, "fw_check") == 0) {
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() != kDeviceStateIdle) {
            pref_err(err, err_len, "请先结束对话再检查更新");
            return ESP_ERR_INVALID_STATE;
        }
        app.RequestManualFirmwareCheck();
        return ESP_OK;
    }
    if (strcasecmp(key, "factory_reset") == 0) {
        /* 语音不执行重置，只打开确认页 */
        nodelio_panel_ui_goto(NL_UI_RESET);
        return ESP_OK;
    }

    pref_err(err, err_len,
             "未知 key。空闲断网=idle_wifi_s；息屏=idle_sleep_s；退下=bye_sleep_s；文字=text_scale；外观=ui_mode");
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t nodelio_panel_ui_get_pref(const char *key, char *buf, size_t len)
{
    if (!key || !key[0] || !buf || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    key = normalize_pref_key(key);
    if (strcasecmp(key, "brightness") == 0) {
        std::snprintf(buf, len, "%d", (int)s_brightness);
        return ESP_OK;
    }
    if (strcasecmp(key, "volume") == 0) {
        std::snprintf(buf, len, "%d", (int)s_volume);
        return ESP_OK;
    }
    if (strcasecmp(key, "power_save") == 0) {
        std::snprintf(buf, len, "%s", nodelio_power_save_enabled() ? "on" : "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "idle_sleep_s") == 0) {
        std::snprintf(buf, len, "%d", (int)s_cfg.idle_sleep_s);
        return ESP_OK;
    }
    if (strcasecmp(key, "idle_wifi_s") == 0) {
        std::snprintf(buf, len, "%d", (int)s_cfg.idle_wifi_s);
        return ESP_OK;
    }
    if (strcasecmp(key, "bye_sleep_s") == 0) {
        std::snprintf(buf, len, "%d", (int)s_cfg.goodbye_sleep_s);
        return ESP_OK;
    }
    if (strcasecmp(key, "controls") == 0) {
        std::snprintf(buf, len, "%s", s_cfg.controls_en ? "on" : "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "xiaozhi") == 0) {
        std::snprintf(buf, len, "%s", s_cfg.xiaozhi_en ? "on" : "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "voice_barge_in") == 0) {
        std::snprintf(buf, len, "%s", s_cfg.voice_barge_in ? "on" : "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "local_cmd") == 0) {
        std::snprintf(buf, len, "%s", "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "listen_idle_s") == 0) {
        std::snprintf(buf, len, "%d",
                      (int)(s_cfg.listen_idle_s ? s_cfg.listen_idle_s : NODELIO_LISTEN_IDLE_DEFAULT));
        return ESP_OK;
    }
    if (strcasecmp(key, "media_sleep") == 0) {
        std::snprintf(buf, len, "%s", s_cfg.media_sleep_en ? "on" : "off");
        return ESP_OK;
    }
    if (strcasecmp(key, "media_pause_s") == 0) {
        std::snprintf(buf, len, "%d",
                      (int)(s_cfg.media_pause_s ? s_cfg.media_pause_s : NODELIO_MEDIA_PAUSE_DEFAULT));
        return ESP_OK;
    }
    if (strcasecmp(key, "media_wake_duck_pct") == 0) {
        std::snprintf(buf, len, "%d",
                      (int)(s_cfg.media_wake_duck_pct ? s_cfg.media_wake_duck_pct
                                                      : NODELIO_MEDIA_WAKE_DUCK_DEFAULT));
        return ESP_OK;
    }
    if (strcasecmp(key, "text_scale") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_text_scale());
        return ESP_OK;
    }
    if (strcasecmp(key, "ui_mode") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ui_mode());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_w") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_drawer_width());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_h") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_drawer_height());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_drawer_lines") == 0 || strcasecmp(key, "ai_lines") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_drawer_lines());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_border_w") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_border_width());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_shadow_w") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_shadow_width());
        return ESP_OK;
    }
    if (strcasecmp(key, "ai_border_c") == 0) {
        std::snprintf(buf, len, "%d", (int)nodelio_ai_border_preset());
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

extern "C" void nodelio_voice_barge_in_apply(bool notify)
{
    const AecMode mode =
        nodelio_voice_barge_in_enabled() ? kAecOnServerSide : kAecOff;
    Application::GetInstance().SetAecMode(mode, notify);
}
