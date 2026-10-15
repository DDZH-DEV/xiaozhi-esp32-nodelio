#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 去尾 '/'，并剥掉尾部 `/api/devices/register`（若有）。 */
void nl_url_strip_base(const char *base, char *out, size_t out_len);

/**
 * 从完整 URL 解析 host/port/https。
 * port 默认 "8899"；url 空则 host 空、https=false。
 */
void nl_url_parse_server(const char *url, char *host, size_t host_len, char *port, size_t port_len,
                         bool *https);

#ifdef __cplusplus
}
#endif
