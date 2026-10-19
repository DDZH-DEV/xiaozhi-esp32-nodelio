#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 墙板本地时间：设 TZ + 挂 IP_EVENT_STA_GOT_IP 后启动 SNTP。
 *
 * 以前时间来自小智 OTA CheckVersion 的 server_time（settimeofday，且把时区偏移
 * 直接加进时间戳）。墙板开机跳过 OTA 后该来源没了，改由 SNTP 取真 UTC，
 * 再用固定 TZ=CST-8 让 localtime() 出本地时间——比原先「把本地时间伪装成 UTC」
 * 更正确：媒体 expires_at 之类的 epoch 比较不再偏 8 小时。
 *
 * 幂等；须在 Application::Initialize() 早期调用（先设 TZ，再等 IP 起 SNTP）。
 */
esp_err_t nodelio_time_begin(void);

/** 是否已同步到有效墙钟（>= 2025 年）。 */
bool nodelio_time_valid(void);

#ifdef __cplusplus
}
#endif
