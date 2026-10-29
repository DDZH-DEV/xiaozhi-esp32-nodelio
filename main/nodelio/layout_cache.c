#include "layout_cache.h"

#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_spiffs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <ctype.h>
#include <dirent.h>
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
static bool s_mount_failed;
static SemaphoreHandle_t s_flash_mu;

/* SPIFFS/NVS 关 Flash cache 时栈必须在内部 SRAM。
 * nl_layout 等任务栈在 PSRAM，直接 fopen 会 assert esp_task_stack_is_sane_cache_disabled。 */
#define CACHE_FLASH_STACK_BYTES 3072

typedef esp_err_t (*cache_flash_fn)(void *ctx);

typedef struct {
    cache_flash_fn fn;
    void *ctx;
    esp_err_t err;
    SemaphoreHandle_t done;
} cache_flash_job_t;

static void cache_flash_lock(void)
{
    if (!s_flash_mu) {
        s_flash_mu = xSemaphoreCreateMutex();
    }
    if (s_flash_mu) {
        xSemaphoreTake(s_flash_mu, portMAX_DELAY);
    }
}

static void cache_flash_unlock(void)
{
    if (s_flash_mu) {
        xSemaphoreGive(s_flash_mu);
    }
}

/* 常驻内部 DRAM 栈：SRAM 碎片化时短命 xTaskCreate 常失败，相册就反复 HTTP */
static StackType_t s_flash_stack[CACHE_FLASH_STACK_BYTES / sizeof(StackType_t)] __attribute__((aligned(8)));
static StaticTask_t s_flash_tcb;
static TaskHandle_t s_flash_worker;
static SemaphoreHandle_t s_flash_req;
static SemaphoreHandle_t s_flash_ack;
static cache_flash_fn s_flash_fn;
static void *s_flash_ctx;
static esp_err_t s_flash_err;

static void cache_flash_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_flash_req, portMAX_DELAY);
        s_flash_err = s_flash_fn ? s_flash_fn(s_flash_ctx) : ESP_FAIL;
        xSemaphoreGive(s_flash_ack);
    }
}

static bool cache_flash_worker_ensure(void)
{
    if (s_flash_worker) {
        return true;
    }
    if (!s_flash_req) {
        s_flash_req = xSemaphoreCreateBinary();
    }
    if (!s_flash_ack) {
        s_flash_ack = xSemaphoreCreateBinary();
    }
    if (!s_flash_req || !s_flash_ack) {
        return false;
    }
    s_flash_worker = xTaskCreateStatic(cache_flash_worker, "nl_spiffs",
                                       (uint32_t)(sizeof(s_flash_stack) / sizeof(StackType_t)), NULL, 5,
                                       s_flash_stack, &s_flash_tcb);
    return s_flash_worker != NULL;
}

/** 当前栈在 PSRAM 时，把 Flash 读写丢到常驻内部栈任务。 */
static esp_err_t cache_run_flash_safe(cache_flash_fn fn, void *ctx)
{
    int probe = 0;
    if (!esp_ptr_external_ram(&probe)) {
        return fn(ctx);
    }
    if (!cache_flash_worker_ensure()) {
        ESP_LOGW(TAG, "flash worker unavailable");
        return ESP_ERR_NO_MEM;
    }
    s_flash_fn = fn;
    s_flash_ctx = ctx;
    s_flash_err = ESP_FAIL;
    xSemaphoreGive(s_flash_req);
    xSemaphoreTake(s_flash_ack, portMAX_DELAY);
    return s_flash_err;
}

static esp_err_t mount_impl(void *ctx)
{
    (void)ctx;
    if (s_mounted) {
        return ESP_OK;
    }
    if (s_mount_failed) {
        return ESP_ERR_NOT_FOUND;
    }
    /* storage：可写 SPIFFS（layout/apps）；勿挂 assets（只读资源包） */
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true, /* 新分区 / 擦片后首次挂载自动格式化 */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        s_mount_failed = true;
        ESP_LOGW(TAG, "layout SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS total=%u used=%u", (unsigned)total, (unsigned)used);
    }
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (s_mount_failed) {
        return ESP_ERR_NOT_FOUND;
    }
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(mount_impl, NULL);
    cache_flash_unlock();
    return err;
}

typedef struct {
    int version;
    const char *json;
    size_t len;
} save_ctx_t;

static esp_err_t save_impl(void *arg)
{
    save_ctx_t *c = (save_ctx_t *)arg;
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        return err;
    }
    FILE *f = fopen(PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "open save failed");
        return ESP_FAIL;
    }
    if (fprintf(f, "v=%d\n", c->version) < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    size_t n = fwrite(c->json, 1, c->len, f);
    fclose(f);
    if (n != c->len) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "cached layout v%d (%u bytes)", c->version, (unsigned)c->len);
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_save(int version, const char *json, size_t len)
{
    if (!json || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    save_ctx_t ctx = {.version = version, .json = json, .len = len};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(save_impl, &ctx);
    cache_flash_unlock();
    return err;
}

typedef struct {
    char **out_json;
    size_t *out_len;
    int *out_version;
} load_ctx_t;

static esp_err_t load_impl(void *arg)
{
    load_ctx_t *c = (load_ctx_t *)arg;
    esp_err_t err = mount_impl(NULL);
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
    char *nl = strchr(buf, '\n');
    if (!nl) {
        free(buf);
        return ESP_FAIL;
    }
    int ver = atoi(buf + 2);
    char *body = nl + 1;
    size_t body_len = strlen(body);
    char *json = malloc(body_len + 1);
    if (!json) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(json, body, body_len + 1);
    free(buf);
    *c->out_json = json;
    if (c->out_len) {
        *c->out_len = body_len;
    }
    if (c->out_version) {
        *c->out_version = ver;
    }
    ESP_LOGI(TAG, "loaded cache v%d (%u bytes)", ver, (unsigned)body_len);
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
    load_ctx_t ctx = {.out_json = out_json, .out_len = out_len, .out_version = out_version};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(load_impl, &ctx);
    cache_flash_unlock();
    return err;
}

static esp_err_t clear_impl(void *arg)
{
    (void)arg;
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear skip (mount): %s", esp_err_to_name(err));
        return ESP_OK;
    }
    if (unlink(PATH) != 0) {
        ESP_LOGI(TAG, "layout cache already empty or missing");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "layout cache cleared");
    return ESP_OK;
}

esp_err_t nodelio_layout_cache_clear(void)
{
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(clear_impl, NULL);
    cache_flash_unlock();
    return err;
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

typedef struct {
    const char *json;
    size_t len;
} apps_save_ctx_t;

static esp_err_t apps_save_impl(void *arg)
{
    apps_save_ctx_t *c = (apps_save_ctx_t *)arg;
    if (mount_impl(NULL) == ESP_OK) {
        FILE *f = fopen(APPS_PATH, "wb");
        if (f) {
            size_t n = fwrite(c->json, 1, c->len, f);
            fclose(f);
            if (n == c->len) {
                ESP_LOGI(TAG, "cached apps file (%u bytes)", (unsigned)c->len);
                return ESP_OK;
            }
        }
        ESP_LOGW(TAG, "apps file save failed, try NVS");
    }
    esp_err_t err = apps_nvs_save(c->json, c->len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "cached apps nvs (%u bytes)", (unsigned)c->len);
    } else {
        ESP_LOGW(TAG, "apps cache save: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t nodelio_apps_cache_save(const char *json, size_t len)
{
    if (!json || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    apps_save_ctx_t ctx = {.json = json, .len = len};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(apps_save_impl, &ctx);
    cache_flash_unlock();
    return err;
}

typedef struct {
    char **out_json;
} apps_load_ctx_t;

static esp_err_t apps_load_impl(void *arg)
{
    apps_load_ctx_t *c = (apps_load_ctx_t *)arg;
    if (mount_impl(NULL) == ESP_OK) {
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
                *c->out_json = buf;
                ESP_LOGI(TAG, "loaded apps file (%u bytes)", (unsigned)n);
                return ESP_OK;
            }
        }
    }
    esp_err_t err = apps_nvs_load(c->out_json);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded apps nvs");
    }
    return err;
}

esp_err_t nodelio_apps_cache_load(char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    apps_load_ctx_t ctx = {.out_json = out_json};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(apps_load_impl, &ctx);
    cache_flash_unlock();
    return err;
}

static esp_err_t apps_clear_impl(void *arg)
{
    (void)arg;
    if (mount_impl(NULL) == ESP_OK) {
        (void)unlink(APPS_PATH);
    }
    return apps_nvs_clear();
}

esp_err_t nodelio_apps_cache_clear(void)
{
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(apps_clear_impl, NULL);
    cache_flash_unlock();
    return err;
}

#define PHOTO_CACHE_PREFIX "ph_"
#define PHOTO_CACHE_SUFFIX ".jpg"
#define PHOTO_CACHE_MAX_BYTES (90 * 1024)
/** layout+apps 预留后，相册占用上限 */
#define PHOTO_CACHE_BUDGET (700 * 1024)

static bool photo_id_ok(const char *id)
{
    if (!id || !id[0]) {
        return false;
    }
    size_t n = 0;
    for (const char *p = id; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
            return false;
        }
        if (++n >= 40) {
            return false;
        }
    }
    return n > 0;
}

static void photo_path(char *out, size_t out_n, const char *id)
{
    snprintf(out, out_n, "/spiffs/%s%s%s", PHOTO_CACHE_PREFIX, id, PHOTO_CACHE_SUFFIX);
}

typedef struct {
    const char *photo_id;
    uint8_t *out;
    size_t cap;
    size_t *out_n;
} photo_load_ctx_t;

static esp_err_t photo_load_impl(void *arg)
{
    photo_load_ctx_t *c = (photo_load_ctx_t *)arg;
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        return err;
    }
    char path[96];
    photo_path(path, sizeof(path), c->photo_id);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)st.st_size > c->cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(c->out, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) {
        return ESP_FAIL;
    }
    if (c->out_n) {
        *c->out_n = n;
    }
    return ESP_OK;
}

esp_err_t nodelio_photo_cache_load(const char *photo_id, uint8_t *out, size_t cap, size_t *out_n)
{
    if (!photo_id_ok(photo_id) || !out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_n) {
        *out_n = 0;
    }
    photo_load_ctx_t ctx = {.photo_id = photo_id, .out = out, .cap = cap, .out_n = out_n};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(photo_load_impl, &ctx);
    cache_flash_unlock();
    return err;
}

typedef struct {
    const char *photo_id;
    const uint8_t *data;
    size_t len;
} photo_save_ctx_t;

static esp_err_t photo_save_impl(void *arg)
{
    photo_save_ctx_t *c = (photo_save_ctx_t *)arg;
    if (c->len == 0 || c->len > PHOTO_CACHE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        return err;
    }
    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        if (c->len > total || used + c->len > total) {
            ESP_LOGW(TAG, "photo cache skip (full used=%u/%u need=%u)", (unsigned)used, (unsigned)total,
                     (unsigned)c->len);
            return ESP_ERR_NO_MEM;
        }
        if (used + c->len > PHOTO_CACHE_BUDGET) {
            ESP_LOGW(TAG, "photo cache skip (budget used=%u need=%u)", (unsigned)used, (unsigned)c->len);
            return ESP_ERR_NO_MEM;
        }
    }
    char path[96];
    photo_path(path, sizeof(path), c->photo_id);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n = fwrite(c->data, 1, c->len, f);
    fclose(f);
    if (n != c->len) {
        (void)unlink(path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "photo cached %s (%u bytes)", c->photo_id, (unsigned)c->len);
    return ESP_OK;
}

esp_err_t nodelio_photo_cache_save(const char *photo_id, const uint8_t *data, size_t len)
{
    if (!photo_id_ok(photo_id) || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_save_ctx_t ctx = {.photo_id = photo_id, .data = data, .len = len};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(photo_save_impl, &ctx);
    cache_flash_unlock();
    return err;
}

typedef struct {
    const char *photo_id;
} photo_remove_ctx_t;

static esp_err_t photo_remove_impl(void *arg)
{
    photo_remove_ctx_t *c = (photo_remove_ctx_t *)arg;
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    char path[96];
    photo_path(path, sizeof(path), c->photo_id);
    (void)unlink(path);
    return ESP_OK;
}

esp_err_t nodelio_photo_cache_remove(const char *photo_id)
{
    if (!photo_id_ok(photo_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    photo_remove_ctx_t ctx = {.photo_id = photo_id};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(photo_remove_impl, &ctx);
    cache_flash_unlock();
    return err;
}

typedef struct {
    const char *const *ids;
    size_t n;
} photo_prune_ctx_t;

static bool photo_keep_has(const char *const *ids, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] && strcmp(ids[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t photo_prune_impl(void *arg)
{
    photo_prune_ctx_t *c = (photo_prune_ctx_t *)arg;
    esp_err_t err = mount_impl(NULL);
    if (err != ESP_OK) {
        return ESP_OK;
    }
    DIR *d = opendir("/spiffs");
    if (!d) {
        return ESP_OK;
    }
    int dropped = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t plen = strlen(PHOTO_CACHE_PREFIX);
        size_t slen = strlen(PHOTO_CACHE_SUFFIX);
        size_t nlen = strlen(name);
        if (nlen <= plen + slen || strncmp(name, PHOTO_CACHE_PREFIX, plen) != 0) {
            continue;
        }
        if (strcmp(name + (nlen - slen), PHOTO_CACHE_SUFFIX) != 0) {
            continue;
        }
        char id[40];
        size_t id_len = nlen - plen - slen;
        if (id_len >= sizeof(id)) {
            continue;
        }
        memcpy(id, name + plen, id_len);
        id[id_len] = '\0';
        if (photo_keep_has(c->ids, c->n, id)) {
            continue;
        }
        char path[96];
        photo_path(path, sizeof(path), id);
        if (unlink(path) == 0) {
            dropped++;
        }
    }
    closedir(d);
    if (dropped > 0) {
        ESP_LOGI(TAG, "photo cache pruned %d", dropped);
    }
    return ESP_OK;
}

esp_err_t nodelio_photo_cache_prune(const char *const *ids, size_t n)
{
    photo_prune_ctx_t ctx = {.ids = ids, .n = n};
    cache_flash_lock();
    esp_err_t err = cache_run_flash_safe(photo_prune_impl, &ctx);
    cache_flash_unlock();
    return err;
}
