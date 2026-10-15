#include "wifi_provision.h"

#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#define TAG "nl_wifi"

static bool s_asleep;

void nodelio_wifi_sleep(void)
{
    if (s_asleep) {
        return;
    }
    s_asleep = true;
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
