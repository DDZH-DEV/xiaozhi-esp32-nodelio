#include "wifi_provision.h"

#include "wifi_manager.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "nl_wifi"

void nodelio_log_heap(const char *where)
{
    const char *w = where && where[0] ? where : "?";
    ESP_LOGI(TAG,
             "heap[%s] dram free=%u largest=%u | psram free=%u largest=%u",
             w, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

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
    /* 先挡新 HTTP，再等在途请求结束，最后停射频（否则 getaddrinfo → Invalid mbox） */
    s_asleep = true;
    const bool held = nodelio_wifi_net_acquire(5000);
    if (!held) {
        ESP_LOGW(TAG, "net op busy — force wifi soft-stop");
    }
    /*
     * Soft sleep：先卸 WifiStation 事件再 wifi_stop，保留 STA netif。
     * 禁止裸 esp_wifi_stop：STA_DISCONNECTED 会 esp_wifi_connect 与 stop 竞态崩溃。
     * 勿 StopStation()（毁 netif），勿 dhcpc_stop（唤醒后可续租）。
     */
    WifiManager::GetInstance().SoftStopStationRadio();
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

    /* 先起射频再 set_ps：wifi_stop 后 ESP_ERROR_CHECK(set_ps) 会 abort */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        WifiManager::GetInstance().SoftStartStationRadio();
    } else {
        ESP_LOGW(TAG, "soft wake no netif — StartStation");
        WifiManager::GetInstance().StartStation();
    }
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi wake");
}

bool nodelio_wifi_is_asleep(void)
{
    return s_asleep;
}
