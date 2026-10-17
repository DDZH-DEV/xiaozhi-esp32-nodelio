#include "wifi_provision.h"

#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "nl_wifi"

static bool s_asleep;
static SemaphoreHandle_t s_net_mu;

static void ensure_net_mu(void)
{
    if (s_net_mu) {
        return;
    }
    s_net_mu = xSemaphoreCreateMutex();
}

bool nodelio_wifi_net_acquire(uint32_t timeout_ms)
{
    ensure_net_mu();
    if (!s_net_mu) {
        return false;
    }
    TickType_t ticks = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_net_mu, ticks) == pdTRUE;
}

void nodelio_wifi_net_release(void)
{
    if (s_net_mu) {
        xSemaphoreGive(s_net_mu);
    }
}

void nodelio_wifi_sleep(void)
{
    if (s_asleep) {
        return;
    }
    /* 先挡新 HTTP，再等在途请求结束，最后 wifi_stop（否则 getaddrinfo → Invalid mbox） */
    s_asleep = true;
    const bool held = nodelio_wifi_net_acquire(5000);
    if (!held) {
        ESP_LOGW(TAG, "net op busy — force wifi_stop");
    }
    /*
     * Soft sleep：只 wifi_stop，保留 WifiManager STA netif / DHCP 客户端。
     * 勿 StopStation()（毁 netif），勿 dhcpc_stop（唤醒后可续租）。
     * 清 IP 仅避免 has_ip 在射频停掉后仍为 true。
     */
    (void)esp_wifi_stop();
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t zero = {};
        (void)esp_netif_set_ip_info(netif, &zero);
    }
    if (held) {
        nodelio_wifi_net_release();
    }
    ESP_LOGI(TAG, "WiFi sleep (screen off)");
}

void nodelio_wifi_wake(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;

    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_err_t err = esp_wifi_start();
        if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
            ESP_LOGI(TAG, "WiFi wake");
            return;
        }
        ESP_LOGW(TAG, "soft wake wifi_start %s — StartStation", esp_err_to_name(err));
    }

    WifiManager::GetInstance().StartStation();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    ESP_LOGI(TAG, "WiFi wake");
}

bool nodelio_wifi_is_asleep(void)
{
    return s_asleep;
}
