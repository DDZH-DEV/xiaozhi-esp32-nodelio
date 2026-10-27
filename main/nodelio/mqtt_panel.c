#include "mqtt_panel.h"

#include "panel_layout.h"
#include "panel_ui.h"
#include "wifi_provision.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "nl_mqtt";

static esp_mqtt_client_handle_t s_client;
static nodelio_nvs_config_t s_cfg;
static char s_notify_topic[96];
static TimerHandle_t s_debounce;
static volatile bool s_need_layout;
static volatile bool s_need_snapshot;
static volatile bool s_need_snapshot_force;
static volatile bool s_connected;

static void schedule_kick_ex(bool layout, bool force)
{
    if (layout) {
        s_need_layout = true;
    } else {
        s_need_snapshot = true;
        if (force) {
            s_need_snapshot_force = true;
        }
    }
    if (!s_debounce) {
        return;
    }
    xTimerStop(s_debounce, 0);
    xTimerChangePeriod(s_debounce, pdMS_TO_TICKS(300), 0);
    xTimerStart(s_debounce, 0);
}

static void schedule_kick(bool layout)
{
    schedule_kick_ex(layout, false);
}

static void debounce_cb(TimerHandle_t t)
{
    (void)t;
    bool layout = s_need_layout;
    bool snap = s_need_snapshot;
    bool force = s_need_snapshot_force;
    s_need_layout = false;
    s_need_snapshot = false;
    s_need_snapshot_force = false;
    if (layout) {
        nodelio_layout_request_sync();
    } else if (snap) {
        if (force) {
            nodelio_layout_request_snapshot_force();
        } else {
            nodelio_layout_request_snapshot();
        }
    }
}

static void handle_notify_payload(const char *data, int len)
{
    if (!data || len <= 0) {
        schedule_kick(false);
        return;
    }
    char tmp[480];
    int n = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    memcpy(tmp, data, (size_t)n);
    tmp[n] = '\0';
    cJSON *root = cJSON_Parse(tmp);
    if (!root) {
        schedule_kick(false);
        return;
    }
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "kind");
    if (cJSON_IsString(kind) && kind->valuestring && strcmp(kind->valuestring, "layout") == 0) {
        schedule_kick(true);
    } else if (cJSON_IsString(kind) && kind->valuestring &&
               strcmp(kind->valuestring, "skills") == 0) {
        /* 只拉已装清单，不拉 layout/snapshot，不拉市场 */
        nodelio_panel_ui_skills_changed();
    } else if (cJSON_IsString(kind) && kind->valuestring &&
               strcmp(kind->valuestring, "skill_hint") == 0) {
        /* PC 桥边沿：needs_you / completed → 单次 invoke，无轮询 */
        const cJSON *inst = cJSON_GetObjectItemCaseSensitive(root, "instance_id");
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(root, "app_id");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
        const char *id = nullptr;
        if (cJSON_IsString(inst) && inst->valuestring && inst->valuestring[0]) {
            id = inst->valuestring;
        } else if (cJSON_IsString(app) && app->valuestring && app->valuestring[0]) {
            id = app->valuestring;
        }
        if (id) {
            nodelio_panel_ui_skill_hint(
                id, cJSON_IsString(reason) ? reason->valuestring : nullptr,
                cJSON_IsString(session) ? session->valuestring : nullptr);
        }
    } else if (cJSON_IsString(kind) && kind->valuestring &&
               strcmp(kind->valuestring, "countdown") == 0) {
        /* 轻量推送：不拉 HTTP，直接改本地剩余秒 */
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "deviceId");
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "channel");
        const cJSON *rem = cJSON_GetObjectItemCaseSensitive(root, "remainingSeconds");
        if (cJSON_IsString(id) && id->valuestring && id->valuestring[0] && cJSON_IsNumber(rem)) {
            int ch_idx = cJSON_IsNumber(ch) ? (int)ch->valuedouble : -1;
            int sec = (int)rem->valuedouble;
            if (sec < 0) {
                sec = 0;
            }
            nodelio_panel_ui_set_countdown(id->valuestring, ch_idx, sec);
        }
    } else {
        /* snapshot；force=true 时跳过固件 ≥60s 限频（online/status） */
        const cJSON *force = cJSON_GetObjectItemCaseSensitive(root, "force");
        schedule_kick_ex(false, cJSON_IsTrue(force));
    }
    cJSON_Delete(root);
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t ev = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected, subscribe %s", s_notify_topic);
        esp_mqtt_client_subscribe(s_client, s_notify_topic, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len > 0 && s_notify_topic[0] &&
            ev->topic_len == (int)strlen(s_notify_topic) &&
            strncmp(ev->topic, s_notify_topic, (size_t)ev->topic_len) == 0) {
            handle_notify_payload(ev->data, ev->data_len);
        }
        break;
    default:
        break;
    }
}

esp_err_t nodelio_mqtt_panel_start(const nodelio_nvs_config_t *cfg)
{
    if (!cfg || cfg->mqtt_server[0] == '\0' || cfg->device_id[0] == '\0') {
        ESP_LOGW(TAG, "skip: no mqtt_server/device_id");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client) {
        return ESP_OK;
    }
    s_cfg = *cfg;
    const char *prefix = s_cfg.topic_prefix[0] ? s_cfg.topic_prefix : "nodelio-devices";
    snprintf(s_notify_topic, sizeof(s_notify_topic), "%s/%s/panel/notify", prefix, s_cfg.device_id);

    const char *port = s_cfg.mqtt_port[0] ? s_cfg.mqtt_port : "1883";
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%s", s_cfg.mqtt_server, port);

    if (!s_debounce) {
        s_debounce = xTimerCreate("nl_mqtt_db", pdMS_TO_TICKS(300), pdFALSE, NULL, debounce_cb);
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = s_cfg.mqtt_user[0] ? s_cfg.mqtt_user : NULL,
        .credentials.authentication.password = s_cfg.mqtt_pass[0] ? s_cfg.mqtt_pass : NULL,
        .session.keepalive = 60,
        .network.disable_auto_reconnect = false,
        /* 明文 mqtt://；栈/缓冲走 PSRAM。notify 很小，收紧 buffer 少占堆。 */
        .buffer.size = 1024,
        .buffer.out_size = 512,
        .task.stack_size = 4096,
    };
    nodelio_log_heap("mqtt_panel init");
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed (NO_MEM?)");
        nodelio_log_heap("mqtt_panel init fail");
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start failed: %s (often=create mqtt task OOM)", esp_err_to_name(err));
        nodelio_log_heap("mqtt_panel start fail");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "started uri=%s topic=%s", uri, s_notify_topic);
    nodelio_log_heap("mqtt_panel started");
    return ESP_OK;
}

bool nodelio_mqtt_panel_connected(void)
{
    return s_connected;
}

void nodelio_mqtt_panel_stop(void)
{
    s_connected = false;
    if (!s_client) {
        return;
    }
    if (s_debounce) {
        xTimerStop(s_debounce, 0);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    ESP_LOGI(TAG, "stopped");
}
