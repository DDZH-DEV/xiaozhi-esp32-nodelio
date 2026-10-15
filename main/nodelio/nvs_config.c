#include "nvs_config.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <stdint.h>
#include <string.h>

/* LVGL 任务栈在 PSRAM：禁 cache 读 flash 时不可用 → 回调内禁止 nvs_load/save */
static nodelio_nvs_config_t s_ram;
static bool s_ram_ok;

static void ram_put(const nodelio_nvs_config_t *in)
{
    if (!in) {
        return;
    }
    s_ram = *in;
    s_ram_ok = true;
}

void nodelio_nvs_cache_put(const nodelio_nvs_config_t *in)
{
    ram_put(in);
}

void nodelio_nvs_cache_get(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    if (s_ram_ok) {
        *out = s_ram;
        return;
    }
    memset(out, 0, sizeof(*out));
    out->xiaozhi_en = 1;
    out->power_save_en = 1;
    out->idle_sleep_s = NODELIO_IDLE_SLEEP_DEFAULT;
    out->idle_wifi_s = NODELIO_IDLE_WIFI_DEFAULT;
    out->goodbye_sleep_s = NODELIO_BYE_SLEEP_DEFAULT;
}

static esp_err_t get_str(nvs_handle_t h, const char *key, char *buf, size_t len)
{
    size_t required = len;
    esp_err_t err = nvs_get_str(h, key, buf, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (len) {
            buf[0] = '\0';
        }
        return ESP_OK;
    }
    return err;
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi, uint16_t def)
{
    if (v == 0) {
        return def;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void apply_defaults(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    if (out->xiaozhi_en > 1) {
        out->xiaozhi_en = 1;
    }
    if (out->power_save_en > 1) {
        out->power_save_en = 1;
    }
    out->idle_sleep_s =
        clamp_u16(out->idle_sleep_s, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX,
                  NODELIO_IDLE_SLEEP_DEFAULT);
    out->idle_wifi_s =
        clamp_u16(out->idle_wifi_s, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX,
                  out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT);
    out->goodbye_sleep_s =
        clamp_u16(out->goodbye_sleep_s, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX,
                  NODELIO_BYE_SLEEP_DEFAULT);
}

/* 避免 save 路径把「关闭小智/省电」(0) 误当成未初始化 */
static void apply_defaults_for_save(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    out->idle_sleep_s =
        clamp_u16(out->idle_sleep_s, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX,
                  NODELIO_IDLE_SLEEP_DEFAULT);
    out->idle_wifi_s =
        clamp_u16(out->idle_wifi_s, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX,
                  out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT);
    out->goodbye_sleep_s =
        clamp_u16(out->goodbye_sleep_s, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX,
                  NODELIO_BYE_SLEEP_DEFAULT);
    if (out->xiaozhi_en > 1) {
        out->xiaozhi_en = 1;
    }
    if (out->power_save_en > 1) {
        out->power_save_en = 1;
    }
}

esp_err_t nodelio_nvs_init(void)
{
    /* Xiaozhi already initialized NVS; do not erase on version mismatch. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }
    return (err == ESP_OK || err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

esp_err_t nodelio_nvs_load(nodelio_nvs_config_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->xiaozhi_en = 1;
    out->power_save_en = 1;
    out->idle_sleep_s = NODELIO_IDLE_SLEEP_DEFAULT;
    out->idle_wifi_s = NODELIO_IDLE_WIFI_DEFAULT;
    out->goodbye_sleep_s = NODELIO_BYE_SLEEP_DEFAULT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    get_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    get_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    get_str(h, "server_url", out->server_url, sizeof(out->server_url));
    get_str(h, "device_key", out->device_key, sizeof(out->device_key));
    get_str(h, "asst_ws", out->assistant_ws_url, sizeof(out->assistant_ws_url));
    get_str(h, "device_id", out->device_id, sizeof(out->device_id));
    get_str(h, "owner", out->owner, sizeof(out->owner));
    get_str(h, "mqtt_server", out->mqtt_server, sizeof(out->mqtt_server));
    get_str(h, "mqtt_port", out->mqtt_port, sizeof(out->mqtt_port));
    get_str(h, "mqtt_user", out->mqtt_user, sizeof(out->mqtt_user));
    get_str(h, "mqtt_pass", out->mqtt_pass, sizeof(out->mqtt_pass));
    get_str(h, "topic_prefix", out->topic_prefix, sizeof(out->topic_prefix));
    {
        uint8_t v = 0;
        if (nvs_get_u8(h, "brightness", &v) == ESP_OK) {
            out->brightness = v;
        }
        v = 0;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK) {
            out->volume = v;
        }
        v = 1;
        if (nvs_get_u8(h, "xz_en", &v) == ESP_OK) {
            out->xiaozhi_en = v ? 1 : 0;
        } else {
            out->xiaozhi_en = 1;
        }
        v = 1;
        if (nvs_get_u8(h, "ps_en", &v) == ESP_OK) {
            out->power_save_en = v ? 1 : 0;
        } else {
            out->power_save_en = 1;
        }
        uint16_t u16 = 0;
        if (nvs_get_u16(h, "idle_slp", &u16) == ESP_OK) {
            out->idle_sleep_s = u16;
        }
        u16 = 0;
        if (nvs_get_u16(h, "wifi_slp", &u16) == ESP_OK) {
            out->idle_wifi_s = u16;
        } else {
            /* 旧固件无此键：与息屏同值，行为与原先「息屏即断网」一致 */
            out->idle_wifi_s = out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT;
        }
        u16 = 0;
        if (nvs_get_u16(h, "bye_slp", &u16) == ESP_OK) {
            out->goodbye_sleep_s = u16;
        }
    }
    nvs_close(h);
    apply_defaults(out);
    ram_put(out);
    return ESP_OK;
}

esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    nodelio_nvs_config_t tmp = *in;
    apply_defaults_for_save(&tmp);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", tmp.wifi_ssid);
    nvs_set_str(h, "wifi_pass", tmp.wifi_pass);
    nvs_set_str(h, "server_url", tmp.server_url);
    nvs_set_str(h, "device_key", tmp.device_key);
    nvs_set_str(h, "asst_ws", tmp.assistant_ws_url);
    nvs_set_str(h, "device_id", tmp.device_id);
    nvs_set_str(h, "owner", tmp.owner);
    nvs_set_str(h, "mqtt_server", tmp.mqtt_server);
    nvs_set_str(h, "mqtt_port", tmp.mqtt_port);
    nvs_set_str(h, "mqtt_user", tmp.mqtt_user);
    nvs_set_str(h, "mqtt_pass", tmp.mqtt_pass);
    nvs_set_str(h, "topic_prefix", tmp.topic_prefix);
    nvs_set_u8(h, "brightness", tmp.brightness);
    nvs_set_u8(h, "volume", tmp.volume);
    nvs_set_u8(h, "xz_en", tmp.xiaozhi_en ? 1 : 0);
    nvs_set_u8(h, "ps_en", tmp.power_save_en ? 1 : 0);
    nvs_set_u16(h, "idle_slp", tmp.idle_sleep_s);
    nvs_set_u16(h, "wifi_slp", tmp.idle_wifi_s);
    nvs_set_u16(h, "bye_slp", tmp.goodbye_sleep_s);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ram_put(&tmp);
    }
    return err;
}

bool nodelio_xiaozhi_enabled(void)
{
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.xiaozhi_en != 0;
}

esp_err_t nodelio_xiaozhi_set_enabled(bool on)
{
    nodelio_nvs_config_t cfg;
    if (s_ram_ok) {
        cfg = s_ram;
    } else {
        esp_err_t err = nodelio_nvs_load(&cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    cfg.xiaozhi_en = on ? 1 : 0;
    ram_put(&cfg);
    return nodelio_nvs_save(&cfg);
}

bool nodelio_power_save_enabled(void)
{
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.power_save_en != 0;
}

esp_err_t nodelio_power_save_set_enabled(bool on)
{
    nodelio_nvs_config_t cfg;
    if (s_ram_ok) {
        cfg = s_ram;
    } else {
        esp_err_t err = nodelio_nvs_load(&cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    cfg.power_save_en = on ? 1 : 0;
    ram_put(&cfg);
    return nodelio_nvs_save(&cfg);
}

uint16_t nodelio_idle_sleep_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_IDLE_SLEEP_DEFAULT;
    }
    return s_ram.idle_sleep_s;
}

uint16_t nodelio_idle_wifi_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_IDLE_WIFI_DEFAULT;
    }
    return s_ram.idle_wifi_s;
}

uint16_t nodelio_goodbye_sleep_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_BYE_SLEEP_DEFAULT;
    }
    return s_ram.goodbye_sleep_s;
}

esp_err_t nodelio_set_sleep_seconds(uint16_t idle_s, uint16_t goodbye_s)
{
    nodelio_nvs_config_t cfg;
    if (s_ram_ok) {
        cfg = s_ram;
    } else {
        esp_err_t err = nodelio_nvs_load(&cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (idle_s != 0) {
        cfg.idle_sleep_s = idle_s;
    }
    if (goodbye_s != 0) {
        cfg.goodbye_sleep_s = goodbye_s;
    }
    apply_defaults_for_save(&cfg);
    ram_put(&cfg);
    return nodelio_nvs_save(&cfg);
}

esp_err_t nodelio_set_idle_wifi_seconds(uint16_t wifi_s)
{
    nodelio_nvs_config_t cfg;
    if (s_ram_ok) {
        cfg = s_ram;
    } else {
        esp_err_t err = nodelio_nvs_load(&cfg);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (wifi_s != 0) {
        cfg.idle_wifi_s = wifi_s;
    }
    apply_defaults_for_save(&cfg);
    ram_put(&cfg);
    return nodelio_nvs_save(&cfg);
}

esp_err_t nodelio_nvs_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void nodelio_nvs_clear_mqtt(nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->mqtt_server[0] = '\0';
    cfg->mqtt_port[0] = '\0';
    cfg->mqtt_user[0] = '\0';
    cfg->mqtt_pass[0] = '\0';
    cfg->topic_prefix[0] = '\0';
}

bool nodelio_nvs_has_wifi(const nodelio_nvs_config_t *cfg)
{
    return cfg && cfg->wifi_ssid[0] != '\0';
}
