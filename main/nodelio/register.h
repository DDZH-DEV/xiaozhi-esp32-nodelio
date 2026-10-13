#pragma once

#include "nvs_config.h"

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ensure device_id / device_key exist in cfg (persist to NVS), then
 * POST {server}/api/devices/register with X-Device-Key.
 * On success, applies mqtt{} from response into cfg (+ NVS).
 * type_code from Kconfig (WALL_PANEL); config includes board_id/display/features.
 */
esp_err_t nodelio_device_register(nodelio_nvs_config_t *cfg);

/** POST/GET heartbeat; no-op if not registered. */
esp_err_t nodelio_device_heartbeat(const nodelio_nvs_config_t *cfg);

#ifdef __cplusplus
}
#endif
