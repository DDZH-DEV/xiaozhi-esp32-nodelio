#include "wifi_provision.h"

#include "wifi_manager.h"

#include <esp_log.h>
#include <esp_wifi.h>

#define TAG "nl_wifi"

static bool s_asleep;

void nodelio_wifi_sleep(void)
{
    if (s_asleep) {
        return;
    }
    s_asleep = true;
    /* 小智 WifiManager 停 STA，避免息屏期重连/收发 */
    WifiManager::GetInstance().StopStation();
    ESP_LOGI(TAG, "WiFi sleep (screen off)");
}

void nodelio_wifi_wake(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    WifiManager::GetInstance().StartStation();
    ESP_LOGI(TAG, "WiFi wake");
}

bool nodelio_wifi_is_asleep(void)
{
    return s_asleep;
}
