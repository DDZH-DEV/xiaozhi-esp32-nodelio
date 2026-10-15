#include "wifi_provision.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_wifi";

/* WiFi owned by Xiaozhi WifiBoard; NodeLio queries link + blocking scan. */

#if CONFIG_NODELIO_PANEL
static bool s_suppress_softap = true; /* 默认屏上配网，不开 SoftAP */
#else
static bool s_suppress_softap = false;
#endif
static esp_netif_t *s_scan_sta_netif;

void nodelio_wifi_set_suppress_softap(bool suppress)
{
    s_suppress_softap = suppress;
    ESP_LOGI(TAG, "suppress SoftAP = %d", (int)suppress);
}

bool nodelio_wifi_suppress_softap(void)
{
    return s_suppress_softap;
}

void nodelio_wifi_release_scan_netif(void)
{
    if (!s_scan_sta_netif) {
        return;
    }
    (void)esp_wifi_stop();
    esp_netif_destroy_default_wifi(s_scan_sta_netif);
    s_scan_sta_netif = NULL;
}

/** SoftAP/Station 停掉后，保证 STA 可扫（无 SCAN_DONE 抢结果的 handler）. */
static esp_err_t wifi_ensure_sta_for_scan(void)
{
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        s_scan_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_scan_sta_netif) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_mode STA %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err == ESP_ERR_WIFI_CONN) {
        err = ESP_OK; /* already started */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_start %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nodelio_wifi_start(nodelio_nvs_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

bool nodelio_wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool nodelio_wifi_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return false;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

/* sleep/wake/is_asleep → wifi_power.cc（WifiManager） */

bool nodelio_wifi_wait_connected(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return nodelio_wifi_is_connected();
    }
    const uint32_t step = 100;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (nodelio_wifi_is_connected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    return nodelio_wifi_is_connected();
}

bool nodelio_wifi_wait_ip(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return nodelio_wifi_has_ip();
    }
    const uint32_t step = 100;
    uint32_t waited = 0;
    /* 勿在等待中 dhcpc_stop/start：双路 wait（bridge+主任务）会互踢，打断即将完成的 DHCP */
    while (waited < timeout_ms) {
        if (nodelio_wifi_has_ip()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    bool ok = nodelio_wifi_has_ip();
    if (!ok) {
        ESP_LOGW(TAG, "wait IP timeout %ums (assoc=%d)", (unsigned)timeout_ms,
                 (int)nodelio_wifi_is_connected());
    }
    return ok;
}

esp_err_t nodelio_wifi_scan(nodelio_wifi_ap_t *out, int max_n, int *out_n)
{
    if (out_n) {
        *out_n = 0;
    }
    if (!out || max_n <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_ensure_sta_for_scan();
    if (err != ESP_OK) {
        return err;
    }

    /* 停掉他人未完成的扫描；勿在 SoftAP/Station 仍挂着 SCAN_DONE 时调用本函数 */
    (void)esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t sc = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time =
            {
                .active = {.min = 120, .max = 320},
            },
    };
    err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start %s", esp_err_to_name(err));
        return err;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        return ESP_ERR_NO_MEM;
    }
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);

    int filled = 0;
    for (uint16_t i = 0; i < got && filled < max_n; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue;
        }
        int best = -1;
        for (int k = 0; k < filled; k++) {
            if (strcmp(out[k].ssid, (char *)recs[i].ssid) == 0) {
                best = k;
                break;
            }
        }
        if (best >= 0) {
            if (recs[i].rssi > out[best].rssi) {
                out[best].rssi = recs[i].rssi;
                out[best].auth = recs[i].authmode != WIFI_AUTH_OPEN;
            }
            continue;
        }
        strncpy(out[filled].ssid, (char *)recs[i].ssid, sizeof(out[filled].ssid) - 1);
        out[filled].ssid[sizeof(out[filled].ssid) - 1] = '\0';
        out[filled].rssi = recs[i].rssi;
        out[filled].auth = recs[i].authmode != WIFI_AUTH_OPEN;
        filled++;
    }
    free(recs);

    for (int i = 0; i < filled; i++) {
        for (int j = i + 1; j < filled; j++) {
            if (out[j].rssi > out[i].rssi) {
                nodelio_wifi_ap_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    if (out_n) {
        *out_n = filled;
    }
    ESP_LOGI(TAG, "scan got %d AP", filled);
    return ESP_OK;
}
