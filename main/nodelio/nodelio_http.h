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

/** 同 post_json，io_ms 为读写超时。技能组件回源用，避免 2.5s 内 0 字节被当成坏状态行。 */
esp_err_t nodelio_http_post_json_ms(const char *url, const char *device_key, const char *body, int *out_status,
                                    char *resp, int resp_cap, int io_ms);

esp_err_t nodelio_http_post_image(const char *url, const char *device_key, const void *data, size_t len,
                                  int *out_status);

esp_err_t nodelio_http_get(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                           char *resp, int resp_cap);

/** 同 nodelio_http_get，io_ms 为读超时（云端回源拉正文时用）。 */
esp_err_t nodelio_http_get_ms(const char *url, const char *device_key, const char *if_none_match, int *out_status,
                              char *resp, int resp_cap, int io_ms);

/** 二进制 GET。*out_n 为正文长度，缓冲内不追加 '\\0'。正文超过 resp_cap 则失败。 */
esp_err_t nodelio_http_get_bin(const char *url, const char *device_key, int *out_status, uint8_t *resp, int resp_cap,
                               size_t *out_n);

esp_err_t nodelio_http_get_bin_ms(const char *url, const char *device_key, int *out_status, uint8_t *resp, int resp_cap,
                                  size_t *out_n, int io_ms);

esp_err_t nodelio_http_delete(const char *url, const char *device_key, int *out_status, char *resp, int resp_cap);

/** 纯 TCP 探测 host:port，打日志；成功 ESP_OK。 */
esp_err_t nodelio_http_probe_tcp(const char *host, int port);

/** 局域网设备 HTTP（无 X-Device-Key；可选 X-Pass）。DIOL 等同网直控。 */
esp_err_t nodelio_http_lan_get_ms(const char *url, const char *x_pass, int *out_status, char *resp, int resp_cap,
                                  int io_ms);
esp_err_t nodelio_http_lan_post_form_ms(const char *url, const char *x_pass, const char *form_body, int *out_status,
                                        char *resp, int resp_cap, int io_ms);

#ifdef __cplusplus
}
#endif
