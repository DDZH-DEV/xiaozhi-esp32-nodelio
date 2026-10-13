#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nodelio_offline_cmd_done_cb_t)(void);

/** 注册结束回调（主任务 Schedule 回 idle）；可传 NULL。 */
void nodelio_offline_cmd_set_done_cb(nodelio_offline_cmd_done_cb_t cb);

/**
 * 小智建链失败后进入本地口令窗（约 5s）。
 * 成功则需 EnableLocalCommandMode(true)，并由 AFE fetch 调用 feed。
 * 无 MultiNet / 已在听 → ESP_ERR_*。
 */
esp_err_t nodelio_offline_cmd_begin_listen(void);

/** 结束本地口令（超时/识别完/取消）；幂等。 */
void nodelio_offline_cmd_end_listen(void);

bool nodelio_offline_cmd_listening(void);

/** AFE fetch 单声道 PCM（16k）；仅 listening 时处理。 */
void nodelio_offline_cmd_feed(const int16_t *data, int samples);

/** TLS/OTA 前：停听并尽量释放 MultiNet（不碰小智 AFE）。 */
void nodelio_offline_cmd_stop(void);

#define NL_DYN_CMD_BASE 100

/** 重置动态离线口令缓冲区 */
void nodelio_offline_cmd_reset_dynamic(void);

/** 动态添加一条离线口令（如 "da kai wo shi deng" -> 设备+通道+动作） */
bool nodelio_offline_cmd_add_dynamic(const char *device_id, int channel, const char *action,
                                    const char *name, const char *pinyin);

/** 提交并激活所有动态口令到 MultiNet 引擎 */
void nodelio_offline_cmd_commit_dynamic(void);

#ifdef __cplusplus
}
#endif
