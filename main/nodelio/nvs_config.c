#include "nvs_config.h"

#include <nvs.h>
#include <nvs_flash.h>
#include <stdint.h>
#include <string.h>

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
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "wifi_ssid", in->wifi_ssid);
    nvs_set_str(h, "wifi_pass", in->wifi_pass);
    nvs_set_str(h, "server_url", in->server_url);
    nvs_set_str(h, "device_key", in->device_key);
    nvs_set_str(h, "asst_ws", in->assistant_ws_url);
    nvs_set_str(h, "device_id", in->device_id);
    nvs_set_str(h, "owner", in->owner);
    nvs_set_str(h, "mqtt_server", in->mqtt_server);
    nvs_set_str(h, "mqtt_port", in->mqtt_port);
    nvs_set_str(h, "mqtt_user", in->mqtt_user);
    nvs_set_str(h, "mqtt_pass", in->mqtt_pass);
    nvs_set_str(h, "topic_prefix", in->topic_prefix);
    nvs_set_u8(h, "brightness", in->brightness);
    nvs_set_u8(h, "volume", in->volume);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
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
