#pragma once

#include "nvs_config.h"

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 订阅 {topic_prefix}/{device_id}/panel/notify；
 * 无 mqtt_server 时跳过。回调内只置位，由 layout 任务拉 HTTP。
 */
esp_err_t nodelio_mqtt_panel_start(const nodelio_nvs_config_t *cfg);

/** 息屏断网前停 MQTT（可再 start）. */
void nodelio_mqtt_panel_stop(void);

/** 面板 MQTT 是否已连上（状态栏云图标）. */
bool nodelio_mqtt_panel_connected(void);

#ifdef __cplusplus
}
#endif
