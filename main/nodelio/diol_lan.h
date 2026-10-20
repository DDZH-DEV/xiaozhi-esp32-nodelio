#pragma once

#include "panel_layout.h"

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** snapshot 里 lanControl 设备：同网探测 /api/state，写回缓存并刷磁贴。 */
void nodelio_diol_probe_all_from_cache(void);

/**
 * 若 bind 目标为局域网 DIOL：直连 HTTP 控通道，成功则乐观刷态。
 * 非 DIOL → ESP_ERR_NOT_FOUND（调用方走云 panel/command）。
 * 401 → *http_status=401，ESP_ERR_INVALID_STATE。
 */
esp_err_t nodelio_diol_try_command(const nl_bind_t *bind, const char *value_str, int *http_status);

/** 面板 NVS 存口令（不进 Server）。空串清除。 */
void nodelio_diol_pass_set(const char *device_id, const char *pass);
void nodelio_diol_pass_get(const char *device_id, char *out, int out_len);

#ifdef __cplusplus
}
#endif
