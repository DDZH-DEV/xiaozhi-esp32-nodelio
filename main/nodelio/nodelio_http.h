#pragma once

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 走小智 EspNetwork/HttpClient（与 OTA 同栈），勿用裸 esp_http_client。
 * device_key 可空；resp 可空。成功时 *out_status 为 HTTP 状态码。
 */
esp_err_t nodelio_http_post_json(const char *url, const char *device_key, const char *body, int *out_status,
                                 char *resp, int resp_cap);

esp_err_t nodelio_http_post_image(const char *url, const char *device_key, const void *data, size_t len,
                                  int *out_status);

esp_err_t nodelio_http_get(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                           char *resp, int resp_cap);

esp_err_t nodelio_http_delete(const char *url, const char *device_key, int *out_status, char *resp, int resp_cap);

/** 纯 TCP 探测 host:port，打日志；成功 ESP_OK。 */
esp_err_t nodelio_http_probe_tcp(const char *host, int port);

#ifdef __cplusplus
}
#endif
