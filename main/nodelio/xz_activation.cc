#include "xz_activation.h"

#include "application.h"
#include "settings.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

/* LVGL / MQTT 等任务栈常在 PSRAM，禁止在其栈上 nvs_open/commit。
 * UI 只读本文件 RAM 缓存；写路径先更新缓存，再 Schedule 到 Application 落盘。 */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_code[32] = {};
static char s_msg[96] = {};
static char s_ota_url[160] = {}; /* 空=用官方 CONFIG_OTA_URL */
static bool s_has_mqtt = false;
static bool s_loaded = false;

static void cache_set_act(const char *code, const char *msg)
{
    portENTER_CRITICAL(&s_lock);
    if (code && code[0]) {
        std::snprintf(s_code, sizeof(s_code), "%s", code);
    } else {
        s_code[0] = '\0';
    }
    if (msg && msg[0]) {
        std::snprintf(s_msg, sizeof(s_msg), "%s", msg);
    } else {
        s_msg[0] = '\0';
    }
    s_loaded = true;
    portEXIT_CRITICAL(&s_lock);
}

static void persist_act_to_nvs(const char *code, const char *msg)
{
    Settings wifi("wifi", true);
    if (code && code[0]) {
        wifi.SetString("xz_act_code", code);
        if (msg && msg[0]) {
            wifi.SetString("xz_act_msg", msg);
        } else {
            wifi.EraseKey("xz_act_msg");
        }
    } else {
        wifi.EraseKey("xz_act_code");
        wifi.EraseKey("xz_act_msg");
    }
}

void nodelio_xz_activation_load(void)
{
    {
        Settings wifi("wifi", false);
        const std::string code = wifi.GetString("xz_act_code");
        const std::string msg = wifi.GetString("xz_act_msg");
        const std::string ota = wifi.GetString("ota_url");
        cache_set_act(code.empty() ? "" : code.c_str(), msg.empty() ? "" : msg.c_str());
        portENTER_CRITICAL(&s_lock);
        if (!ota.empty()) {
            std::snprintf(s_ota_url, sizeof(s_ota_url), "%s", ota.c_str());
        } else {
            s_ota_url[0] = '\0';
        }
        portEXIT_CRITICAL(&s_lock);
    }
    {
        Settings mqtt("mqtt", false);
        const bool has = !mqtt.GetString("endpoint").empty();
        portENTER_CRITICAL(&s_lock);
        s_has_mqtt = has;
        s_loaded = true;
        portEXIT_CRITICAL(&s_lock);
    }
}

void nodelio_xz_activation_store(const char *code, const char *msg)
{
    if (!code || !code[0]) {
        return;
    }
    char trunc_msg[96];
    trunc_msg[0] = '\0';
    if (msg && msg[0]) {
        std::snprintf(trunc_msg, sizeof(trunc_msg), "%s", msg);
    }
    cache_set_act(code, trunc_msg);

    std::string code_copy(code);
    std::string msg_copy(trunc_msg);
    Application::GetInstance().Schedule([code_copy = std::move(code_copy),
                                         msg_copy = std::move(msg_copy)]() {
        persist_act_to_nvs(code_copy.c_str(), msg_copy.empty() ? nullptr : msg_copy.c_str());
    });
}

void nodelio_xz_activation_clear(void)
{
    cache_set_act("", "");
    Application::GetInstance().Schedule([]() { persist_act_to_nvs(nullptr, nullptr); });
}

bool nodelio_xz_activation_get(char *out_code, size_t code_cap, char *out_msg, size_t msg_cap)
{
    if (!s_loaded) {
        if (out_code && code_cap) {
            out_code[0] = '\0';
        }
        if (out_msg && msg_cap) {
            out_msg[0] = '\0';
        }
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    const bool ok = s_code[0] != '\0';
    if (out_code && code_cap) {
        std::snprintf(out_code, code_cap, "%s", s_code);
    }
    if (out_msg && msg_cap) {
        std::snprintf(out_msg, msg_cap, "%s", s_msg);
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

bool nodelio_xz_voice_has_mqtt(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool v = s_loaded && s_has_mqtt;
    portEXIT_CRITICAL(&s_lock);
    return v;
}

void nodelio_xz_voice_set_has_mqtt(bool has)
{
    portENTER_CRITICAL(&s_lock);
    s_has_mqtt = has;
    s_loaded = true;
    portEXIT_CRITICAL(&s_lock);
}

void nodelio_xz_voice_get_ota_url(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_ota_url[0]) {
        std::snprintf(out, cap, "%s", s_ota_url);
    } else {
        out[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

void nodelio_xz_voice_set_ota_url_cache(const char *url)
{
    portENTER_CRITICAL(&s_lock);
    if (url && url[0] && std::strcmp(url, CONFIG_OTA_URL) != 0) {
        std::snprintf(s_ota_url, sizeof(s_ota_url), "%s", url);
    } else {
        s_ota_url[0] = '\0';
    }
    s_loaded = true;
    portEXIT_CRITICAL(&s_lock);
}
