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

/** 小智正在连接/聆听/说话：布局 snapshot 应让路，避免抢 UDP 语音. */
bool nodelio_voice_session_active(void);

#ifdef __cplusplus
}
#endif
