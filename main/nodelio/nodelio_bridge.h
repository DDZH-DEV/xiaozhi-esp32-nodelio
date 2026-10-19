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

/**
 * 等局域网 bridge 完成一轮（注册/布局/面板 MQTT 尝试结束）。
 * 开机让小智云端 MQTT 让路用；超时仍返回，不阻塞死。
 */
bool nodelio_bridge_wait_ready(uint32_t timeout_ms);

/** 小智正在连接/聆听/说话：布局 snapshot 应让路，避免抢 UDP 语音. */
bool nodelio_voice_session_active(void);

#ifdef __cplusplus
}
#endif
