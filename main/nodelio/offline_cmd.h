#pragma once

#include "panel_layout.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nodelio_offline_cmd_done_cb_t)(void);

/** 注册结束回调（主任务 Schedule 回 idle）；可传 NULL。 */
void nodelio_offline_cmd_set_done_cb(nodelio_offline_cmd_done_cb_t cb);

/**
 * 空闲时预创建 MultiNet（避免唤醒瞬间与 Opus 编码抢内部 DRAM 崩 FST）。
 * 已就绪则立即返回 ESP_OK。
 */
esp_err_t nodelio_offline_cmd_prewarm(void);

/**
 * 小智建链失败后进入本地口令窗（约 12s）。
 * 成功则需 EnableLocalCommandMode(true)，并由 AFE fetch 调用 feed。
 * 无 MultiNet / 已在听 → ESP_ERR_*。
 */
esp_err_t nodelio_offline_cmd_begin_listen(void);

/** 结束本地口令（超时/识别完/取消）；幂等。 */
void nodelio_offline_cmd_end_listen(void);

bool nodelio_offline_cmd_listening(void);

/** AFE fetch 单声道 PCM（16k）；仅 listening 时处理。 */
void nodelio_offline_cmd_feed(const int16_t *data, int samples);

/** 播歌前：结束口令窗并恢复唤醒词。不释放 MultiNet，连续内部块留给播歌后的再次唤醒。 */
void nodelio_offline_cmd_stop(void);

/** 当前 DRAM 占位代号。播歌任务开始时记下，结束时带回来，避免和下一首抢。 */
int nodelio_offline_cmd_dram_epoch(void);

/** 播歌任务真正开流前：放开 stop() 占住的 DRAM，给解码器 / TLS。没有占位则返回 false。 */
bool nodelio_offline_cmd_yield_slot(void);

/**
 * 云端聆听/UDP 开流前：放开 slot + shield，给 lwIP pbuf / AES-CTR。
 * 不拆 MultiNet 实例（下次本地口令仍可复用）。
 */
void nodelio_offline_cmd_yield_for_cloud_audio(void);

/**
 * 解码器已打开后调用：把剩下的大块内部 RAM 占住，避免下载循环里的小分配把它切碎。
 * MultiNet 仍在时不做任何事。
 */
void nodelio_offline_cmd_shield_up(void);

/**
 * HTTP 与解码器都已关闭后调用（可在 PSRAM 栈上）。
 * 放开 shield，并尽量把 ≥48KB 的内部块重新占住。不创建模型。
 */
void nodelio_offline_cmd_finish_playback_dram(int epoch);

/** 主任务：按 epoch 把占位还给 MultiNet。代号已变（下一首已 stop）则跳过。 */
void nodelio_offline_cmd_restore_after_media(int epoch);

#define NL_DYN_CMD_BASE 100

/** 重置动态离线口令缓冲区 */
void nodelio_offline_cmd_reset_dynamic(void);

/** 动态添加一条离线口令（如 "da kai wo shi deng" -> 设备+通道/引脚+动作） */
bool nodelio_offline_cmd_add_dynamic(const char *device_id, int channel_or_pin, bool is_pin,
                                    const char *action, const char *name, const char *pinyin);

/** 提交并激活所有动态口令到 MultiNet 引擎 */
void nodelio_offline_cmd_commit_dynamic(void);

/** 按布局磁贴 title 的 pinyinOn/Off 重建词表（屏上名） */
void nodelio_offline_cmd_rebuild_from_layout(const nodelio_panel_layout_t *layout);

/** 布局动态口令条数（设置页一览）。 */
int nodelio_offline_cmd_dynamic_count(void);

/** 取第 i 条动态口令：屏上名 + ON/OFF。 */
bool nodelio_offline_cmd_dynamic_at(int i, char *name, size_t name_len, char *action,
                                   size_t action_len);

#ifdef __cplusplus
}
#endif
