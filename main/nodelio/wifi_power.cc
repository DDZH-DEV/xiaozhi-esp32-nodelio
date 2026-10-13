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
    /* 息屏切 Modem-sleep 模式：保持与 AP 连接与心跳，射频按 DTIM 休眠，电流仅 ~15mA */
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi modem sleep (screen off, connection maintained)");
}

void nodelio_wifi_wake(void)
{
    if (!s_asleep) {
        return;
    }
    s_asleep = false;
    /* 亮屏恢复全速高性能射频 */
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (!WifiManager::GetInstance().IsConnected()) {
        WifiManager::GetInstance().StartStation();
    }
    ESP_LOGI(TAG, "WiFi wake (performance mode)");
}

bool nodelio_wifi_is_asleep(void)
{
    return s_asleep;
}
