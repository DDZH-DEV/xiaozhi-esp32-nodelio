#include "register.h"

#include "board_info.h"
#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_reg";

#define FW_VERSION "0.3.17"

typedef struct {
    char *buf;
    int len;
    int cap;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    http_buf_t *hb = evt->user_data;
    if (hb->len + evt->data_len + 1 > hb->cap) {
        return ESP_OK;
    }
    memcpy(hb->buf + hb->len, evt->data, (size_t)evt->data_len);
    hb->len += evt->data_len;
    hb->buf[hb->len] = '\0';
    return ESP_OK;
}

static void ensure_ids(nodelio_nvs_config_t *cfg)
{
    bool dirty = false;
    if (cfg->device_id[0] == '\0') {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(cfg->device_id, sizeof(cfg->device_id), "WP-%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        dirty = true;
    }
    if (cfg->device_key[0] == '\0') {
        /* 32 hex chars */
        for (int i = 0; i < 32; i++) {
            const char *hex = "0123456789abcdef";
            cfg->device_key[i] = hex[esp_random() & 0xF];
        }
        cfg->device_key[32] = '\0';
        dirty = true;
    }
    if (dirty) {
        nodelio_nvs_save(cfg);
        ESP_LOGI(TAG, "issued device_id=%s", cfg->device_id);
    }
}

static void local_ip(char *buf, size_t len)
{
    buf[0] = '\0';
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
}

static void build_register_url(const char *base, char *out, size_t out_len)
{
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    if (n >= 22 && strncmp(base + n - 22, "/api/devices/register", 22) == 0) {
        snprintf(out, out_len, "%.*s", (int)n, base);
        return;
    }
    snprintf(out, out_len, "%.*s/api/devices/register", (int)n, base);
}

static void build_heartbeat_url(const char *base, const char *device_id, char *out, size_t out_len)
{
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    /* strip trailing /api/devices/register if user pasted full path */
    if (n >= 22 && strncmp(base + n - 22, "/api/devices/register", 22) == 0) {
        n -= 22;
    }
    snprintf(out, out_len, "%.*s/api/devices/%s/heartbeat", (int)n, base, device_id);
}

static void copy_json_str(char *dst, size_t dst_len, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, dst_len, "%s", v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        snprintf(dst, dst_len, "%d", v->valueint);
    }
}

static bool apply_mqtt_provision(nodelio_nvs_config_t *cfg, const char *body)
{
    if (!cfg || !body || body[0] == '\0') {
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return false;
    }
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(root, "mqtt");
    if (!cJSON_IsObject(mqtt)) {
        cJSON_Delete(root);
        return false;
    }
    char server[40] = {0};
    char port[8] = {0};
    char user[64] = {0};
    char pass[40] = {0};
    char prefix[40] = {0};
    copy_json_str(server, sizeof(server), mqtt, "mqtt_server");
    copy_json_str(port, sizeof(port), mqtt, "mqtt_port");
    copy_json_str(user, sizeof(user), mqtt, "mqtt_user");
    copy_json_str(pass, sizeof(pass), mqtt, "mqtt_pass");
    copy_json_str(prefix, sizeof(prefix), mqtt, "topic_prefix");
    cJSON_Delete(root);

    if (server[0] == '\0') {
        return false;
    }
    if (port[0] == '\0') {
        snprintf(port, sizeof(port), "1883");
    }
    if (prefix[0] == '\0') {
        snprintf(prefix, sizeof(prefix), "nodelio-devices");
    }
    bool changed = strcmp(cfg->mqtt_server, server) != 0 || strcmp(cfg->mqtt_port, port) != 0 ||
                   strcmp(cfg->mqtt_user, user) != 0 || strcmp(cfg->mqtt_pass, pass) != 0 ||
                   strcmp(cfg->topic_prefix, prefix) != 0;
    if (!changed) {
        return false;
    }
    snprintf(cfg->mqtt_server, sizeof(cfg->mqtt_server), "%s", server);
    snprintf(cfg->mqtt_port, sizeof(cfg->mqtt_port), "%s", port);
    snprintf(cfg->mqtt_user, sizeof(cfg->mqtt_user), "%s", user);
    snprintf(cfg->mqtt_pass, sizeof(cfg->mqtt_pass), "%s", pass);
    snprintf(cfg->topic_prefix, sizeof(cfg->topic_prefix), "%s", prefix);
    nodelio_nvs_save(cfg);
    ESP_LOGI(TAG, "mqtt provision saved host=%s:%s prefix=%s", cfg->mqtt_server, cfg->mqtt_port, cfg->topic_prefix);
    return true;
}

static esp_err_t http_post_json(const char *url, const char *device_key, const char *body, int *out_status,
                                char *resp, int resp_cap)
{
    http_buf_t hb = {.buf = resp, .len = 0, .cap = resp_cap};
    if (resp && resp_cap > 0) {
        resp[0] = '\0';
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .event_handler = resp ? http_event : NULL,
        .user_data = resp ? &hb : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Key", device_key);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (out_status) {
            *out_status = status;
        }
        ESP_LOGI(TAG, "POST %s → HTTP %d", url, status);
        if (status != 200 && status != 201) {
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t nodelio_device_register(nodelio_nvs_config_t *cfg)
{
    if (!cfg || cfg->server_url[0] == '\0') {
        ESP_LOGW(TAG, "skip register: no server_url");
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->owner[0] == '\0') {
        ESP_LOGW(TAG, "skip register: no owner/bind code");
        return ESP_ERR_INVALID_STATE;
    }

    ensure_ids(cfg);

    const nodelio_board_t *board = nodelio_board_get();
    char ip[32];
    local_ip(ip, sizeof(ip));

    /* SERVER models.RegistrationRequest.Config 是 string，须传转义后的 JSON 文本 */
    char body[768];
    snprintf(body, sizeof(body),
             "{\"deviceId\":\"%s\",\"ipAddress\":\"%s\",\"mqttId\":\"%s\",\"type\":\"%s\","
             "\"owner\":\"%s\",\"firmwareVersion\":\"%s\","
             "\"config\":\"{\\\"board_id\\\":\\\"%s\\\",\\\"display\\\":{\\\"w\\\":%u,\\\"h\\\":%u,\\\"touch\\\":%s},"
             "\\\"features\\\":[\\\"panel_layout\\\",\\\"offline_cmd\\\",\\\"wake_word\\\",\\\"xiaozhi_chat\\\"],"
             "\\\"firmware_family\\\":\\\"esp32-idf\\\"}\"}",
             cfg->device_id, ip, cfg->device_id, CONFIG_NODELIO_TYPE_CODE, cfg->owner, FW_VERSION, board->info.board_id,
             (unsigned)board->info.display_w, (unsigned)board->info.display_h, board->info.touch ? "true" : "false");

    char url[192];
    build_register_url(cfg->server_url, url, sizeof(url));

    const int cap = 2048;
    char *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(cap);
    }
    int status = 0;
    esp_err_t err = http_post_json(url, cfg->device_key, body, &status, resp, resp ? cap : 0);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered OK device_id=%s type=%s", cfg->device_id, CONFIG_NODELIO_TYPE_CODE);
        if (resp) {
            apply_mqtt_provision(cfg, resp);
        }
    }
    free(resp);
    return err;
}

esp_err_t nodelio_device_heartbeat(const nodelio_nvs_config_t *cfg)
{
    if (!cfg || cfg->server_url[0] == '\0' || cfg->device_id[0] == '\0' || cfg->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[192];
    build_heartbeat_url(cfg->server_url, cfg->device_id, url, sizeof(url));
    int status = 0;
    /* heartbeat accepts empty body */
    return http_post_json(url, cfg->device_key, "{}", &status, NULL, 0);
}
