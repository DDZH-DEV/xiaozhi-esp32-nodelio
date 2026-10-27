#pragma once

#include "panel_layout.h"

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * snapshot 里 lanControl 设备：后台异步同网探测 /api/state。
 * 调用前 UI 已用 sticky/lan cache；不阻塞 layout；死 IP 短超时+长退避。
 */
void nodelio_diol_probe_all_from_cache(void);

/**
 * 开机安静期：hold=true 时探测任务空转（可排队，不发 HTTP）。
 * 小智 TLS MQTT 握手与 DIOL 短 TCP 并行会砸坏 PSRAM 堆（tlsf assert）。
 * 默认 hold；云 MQTT 起来后再 nodelio_diol_boot_quiet_end()。
 */
void nodelio_diol_set_probe_hold(bool hold);

/** 停在途探测并 hold；timeout 内等 running 结束。 */
void nodelio_diol_wait_idle(uint32_t timeout_ms);

/** 解除 hold 并若已有排队则立刻探一轮。 */
void nodelio_diol_boot_quiet_end(void);

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
