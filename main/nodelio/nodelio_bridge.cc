#include "nodelio_bridge.h"

#include "application.h"
#include "mqtt_panel.h"
#include "nodelio_overlay.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "register.h"
#include "sdkconfig.h"
#include "wifi_provision.h"

#include <cstdio>
#include <cstring>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <functional>

#define TAG "nl_bridge"

static bool s_started;
static nodelio_nvs_config_t s_cfg;
static QueueHandle_t s_pwr_q;

enum { PWR_SLEEP = 0, PWR_WAKE = 1 };

/** 在主任务执行并等待完成（息屏挂起小智 MQTT 须先于断网） */
static void run_on_main_sync(std::function<void()> fn)
{
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        fn();
        return;
    }
    Application::GetInstance().Schedule([fn = std::move(fn), sem]() {
        fn();
        xSemaphoreGive(sem);
    });
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "main sync timeout");
    }
    vSemaphoreDelete(sem);
}

static void seed_from_kconfig(nodelio_nvs_config_t *cfg)
{
    if (cfg->server_url[0] == '\0' && CONFIG_NODELIO_SERVER_URL[0] != '\0') {
        std::snprintf(cfg->server_url, sizeof(cfg->server_url), "%s", CONFIG_NODELIO_SERVER_URL);
    }
    if (cfg->owner[0] == '\0' && CONFIG_NODELIO_OWNER[0] != '\0') {
        std::snprintf(cfg->owner, sizeof(cfg->owner), "%s", CONFIG_NODELIO_OWNER);
    }
}

static void pwr_task(void *arg)
{
    (void)arg;
    int cmd = PWR_WAKE;
    ESP_LOGI(TAG, "pwr task up (screen-off → WiFi/MQTT off)");
    while (true) {
        if (xQueueReceive(s_pwr_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int newer;
        while (xQueueReceive(s_pwr_q, &newer, 0) == pdTRUE) {
            cmd = newer;
        }

        if (cmd == PWR_SLEEP) {
            ESP_LOGI(TAG, "net sleep: suspend xz MQTT → panel MQTT → wifi");
            run_on_main_sync([]() { Application::GetInstance().SuspendTransport(); });
            nodelio_mqtt_panel_stop();
            nodelio_wifi_sleep();
            continue;
        }

        ESP_LOGI(TAG, "net wake: wifi + mqtt + heartbeat");
        nodelio_wifi_wake();
        if (!nodelio_wifi_wait_connected(20000)) {
            ESP_LOGW(TAG, "WiFi not up in 20s — skip hb/mqtt");
            continue;
        }
        esp_err_t mq = nodelio_mqtt_panel_start(&s_cfg);
        if (mq != ESP_OK) {
            ESP_LOGW(TAG, "mqtt: %s", esp_err_to_name(mq));
        }
        esp_err_t hb = nodelio_device_heartbeat(&s_cfg);
        if (hb != ESP_OK) {
            ESP_LOGW(TAG, "wake heartbeat: %s", esp_err_to_name(hb));
        }
        run_on_main_sync([]() { Application::GetInstance().ResumeTransport(); });
    }
}

static void on_screen(bool on)
{
    if (!s_pwr_q) {
        return;
    }
    int cmd = on ? PWR_WAKE : PWR_SLEEP;
    if (xQueueOverwrite(s_pwr_q, &cmd) != pdTRUE) {
        (void)xQueueSend(s_pwr_q, &cmd, 0);
    }
}

static void ensure_pwr_task(void)
{
    if (s_pwr_q) {
        return;
    }
    s_pwr_q = xQueueCreate(1, sizeof(int));
    if (!s_pwr_q) {
        return;
    }
    nodelio_panel_ui_set_screen_cb(on_screen);
    if (xTaskCreate(pwr_task, "nl_pwr", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "pwr task create fail");
    }
}

static esp_err_t start_client(void)
{
    nodelio_nvs_config_t cfg{};
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs load: %s", esp_err_to_name(err));
        return err;
    }
    bool had_url = cfg.server_url[0] != '\0';
    bool had_owner = cfg.owner[0] != '\0';
    seed_from_kconfig(&cfg);
    if ((!had_url && cfg.server_url[0]) || (!had_owner && cfg.owner[0])) {
        (void)nodelio_nvs_save(&cfg);
    }

    if (cfg.server_url[0] == '\0' || cfg.owner[0] == '\0') {
        ESP_LOGW(TAG, "未绑定：点右下角「家」→ 设置里填服务器与绑定码");
        (void)nodelio_overlay_init();
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = cfg;
    (void)nodelio_panel_ui_start(&cfg);
    ensure_pwr_task();

    bool cloud = false;
    err = nodelio_device_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register: %s", esp_err_to_name(err));
        if (cfg.device_id[0] == '\0' || cfg.device_key[0] == '\0') {
            return err;
        }
    } else {
        (void)nodelio_nvs_save(&cfg);
        s_cfg = cfg;
        cloud = true;
    }

    err = nodelio_device_heartbeat(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heartbeat: %s", esp_err_to_name(err));
    } else {
        cloud = true;
    }

    err = nodelio_mqtt_panel_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mqtt panel: %s", esp_err_to_name(err));
    }

    err = nodelio_layout_start_poll(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "layout poll: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    bool wifi_ok = nodelio_wifi_is_connected();
    nodelio_panel_ui_set_status(wifi_ok, cloud || nodelio_mqtt_panel_connected(), "v0");
    ESP_LOGI(TAG, "NodeLio panel up device_id=%s server=%s", cfg.device_id, cfg.server_url);
    return ESP_OK;
}

esp_err_t nodelio_bridge_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nodelio_nvs_init());
    return start_client();
}

esp_err_t nodelio_bridge_restart(void)
{
    nodelio_bridge_stop();
    return start_client();
}

void nodelio_bridge_stop(void)
{
    if (!s_started) {
        return;
    }
    nodelio_mqtt_panel_stop();
    s_started = false;
}
