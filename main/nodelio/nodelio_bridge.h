#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** After Xiaozhi WiFi + OTA/activation: register, MQTT notify, layout poll. */
esp_err_t nodelio_bridge_start(void);

/** Re-run register/layout after LVGL settings save (no reboot required). */
esp_err_t nodelio_bridge_restart(void);

void nodelio_bridge_stop(void);

#ifdef __cplusplus
}
#endif
