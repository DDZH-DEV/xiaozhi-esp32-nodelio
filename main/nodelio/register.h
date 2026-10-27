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

/** 云端已刷 presence（heartbeat / snapshot / bootstrap 成功）。 */
void nodelio_presence_note_touch(void);
/** 最近 within_ms 内已有云端 presence，可跳过单独 heartbeat。 */
bool nodelio_presence_recent(uint32_t within_ms);

#ifdef __cplusplus
}
#endif
