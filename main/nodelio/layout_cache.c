#include "layout_cache.h"

#include <esp_log.h>
#include <esp_spiffs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "nl_lcache";
static const char *PATH = "/spiffs/panel_layout.json";
static bool s_mounted;
static bool s_mount_failed; /* xz25 无 storage 分区，失败只报一次 */

esp_err_t nodelio_layout_cache_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (s_mount_failed) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 本板分区表只有 assets(spiffs 只读资源)，无独立 storage；勿挂 assets 以免写坏 */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        s_mount_failed = true;
        ESP_LOGW(TAG, "no layout SPIFFS (ok, use cloud): %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS total=%u used=%u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_save(int version, const char *json, size_t len)
{
    if (!json || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = nodelio_layout_cache_mount();
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open save failed");
        return ESP_FAIL;
    }
    if (fprintf(f, "v=%d\n", version) < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t n = fwrite(json, 1, len, f);
    fclose(f);
    if (n != len) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "cached layout v%d (%u bytes)", version, (unsigned)len);
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_load(char **out_json, size_t *out_len, int *out_version)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (out_len) {
        *out_len = 0;
    }
    if (out_version) {
        *out_version = 0;
    }

    esp_err_t err = nodelio_layout_cache_mount();
    if (err != ESP_OK) {
        return err;
    }
    struct stat st;
    if (stat(PATH, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    FILE *f = fopen(PATH, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    if (n < 4 || strncmp(buf, "v=", 2) != 0) {
        free(buf);
        return ESP_FAIL;
    }
    int ver = 0;
    char *nl = strchr(buf, '\n');
    if (!nl) {
        free(buf);
        return ESP_FAIL;
    }
    ver = atoi(buf + 2);
    char *body = nl + 1;
    size_t body_len = strlen(body);
    char *json = malloc(body_len + 1);
    if (!json) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, body, body_len + 1);
    free(buf);
    *out_json = json;
    if (out_len) {
        *out_len = body_len;
    }
    if (out_version) {
        *out_version = ver;
    }
    ESP_LOGI(TAG, "loaded cache v%d (%u bytes)", ver, (unsigned)body_len);
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_clear(void)
{
    esp_err_t err = nodelio_layout_cache_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear skip (mount): %s", esp_err_to_name(err));
        return ESP_OK;
    }
    if (unlink(PATH) != 0) {
        /* 无文件也算清干净 */
        ESP_LOGI(TAG, "layout cache already empty or missing");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "layout cache cleared");
    return ESP_OK;
}
