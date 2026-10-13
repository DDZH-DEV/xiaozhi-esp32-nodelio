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

#ifdef __cplusplus
}
#endif
