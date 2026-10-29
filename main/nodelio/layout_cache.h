#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nodelio_layout_cache_mount(void);

/** 保存 GET layout 的原始 JSON（含 layoutVersion 的 API body）. */
esp_err_t nodelio_layout_cache_save(int version, const char *json, size_t len);

/**
 * 读出缓存；*out_json 由 heap 分配，调用方 free。
 * 无缓存 → ESP_ERR_NOT_FOUND。
 */
esp_err_t nodelio_layout_cache_load(char **out_json, size_t *out_len, int *out_version);

/** 删除 layout 缓存文件；无文件/未挂载视为成功。 */
esp_err_t nodelio_layout_cache_clear(void);

/**
 * 已装应用瘦身列表。优先 `/spiffs/panel_apps.json`；
 * 本板无 storage 分区时落 NVS `apps_list`。
 * *out_json 由 heap 分配，调用方 free。
 */
esp_err_t nodelio_apps_cache_save(const char *json, size_t len);
esp_err_t nodelio_apps_cache_load(char **out_json);
esp_err_t nodelio_apps_cache_clear(void);

/**
 * 首页相册 JPEG 落 SPIFFS（`/spiffs/ph_<id>.jpg`）。
 * storage 1MB：给 layout/apps 留余量，相册总预算约 700KB；单张 ≤90KB。
 * 须经 flash-safe 路径（PSRAM 任务栈不可直接 fopen）。
 */
esp_err_t nodelio_photo_cache_load(const char *photo_id, uint8_t *out, size_t cap, size_t *out_n);
esp_err_t nodelio_photo_cache_save(const char *photo_id, const uint8_t *data, size_t len);
esp_err_t nodelio_photo_cache_remove(const char *photo_id);
/** 只保留 keep 列表中的 id；其它 ph_*.jpg 删除。ids 可空、n=0 表示清空相册缓存。 */
esp_err_t nodelio_photo_cache_prune(const char *const *ids, size_t n);

#ifdef __cplusplus
}
#endif
