#include "diol_lan.h"

#include "nodelio_http.h"
#include "panel_ui.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_diol";

#define DIOL_NVS_NS "nl_diol"
#define DIOL_HTTP_MS 2000
#define DIOL_RESP_CAP 1536
/* 已确认离线：长退避，避免每次 snapshot 再扫死 IP */
#define DIOL_FAIL_BACKOFF_MS 180000
/* 尚未标离线：短退避，尽快确认 */
#define DIOL_FAIL_SOFT_BACKOFF_MS 8000
#define DIOL_FAIL_CACHE 16
#define DIOL_FAIL_OFFLINE_N 2
#define DIOL_PROBE_STACK 8192
#define DIOL_PROBE_MAX 32

static struct {
    char id[40];
    int64_t fail_ms;
    uint8_t fails;
} s_fail_cache[DIOL_FAIL_CACHE];

static TaskHandle_t s_probe_task;
static StackType_t *s_probe_stack;
static StaticTask_t s_probe_tcb;
static volatile bool s_probe_pending;
static volatile bool s_probe_running;
/* 默认 hold：等小智/面板 MQTT 过 TLS 后再探，避免与 mbedtls SPIRAM alloc 并发砸堆 */
static volatile bool s_probe_hold = true;

static bool diol_fail_skip(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return false;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < DIOL_FAIL_CACHE; i++) {
        if (s_fail_cache[i].id[0] && strcmp(s_fail_cache[i].id, device_id) == 0) {
            const int64_t bo = (s_fail_cache[i].fails >= DIOL_FAIL_OFFLINE_N)
                                   ? (int64_t)DIOL_FAIL_BACKOFF_MS
                                   : (int64_t)DIOL_FAIL_SOFT_BACKOFF_MS;
            return (now - s_fail_cache[i].fail_ms) < bo;
        }
    }
    return false;
}

static uint8_t diol_fail_count(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return 0;
    }
    for (int i = 0; i < DIOL_FAIL_CACHE; i++) {
        if (s_fail_cache[i].id[0] && strcmp(s_fail_cache[i].id, device_id) == 0) {
            return s_fail_cache[i].fails;
        }
    }
    return 0;
}

/** @return 累计连续失败次数 */
static uint8_t diol_fail_note(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return 0;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    int slot = -1;
    for (int i = 0; i < DIOL_FAIL_CACHE; i++) {
        if (s_fail_cache[i].id[0] && strcmp(s_fail_cache[i].id, device_id) == 0) {
            s_fail_cache[i].fail_ms = now;
            if (s_fail_cache[i].fails < 255) {
                s_fail_cache[i].fails++;
            }
            return s_fail_cache[i].fails;
        }
        if (slot < 0 && s_fail_cache[i].id[0] == '\0') {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    snprintf(s_fail_cache[slot].id, sizeof(s_fail_cache[slot].id), "%s", device_id);
    s_fail_cache[slot].fail_ms = now;
    s_fail_cache[slot].fails = 1;
    return 1;
}

static void diol_fail_clear(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return;
    }
    for (int i = 0; i < DIOL_FAIL_CACHE; i++) {
        if (s_fail_cache[i].id[0] && strcmp(s_fail_cache[i].id, device_id) == 0) {
            s_fail_cache[i].id[0] = '\0';
            s_fail_cache[i].fail_ms = 0;
            s_fail_cache[i].fails = 0;
            return;
        }
    }
}

static void diol_mark_probe_fail(const char *device_id)
{
    uint8_t n = diol_fail_note(device_id);
    if (n >= DIOL_FAIL_OFFLINE_N) {
        nodelio_panel_ui_set_lan_online(device_id, false, "UNKNOWN");
    }
}

/** NVS key ≤15；取 deviceId 尾部 */
static void diol_nvs_key(const char *device_id, char *key, size_t key_len)
{
    if (!key || key_len < 2) {
        return;
    }
    key[0] = '\0';
    if (!device_id || !device_id[0]) {
        return;
    }
    size_t n = strlen(device_id);
    if (n <= 15) {
        snprintf(key, key_len, "%s", device_id);
    } else {
        snprintf(key, key_len, "%s", device_id + n - 15);
    }
}

void nodelio_diol_pass_get(const char *device_id, char *out, int out_len)
{
    if (!out || out_len <= 0) {
        return;
    }
    out[0] = '\0';
    if (!device_id || !device_id[0]) {
        return;
    }
    char key[16];
    diol_nvs_key(device_id, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(DIOL_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = (size_t)out_len;
    (void)nvs_get_str(h, key, out, &len);
    nvs_close(h);
}

void nodelio_diol_pass_set(const char *device_id, const char *pass)
{
    if (!device_id || !device_id[0]) {
        return;
    }
    char key[16];
    diol_nvs_key(device_id, key, sizeof(key));
    nvs_handle_t h;
    if (nvs_open(DIOL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (!pass || !pass[0]) {
        (void)nvs_erase_key(h, key);
    } else {
        (void)nvs_set_str(h, key, pass);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

static void normalize_host(const char *ip, char *host, int host_len)
{
    if (!host || host_len <= 0) {
        return;
    }
    host[0] = '\0';
    if (!ip || !ip[0]) {
        return;
    }
    while (*ip == ' ') {
        ip++;
    }
    if (strncmp(ip, "http://", 7) == 0) {
        ip += 7;
    } else if (strncmp(ip, "https://", 8) == 0) {
        ip += 8;
    }
    snprintf(host, (size_t)host_len, "%s", ip);
    char *slash = strchr(host, '/');
    if (slash) {
        *slash = '\0';
    }
}

static void apply_state_json(const char *device_id, const cJSON *root)
{
    if (!device_id || !root) {
        return;
    }
    const cJSON *states = cJSON_GetObjectItemCaseSensitive(root, "states");
    if (!cJSON_IsArray(states)) {
        return;
    }
    int n = cJSON_GetArraySize(states);
    for (int i = 0; i < n && i < NL_CH_PER_DEV; i++) {
        const cJSON *st = cJSON_GetArrayItem(states, i);
        bool on = cJSON_IsTrue(st) || (cJSON_IsNumber(st) && st->valuedouble != 0);
        const char *ch_name = "";
        const cJSON *names = cJSON_GetObjectItemCaseSensitive(root, "names");
        if (cJSON_IsArray(names)) {
            const cJSON *nm = cJSON_GetArrayItem(names, i);
            if (cJSON_IsString(nm) && nm->valuestring) {
                ch_name = nm->valuestring;
            }
        }
        nodelio_panel_ui_snapshot_add_channel(device_id, i, ch_name, on ? "ON" : "OFF");
        const cJSON *cds = cJSON_GetObjectItemCaseSensitive(root, "countdownSec");
        if (cJSON_IsArray(cds)) {
            const cJSON *cd = cJSON_GetArrayItem(cds, i);
            if (cJSON_IsNumber(cd) && cd->valuedouble > 0) {
                nodelio_panel_ui_note_countdown(device_id, i, (int)cd->valuedouble);
            }
        }
    }
    bool any_on = false;
    for (int i = 0; i < n; i++) {
        const cJSON *st = cJSON_GetArrayItem(states, i);
        if (cJSON_IsTrue(st) || (cJSON_IsNumber(st) && st->valuedouble != 0)) {
            any_on = true;
            break;
        }
    }
    nodelio_panel_ui_set_lan_online(device_id, true, any_on ? "ON" : "OFF");
}

static esp_err_t probe_one(const char *device_id, const char *ip)
{
    if (!device_id || !device_id[0] || !ip || !ip[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char host[48];
    normalize_host(ip, host, sizeof(host));
    if (!host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char pass[48];
    nodelio_diol_pass_get(device_id, pass, sizeof(pass));

    char url[96];
    snprintf(url, sizeof(url), "http://%s/api/state", host);
    char *resp = (char *)heap_caps_malloc(DIOL_RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = (char *)malloc(DIOL_RESP_CAP);
    }
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    resp[0] = '\0';
    int status = 0;
    esp_err_t err = nodelio_http_lan_probe_get(url, pass[0] ? pass : NULL, &status, resp, DIOL_RESP_CAP - 1);
    if (status == 401) {
        nodelio_panel_ui_set_lan_online(device_id, true, "UNKNOWN");
        nodelio_panel_ui_set_lan_pass_required(device_id, true);
        heap_caps_free(resp);
        diol_fail_clear(device_id);
        ESP_LOGI(TAG, "probe %s → need pass (online)", device_id);
        return ESP_OK;
    }
    if (err != ESP_OK || status != 200) {
        heap_caps_free(resp);
        diol_mark_probe_fail(device_id);
        ESP_LOGD(TAG, "probe %s fail http=%d err=%s", device_id, status, esp_err_to_name(err));
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);
    if (!root) {
        diol_mark_probe_fail(device_id);
        return ESP_FAIL;
    }
    nodelio_panel_ui_set_lan_pass_required(device_id, false);
    diol_fail_clear(device_id);
    nodelio_panel_ui_clear_lan_channels(device_id);
    apply_state_json(device_id, root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "probe %s ok", device_id);
    return ESP_OK;
}

typedef struct {
    char id[NL_LAYOUT_STR];
    char ip[48];
    uint8_t fails;
} diol_probe_job_t;

static diol_probe_job_t *s_jobs;

static void probe_worker(void *arg)
{
    (void)arg;
    if (!s_jobs) {
        s_jobs = (diol_probe_job_t *)heap_caps_calloc(DIOL_PROBE_MAX, sizeof(*s_jobs),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    for (;;) {
        while (s_probe_hold || !s_probe_pending) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        s_probe_pending = false;
        if (!s_jobs) {
            continue;
        }
        s_probe_running = true;

        /* 拷贝待探名单：先探历史失败少的（活设备优先），死 IP 放后 */
        int njob = 0;
        const int ndev = nodelio_panel_ui_get_dev_count();
        for (int i = 0; i < ndev && njob < DIOL_PROBE_MAX; i++) {
            const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
            if (!d || !d->lan_control || !d->ip[0]) {
                continue;
            }
            if (diol_fail_skip(d->id)) {
                ESP_LOGD(TAG, "probe skip backoff %s", d->id);
                continue;
            }
            snprintf(s_jobs[njob].id, sizeof(s_jobs[njob].id), "%.*s", (int)sizeof(s_jobs[njob].id) - 1,
                     d->id);
            snprintf(s_jobs[njob].ip, sizeof(s_jobs[njob].ip), "%.*s", (int)sizeof(s_jobs[njob].ip) - 1,
                     d->ip);
            s_jobs[njob].fails = diol_fail_count(d->id);
            njob++;
        }
        for (int a = 0; a + 1 < njob; a++) {
            for (int b = a + 1; b < njob; b++) {
                if (s_jobs[b].fails < s_jobs[a].fails) {
                    diol_probe_job_t t = s_jobs[a];
                    s_jobs[a] = s_jobs[b];
                    s_jobs[b] = t;
                }
            }
        }

        ESP_LOGI(TAG, "probe round n=%d (async, short-tcp)", njob);
        bool any = false;
        for (int i = 0; i < njob; i++) {
            if (s_probe_hold || s_probe_pending) {
                /* hold / 新一轮：打断当前扫尾 */
                break;
            }
            any = true;
            (void)probe_one(s_jobs[i].id, s_jobs[i].ip);
        }
        if (any && !s_probe_hold) {
            nodelio_panel_ui_refresh_tiles();
        }
        s_probe_running = false;
    }
}

static void ensure_probe_task(void)
{
    if (s_probe_task) {
        return;
    }
    if (!s_probe_stack) {
        s_probe_stack = heap_caps_malloc(DIOL_PROBE_STACK * sizeof(StackType_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_probe_stack) {
        ESP_LOGW(TAG, "probe task stack alloc fail — sync fallback");
        return;
    }
    s_probe_task = xTaskCreateStatic(probe_worker, "nl_diol_probe", DIOL_PROBE_STACK, NULL, 3, s_probe_stack,
                                     &s_probe_tcb);
    if (!s_probe_task) {
        ESP_LOGW(TAG, "probe task create fail");
    }
}

void nodelio_diol_set_probe_hold(bool hold)
{
    s_probe_hold = hold;
}

void nodelio_diol_wait_idle(uint32_t timeout_ms)
{
    s_probe_hold = true;
    s_probe_pending = false;
    uint32_t waited = 0;
    while (s_probe_running && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
}

void nodelio_diol_boot_quiet_end(void)
{
    s_probe_hold = false;
    /* snapshot 期间已排队的探测现在放行 */
    if (s_probe_task) {
        s_probe_pending = true;
    }
    ESP_LOGI(TAG, "boot quiet end — probes allowed");
}

void nodelio_diol_probe_all_from_cache(void)
{
    /* snapshot 已用 sticky/lan cache 刷过 UI；探测丢后台，勿堵 layout 任务 */
    ensure_probe_task();
    if (!s_probe_task) {
        if (s_probe_hold) {
            ESP_LOGI(TAG, "probe sync skipped (hold)");
            return;
        }
        /* 无任务时同步短探一轮（仍用 probe_get，不再 2s×3） */
        int n = nodelio_panel_ui_get_dev_count();
        bool any = false;
        for (int i = 0; i < n; i++) {
            const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
            if (!d || !d->lan_control || !d->ip[0] || diol_fail_skip(d->id)) {
                continue;
            }
            any = true;
            (void)probe_one(d->id, d->ip);
        }
        if (any) {
            nodelio_panel_ui_refresh_tiles();
        }
        return;
    }
    s_probe_pending = true;
    ESP_LOGI(TAG, "probe scheduled%s (ui already sticky)", s_probe_hold ? " (held)" : "");
}

esp_err_t nodelio_diol_try_command(const nl_bind_t *bind, const char *value_str, int *http_status)
{
    if (http_status) {
        *http_status = 0;
    }
    if (!bind || bind->kind != NL_BIND_DEVICE_CHANNEL || !bind->device_id[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    const nl_dev_cache_t *dev = NULL;
    int n = nodelio_panel_ui_get_dev_count();
    for (int i = 0; i < n; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (d && strcmp(d->id, bind->device_id) == 0) {
            dev = d;
            break;
        }
    }
    if (!dev || !dev->lan_control) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!dev->ip[0]) {
        if (http_status) {
            *http_status = 503;
        }
        return ESP_ERR_INVALID_STATE;
    }
    char host[48];
    normalize_host(dev->ip, host, sizeof(host));
    char pass[48];
    nodelio_diol_pass_get(bind->device_id, pass, sizeof(pass));

    const char *cmd = (value_str && value_str[0]) ? value_str : "TOGGLE";
    int ch = bind->channel;
    if (ch < 0) {
        ch = 0;
    }
    char url[96];
    snprintf(url, sizeof(url), "http://%s/api/channel", host);
    char form[64];
    snprintf(form, sizeof(form), "channel=%d&cmd=%s", ch, cmd);

    char *resp = (char *)heap_caps_malloc(DIOL_RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = (char *)malloc(DIOL_RESP_CAP);
    }
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    resp[0] = '\0';
    int status = 0;
    esp_err_t err =
        nodelio_http_lan_post_form_ms(url, pass[0] ? pass : NULL, form, &status, resp, DIOL_RESP_CAP - 1, DIOL_HTTP_MS);
    if (http_status) {
        *http_status = status;
    }
    if (status == 401) {
        heap_caps_free(resp);
        nodelio_panel_ui_set_lan_pass_required(bind->device_id, true);
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || status != 200) {
        heap_caps_free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    heap_caps_free(resp);
    if (root) {
        nodelio_panel_ui_clear_lan_channels(bind->device_id);
        apply_state_json(bind->device_id, root);
        cJSON_Delete(root);
        nodelio_panel_ui_refresh_tiles();
    } else {
        nodelio_panel_ui_apply_bind_state(bind, cmd);
    }
    ESP_LOGI(TAG, "cmd %s ch=%d %s ok", bind->device_id, ch, cmd);
    return ESP_OK;
}
