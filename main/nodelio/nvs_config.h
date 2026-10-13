#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODELIO_NVS_NS "nodelio"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[128];
    char device_key[65];
    char assistant_ws_url[128];
    char device_id[40];
    char owner[64];
    char mqtt_server[40];
    char mqtt_port[8];
    char mqtt_user[64];
    char mqtt_pass[40];
    char topic_prefix[40];
    uint8_t brightness; /* 1–100；0=未设置用板级默认 */
    uint8_t volume;     /* 0–100；助手音频启用后生效 */
} nodelio_nvs_config_t;

esp_err_t nodelio_nvs_init(void);
esp_err_t nodelio_nvs_load(nodelio_nvs_config_t *out);
esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t *in);
esp_err_t nodelio_nvs_clear(void);
/** Drop broker credentials (keep device_id/key); call when server_url changes. */
void nodelio_nvs_clear_mqtt(nodelio_nvs_config_t *cfg);
bool nodelio_nvs_has_wifi(const nodelio_nvs_config_t *cfg);

#ifdef __cplusplus
}
#endif
