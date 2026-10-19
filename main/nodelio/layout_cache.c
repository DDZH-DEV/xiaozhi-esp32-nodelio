#include "layout_cache.h"

#include <esp_log.h>
#include <esp_spiffs.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "nl_lcache";
static const char *PATH = "/spiffs/panel_layout.json";
static const char *APPS_PATH = "/spiffs/panel_apps.json";
static const char *APPS_NVS_NS = "nodelio";
static const char *APPS_NVS_KEY = "apps_list";
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

static esp_err_t apps_nvs_save(const char *json, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APPS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, APPS_NVS_KEY, json, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t apps_nvs_load(char **out_json)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APPS_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = 0;
    err = nvs_get_blob(h, APPS_NVS_KEY, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    char *buf = malloc(len + 1);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(h, APPS_NVS_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[len] = '\0';
    *out_json = buf;
    return ESP_OK;
}

static esp_err_t apps_nvs_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(APPS_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    err = nvs_erase_key(h, APPS_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t nodelio_apps_cache_save(const char *json, size_t len)
{
    if (!json || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nodelio_layout_cache_mount() == ESP_OK) {
        FILE *f = fopen(APPS_PATH, "wb");
        if (f) {
            size_t n = fwrite(json, 1, len, f);
            fclose(f);
            if (n == len) {
                ESP_LOGI(TAG, "cached apps file (%u bytes)", (unsigned)len);
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "apps file save failed, try NVS");
    }
    esp_err_t err = apps_nvs_save(json, len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cached apps nvs (%u bytes)", (unsigned)len);
    } else {
        ESP_LOGW(TAG, "apps cache save: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nodelio_apps_cache_load(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (nodelio_layout_cache_mount() == ESP_OK) {
        struct stat st;
        if (stat(APPS_PATH, &st) == 0 && st.st_size > 0) {
            FILE *f = fopen(APPS_PATH, "rb");
            if (f) {
                char *buf = malloc((size_t)st.st_size + 1);
                if (!buf) {
                    fclose(f);
                    return ESP_ERR_NO_MEM;
                }
                size_t n = fread(buf, 1, (size_t)st.st_size, f);
                fclose(f);
                buf[n] = '\0';
                *out_json = buf;
                ESP_LOGI(TAG, "loaded apps file (%u bytes)", (unsigned)n);
                return ESP_OK;
            }
        }
    }
    esp_err_t err = apps_nvs_load(out_json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded apps nvs");
    }
    return err;
}

esp_err_t nodelio_apps_cache_clear(void)
{
    if (nodelio_layout_cache_mount() == ESP_OK) {
        (void)unlink(APPS_PATH);
    }
    return apps_nvs_clear();
}
