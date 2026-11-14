#pragma once

#include "content_envelope.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/**
 * 重新扫描卡内 jpg/jpeg（与音频列表分开存放）。
 * 须在面板 Job 里调用。成功返回 ESP_OK。
 */
esp_err_t nodelio_sd_rescan_photos(void);

int nodelio_sd_photo_count(void);
int nodelio_sd_photo_batch(void);
void nodelio_sd_photo_set_batch(int batch);
int nodelio_sd_photo_batch_count(void);
void nodelio_sd_photo_next_batch(void);

/** 当前批次填进 list 信封（id=photo 下标，title=文件名）。返回条数。 */
int nodelio_sd_fill_photo_envelope(nl_content_envelope_t *env, int *total_out);

/** 按下标取 VFS 路径；越界返回 false。 */
bool nodelio_sd_photo_path(int index, char *path, size_t path_len);

/** 在已扫描的照片表里找绝对路径；未命中返回 -1。 */
int nodelio_sd_photo_index_of(const char *abs_path);

typedef enum {
    NODELIO_SD_ENTRY_DIR = 0,
    NODELIO_SD_ENTRY_AUDIO,
    NODELIO_SD_ENTRY_PHOTO,
    NODELIO_SD_ENTRY_OTHER,
} nodelio_sd_entry_kind_t;

#define NODELIO_SD_ENTRY_NAME_MAX 96
#define NODELIO_SD_ENTRY_PATH_MAX 160
#define NODELIO_SD_LIST_MAX 200

typedef struct nodelio_sd_entry {
    nodelio_sd_entry_kind_t kind;
    char name[NODELIO_SD_ENTRY_NAME_MAX];
    char path[NODELIO_SD_ENTRY_PATH_MAX];
    /** 字节；目录为 0；stat 失败为 0。 */
    uint64_t size_bytes;
} nodelio_sd_entry_t;

/**
 * 列举单层目录（Job 线程调用）。dir_path 空则根 `/sdcard`。
 * 目录优先、再按名排序；跳过 . / ..；路径必须落在挂载点下。
 */
esp_err_t nodelio_sd_list_dir(const char *dir_path, nodelio_sd_entry_t *out, int max, int *out_n);

/** 规范化到挂载点下的绝对路径；失败返回 false。 */
bool nodelio_sd_normalize_path(const char *in, char *out, size_t out_len);

/** 相对挂载点的目录深度（根=0）。非法路径返回 -1。 */
int nodelio_sd_path_depth(const char *abs_path);

/** 挂载点字符串（"/sdcard"）。 */
const char *nodelio_sd_mount_point(void);

#ifdef __cplusplus
}
#endif
