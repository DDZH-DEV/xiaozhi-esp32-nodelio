#include "nl_time.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <time.h>

#include <cstdlib>
#include <ctime>

static const char *TAG = "nl_time";

/* 固定东八区（无夏令时）。picolibc 支持 setenv("TZ") + tzset()。 */
static constexpr const char *kNlTimezone = "CST-8";

static bool s_started = false;
static bool s_sntp_started = false;

bool nodelio_time_valid(void)
{
    const time_t now = time(nullptr);
    if (now <= 0) {
        return false;
    }
    struct tm tm_now {};
    if (localtime_r(&now, &tm_now) == nullptr) {
        return false;
    }
    return tm_now.tm_year >= (2025 - 1900);
}

static void on_time_synced(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP synced, wall clock valid");
}

/** 有 IP 才起 SNTP：lwIP 的 sntp 走 raw UDP，无 netif 时发不出去。 */
static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = on_time_synced;
    cfg.start = true;
    cfg.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
}

static void on_got_ip(void *, esp_event_base_t, int32_t, void *)
{
    start_sntp();
}

esp_err_t nodelio_time_begin(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;

    /* 先设 TZ 再起 SNTP：同步回调里任何 localtime() 都应拿到本地时间。 */
    setenv("TZ", kNlTimezone, 1);
    tzset();

    esp_err_t err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, nullptr, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register GOT_IP handler: %s", esp_err_to_name(err));
    }

    /* 开机唤醒重连等场景：调用时可能已有 IP，直接起一次。 */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t ip {};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            start_sntp();
        }
    }
    return ESP_OK;
}
