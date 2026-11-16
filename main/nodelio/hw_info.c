#include "hw_info.h"

#include "sdkconfig.h"
#include "wifi_provision.h"

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

static const char *soc_name(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    return "ESP32-S3";
#elif CONFIG_IDF_TARGET_ESP32P4
    return "ESP32-P4";
#elif CONFIG_IDF_TARGET_ESP32C5
    return "ESP32-C5";
#elif CONFIG_IDF_TARGET_ESP32C6
    return "ESP32-C6";
#elif CONFIG_IDF_TARGET_ESP32C3
    return "ESP32-C3";
#else
    return CONFIG_IDF_TARGET;
#endif
}

static bool read_mac_colon(char *out, size_t out_len)
{
    if (!out || out_len < 18) {
        return false;
    }
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        /* P4 has no native Wi-Fi MAC; use stable factory base MAC. */
        err = esp_efuse_mac_get_default(mac);
    }
    if (err != ESP_OK) {
        return false;
    }
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return true;
}

cJSON *nodelio_hw_info_build(const char *module, const char *fw)
{
    cJSON *hw = cJSON_CreateObject();
    if (!hw) {
        return NULL;
    }

    const char *soc = soc_name();
    if (soc && soc[0]) {
        cJSON_AddStringToObject(hw, "soc", soc);
    }
    if (module && module[0]) {
        cJSON_AddStringToObject(hw, "module", module);
    }

    char mac[24];
    if (read_mac_colon(mac, sizeof(mac))) {
        cJSON_AddStringToObject(hw, "mac", mac);
    }

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK && flash_bytes > 0) {
        cJSON_AddNumberToObject(hw, "flash_kb", (double)(flash_bytes / 1024));
    }

    size_t ram_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    if (ram_bytes > 0) {
        cJSON_AddNumberToObject(hw, "ram_kb", (double)(ram_bytes / 1024));
    }

    size_t psram_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_bytes > 0) {
        cJSON_AddNumberToObject(hw, "psram_kb", (double)(psram_bytes / 1024));
    }

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    cJSON_AddNumberToObject(hw, "heap_free_kb", (double)(free_heap / 1024));

    int64_t uptime_us = esp_timer_get_time();
    if (uptime_us >= 0) {
        cJSON_AddNumberToObject(hw, "uptime_s", (double)(uptime_us / 1000000));
    }

    if (nodelio_wifi_is_connected()) {
        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            cJSON_AddNumberToObject(hw, "rssi", (double)ap.rssi);
        }
    }

    cJSON_AddBoolToObject(hw, "wifi", true);
    cJSON_AddBoolToObject(hw, "ble", false);

    if (fw && fw[0]) {
        cJSON_AddStringToObject(hw, "fw", fw);
    }

    return hw;
}
