#pragma once

#include "nvs_config.h"

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 WiFi 并尽量连 NVS 凭据。
 * 无凭据 / 超时：进 SoftAP 门户（WiFi + 服务器/绑定码）；屏上「连接」页仍可用。
 */
esp_err_t nodelio_wifi_start(nodelio_nvs_config_t *cfg);

/** STA 已关联（L2），不保证已有 IP. */
bool nodelio_wifi_is_connected(void);
bool nodelio_wifi_has_ip(void);

/** 息屏超省电：卸 Station 事件后 soft-stop 射频（禁止断网期自动重连；AFE 本地唤醒仍可用）. */
void nodelio_wifi_sleep(void);

/** 亮屏后恢复 STA；非阻塞启动，可用 wait_connected 等待. */
void nodelio_wifi_wake(void);

bool nodelio_wifi_is_asleep(void);

/**
 * HTTP / DNS 与 wifi_stop 互斥：持锁期间禁止 sleep 停射频。
 * timeout_ms=0 立即尝试；拿不到返回 false。
 */
bool nodelio_wifi_net_acquire(uint32_t timeout_ms);
void nodelio_wifi_net_release(void);

/** 打印片内 DRAM + PSRAM 空闲量与最大连续块（诊断「8MB PSRAM 够不够」用）。 */
void nodelio_log_heap(const char *where);

/** 等待 L2 关联；timeout_ms=0 立即返回当前态. */
bool nodelio_wifi_wait_connected(uint32_t timeout_ms);
/** 等待 DHCP；HTTP 前用. */
bool nodelio_wifi_wait_ip(uint32_t timeout_ms);

#define NL_WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool auth; /* true=需密码 */
} nodelio_wifi_ap_t;

/**
 * 屏上扫描前可临时 suppress，避免 SoftAP SCAN_DONE 抢结果。
 * 默认 false（允许 SoftAP 门户）。
 */
void nodelio_wifi_set_suppress_softap(bool suppress);
bool nodelio_wifi_suppress_softap(void);

/** 扫完若要 StartStation：先释放扫描临时 STA netif，避免与 Station 抢 WIFI_STA_DEF. */
void nodelio_wifi_release_scan_netif(void);

/** 阻塞扫描（数秒）；同 SSID 留最强。调用勿在 LVGL 线程。 */
esp_err_t nodelio_wifi_scan(nodelio_wifi_ap_t *out, int max_n, int *out_n);

#ifdef __cplusplus
}
#endif
