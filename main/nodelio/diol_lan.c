#include "diol_lan.h"

#include "nodelio_http.h"
#include "panel_ui.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_diol";

#define DIOL_NVS_NS "nl_diol"
#define DIOL_HTTP_MS 2500
#define DIOL_RESP_CAP 1536

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

static esp_err_t probe_one(const nl_dev_cache_t *dev)
{
    if (!dev || !dev->lan_control || !dev->ip[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char host[48];
    normalize_host(dev->ip, host, sizeof(host));
    if (!host[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char pass[48];
    nodelio_diol_pass_get(dev->id, pass, sizeof(pass));

    char url[96];
    snprintf(url, sizeof(url), "http://%s/api/state", host);
    char *resp = malloc(DIOL_RESP_CAP);
    if (!resp) {
        return ESP_ERR_NO_MEM;
    }
    resp[0] = '\0';
    int status = 0;
    esp_err_t err = nodelio_http_lan_get_ms(url, pass[0] ? pass : NULL, &status, resp, DIOL_RESP_CAP - 1, DIOL_HTTP_MS);
    if (status == 401) {
        /* 需口令仍视为同网在线（与 APP 一致） */
        nodelio_panel_ui_set_lan_online(dev->id, true, "UNKNOWN");
        nodelio_panel_ui_set_lan_pass_required(dev->id, true);
        free(resp);
        ESP_LOGI(TAG, "probe %s → need pass (online)", dev->id);
        return ESP_OK;
    }
    if (err != ESP_OK || status != 200) {
        nodelio_panel_ui_set_lan_online(dev->id, false, "UNKNOWN");
        free(resp);
        ESP_LOGD(TAG, "probe %s fail http=%d err=%s", dev->id, status, esp_err_to_name(err));
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        nodelio_panel_ui_set_lan_online(dev->id, false, "UNKNOWN");
        return ESP_FAIL;
    }
    nodelio_panel_ui_set_lan_pass_required(dev->id, false);
    /* 清旧通道再写 */
    nodelio_panel_ui_clear_lan_channels(dev->id);
    apply_state_json(dev->id, root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "probe %s ok", dev->id);
    return ESP_OK;
}

void nodelio_diol_probe_all_from_cache(void)
{
    int n = nodelio_panel_ui_get_dev_count();
    bool any = false;
    for (int i = 0; i < n; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->lan_control || !d->ip[0]) {
            continue;
        }
        any = true;
        (void)probe_one(d);
    }
    if (any) {
        nodelio_panel_ui_refresh_tiles();
    }
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

    char *resp = malloc(DIOL_RESP_CAP);
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
        free(resp);
        nodelio_panel_ui_set_lan_pass_required(bind->device_id, true);
        return ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK || status != 200) {
        free(resp);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(resp);
    free(resp);
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
