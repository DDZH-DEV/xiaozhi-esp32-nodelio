#include "register.h"

#include "board_info.h"
#include "device_caps.h"
#include "nodelio_http.h"
#include "sdkconfig.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_reg";

#define FW_VERSION "0.3.18"

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

    const nodelio_display_profile_t* profile = nodelio_display_profile();
    cJSON* root = cJSON_CreateObject();
    cJSON* config = cJSON_CreateObject();
    cJSON* display = config ? cJSON_AddObjectToObject(config, "display") : NULL;
    cJSON* dp = config ? cJSON_AddObjectToObject(config, "display_profile") : NULL;
    const char* features[] = {"panel_layout", "offline_cmd", "wake_word", "xiaozhi_chat"};
    bool ok = root && config && display && dp &&
              cJSON_AddStringToObject(root, "deviceId", cfg->device_id) &&
              cJSON_AddStringToObject(root, "ipAddress", ip) &&
              cJSON_AddStringToObject(root, "mqttId", cfg->device_id) &&
              cJSON_AddStringToObject(root, "type", CONFIG_NODELIO_TYPE_CODE) &&
              cJSON_AddStringToObject(root, "owner", cfg->owner) &&
              cJSON_AddStringToObject(root, "firmwareVersion", FW_VERSION) &&
              cJSON_AddStringToObject(config, "board_id", board->info.board_id) &&
              cJSON_AddStringToObject(config, "firmware_family", "esp32-idf") &&
              cJSON_AddNumberToObject(display, "w", board->info.display_w) &&
              cJSON_AddNumberToObject(display, "h", board->info.display_h) &&
              cJSON_AddBoolToObject(display, "touch", board->info.touch) &&
              cJSON_AddStringToObject(dp, "id", profile->id) &&
              cJSON_AddNumberToObject(dp, "cols", profile->grid_cols) &&
              cJSON_AddNumberToObject(dp, "rows", profile->grid_rows) &&
              cJSON_AddNumberToObject(dp, "nav_width", profile->nav_width);
    cJSON* feature_array = ok ? cJSON_CreateStringArray(features, 4) : NULL;
    if (!feature_array || !cJSON_AddItemToObject(config, "features", feature_array)) {
        cJSON_Delete(feature_array);
        ok = false;
    }
    char* config_json = ok ? cJSON_PrintUnformatted(config) : NULL;
    ok = config_json && cJSON_AddStringToObject(root, "config", config_json);
    cJSON_free(config_json);
    cJSON_Delete(config);
    char* body = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    char url[192];
    build_register_url(cfg->server_url, url, sizeof(url));

    const int cap = 2048;
    char *resp = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = malloc(cap);
    }
    int status = 0;
    ESP_LOGI(TAG, "register device_id=%s owner=%s", cfg->device_id, cfg->owner);
    esp_err_t err = nodelio_http_post_json(url, cfg->device_key, body, &status, resp, resp ? cap : 0);
    cJSON_free(body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "registered OK device_id=%s type=%s", cfg->device_id, CONFIG_NODELIO_TYPE_CODE);
        if (resp) {
            apply_mqtt_provision(cfg, resp);
        }
    } else {
        ESP_LOGW(TAG, "POST %s failed: %s http=%d body=%s", url, esp_err_to_name(err), status,
                 (resp && resp[0]) ? resp : "(empty)");
    }
    free(resp);
    return err;
}

static int64_t s_presence_touch_ms;

void nodelio_presence_note_touch(void)
{
    s_presence_touch_ms = esp_timer_get_time() / 1000;
}

bool nodelio_presence_recent(uint32_t within_ms)
{
    if (s_presence_touch_ms <= 0 || within_ms == 0) {
        return false;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    return (now - s_presence_touch_ms) < (int64_t)within_ms;
}

esp_err_t nodelio_device_heartbeat(const nodelio_nvs_config_t *cfg)
{
    if (!cfg || cfg->server_url[0] == '\0' || cfg->device_id[0] == '\0' || cfg->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char url[192];
    build_heartbeat_url(cfg->server_url, cfg->device_id, url, sizeof(url));

    char caps[1536];
    int n = nodelio_caps_export_json(caps, sizeof(caps));
    char body[1600];
    if (n > 0) {
        snprintf(body, sizeof(body), "{\"capabilities\":%s}", caps);
    } else {
        snprintf(body, sizeof(body), "{}");
    }
    int status = 0;
    esp_err_t err = nodelio_http_post_json(url, cfg->device_key, body, &status, NULL, 0);
    if (err == ESP_OK && status >= 200 && status < 300) {
        nodelio_presence_note_touch();
    }
    return err;
}
