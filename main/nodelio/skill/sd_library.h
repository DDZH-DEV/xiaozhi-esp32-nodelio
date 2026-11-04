#pragma once

#include "content_envelope.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 本板是否声明了 SD/TF 接线（Board::GetSdCardPins）。 */
bool nodelio_sd_available(void);
/**
 * 挂载内存卡（幂等）。首次调用会初始化 SDMMC host + FATFS。
 * 失败返回错误但不 abort：未插卡/接触不良属正常状态。
 */
esp_err_t nodelio_sd_mount(void);

bool nodelio_sd_is_mounted(void);

/**
 * 重新扫描卡内音频文件（阻塞，须在面板 Job 里调用，勿在 LVGL 回调里跑）。
 * 成功返回 ESP_OK；未挂载返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t nodelio_sd_rescan(void);

/** 最近一次扫描到的音频文件总数。 */
int nodelio_sd_track_count(void);

/** 卡内曲目按文件名稳定排序后分批；每批最多 NL_ENV_TRACK_MAX 首。 */
int nodelio_sd_batch(void);
void nodelio_sd_set_batch(int batch);
int nodelio_sd_batch_count(void);

/**
 * 把当前批次填进 music_list 信封（调用方负责分配 env）。
 * total_out 可空，写入曲目总数。返回写入的曲目数（0 = 无曲目）。
 */
int nodelio_sd_fill_envelope(nl_content_envelope_t *env, int *total_out);

/**
 * playback_url 形如 sd:///sdcard/Music/a.mp3 → 还原为 VFS 路径。
 * 命中返回 true。非 sd:// 直接返回 false。
 */
bool nodelio_sd_resolve_url(const char *url, char *path, size_t path_len);

/** 把 VFS 路径转成 sd:// URL（用于 track.playback_url）。 */
void nodelio_sd_make_url(const char *path, char *url, size_t url_len);

/** 曲库批次自增（「换一批」）；无曲目时保持 0。 */
void nodelio_sd_next_batch(void);

#ifdef __cplusplus
}
#endif
