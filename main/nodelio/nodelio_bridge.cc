#include "nodelio_bridge.h"

#include "application.h"
#include "device_state.h"
#include "mqtt_panel.h"
#include "nodelio_overlay.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "register.h"
#include "sdkconfig.h"
#include "wifi_provision.h"

#include <esp_wifi.h>

#include <atomic>
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
static bool s_starting;
static nodelio_nvs_config_t s_cfg;
static QueueHandle_t s_pwr_q;

enum { PWR_SLEEP = 0, PWR_WAKE = 1 };

struct MainSyncGate {
    SemaphoreHandle_t sem = nullptr;
    std::atomic<bool> done{false};
};

/**
 * 在主任务执行并等待完成（息屏挂起小智 MQTT 须先于断网）。
 * 禁止超时后删 semaphore：主线程若卡在 HTTP，迟到 Give 会 UAF → InstructionFetchError。
 */
static void run_on_main_sync(std::function<void()> fn)
{
    auto *gate = new MainSyncGate();
    gate->sem = xSemaphoreCreateBinary();
    if (!gate->sem) {
        delete gate;
        fn();
        return;
    }
    Application::GetInstance().Schedule([fn = std::move(fn), gate]() {
        fn();
        gate->done.store(true, std::memory_order_release);
        xSemaphoreGive(gate->sem);
    });

    TickType_t last_warn = xTaskGetTickCount();
    while (!gate->done.load(std::memory_order_acquire)) {
        if (xSemaphoreTake(gate->sem, pdMS_TO_TICKS(1000)) == pdTRUE) {
            break;
        }
        TickType_t now = xTaskGetTickCount();
        if ((now - last_warn) >= pdMS_TO_TICKS(5000)) {
            ESP_LOGW(TAG, "main sync still waiting — Application main may be blocked");
            last_warn = now;
        }
    }
    (void)xSemaphoreTake(gate->sem, 0);
    vSemaphoreDelete(gate->sem);
    delete gate;
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
    ESP_LOGI(TAG, "pwr task up (modem-sleep power save, ready for instant wake)");
    while (true) {
        if (xQueueReceive(s_pwr_q, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int newer;
        while (xQueueReceive(s_pwr_q, &newer, 0) == pdTRUE) {
            cmd = newer;
        }

        if (cmd == PWR_SLEEP) {
            ESP_LOGI(TAG, "net sleep: screen-off -> WiFi modem-sleep (keep MQTT & wake word ready)");
            /* 息屏切 Modem-sleep：WiFi 保持与 AP 关联，小智/NodeLio MQTT 均保持长连接，
             * 既大幅降低射频功耗，又确保“你好小智”随时秒开通道，首唤即连，云端推送不丢 */
            nodelio_wifi_sleep();
            continue;
        }

        ESP_LOGI(TAG, "net wake: screen-on -> restore performance");
        nodelio_wifi_wake();

        /* 若意外掉线，给短暂重连宽限 */
        if (!nodelio_wifi_has_ip()) {
            if (!nodelio_wifi_wait_connected(6000) || !nodelio_wifi_wait_ip(4000)) {
                ESP_LOGW(TAG, "WiFi reconnecting in background...");
                continue;
            }
        }

        /* 仅缺 broker 时再 register（换 server 已清 mqtt）；已有则直接连 */
        if (s_cfg.mqtt_server[0] == '\0') {
            esp_err_t reg = nodelio_device_register(&s_cfg);
            if (reg != ESP_OK) {
                ESP_LOGW(TAG, "wake register: %s — skip mqtt until provisioned", esp_err_to_name(reg));
                continue;
            }
            (void)nodelio_nvs_save(&s_cfg);
            (void)nodelio_layout_start_poll(&s_cfg);
        }
        esp_err_t mq = nodelio_mqtt_panel_start(&s_cfg);
        if (mq != ESP_OK) {
            ESP_LOGW(TAG, "mqtt: %s", esp_err_to_name(mq));
        }
        esp_err_t hb = nodelio_device_heartbeat(&s_cfg);
        if (hb != ESP_OK) {
            ESP_LOGW(TAG, "wake heartbeat: %s", esp_err_to_name(hb));
        }
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
    /* register/HTTP + mqtt 同任务：须 ≥ nl_start；4K 会 stack overflow 重启 */
    if (xTaskCreate(pwr_task, "nl_pwr", 12288, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "pwr task create fail");
    }
}

/** 仅当正在对话时才等 idle；激活后已是 idle 则立刻继续（勿再卡 800ms） */
static void wait_if_busy_talking(uint32_t timeout_ms)
{
    DeviceState st0 = Application::GetInstance().GetDeviceState();
    if (st0 == kDeviceStateIdle || st0 == kDeviceStateStarting || st0 == kDeviceStateActivating) {
        return;
    }
    const uint32_t step = 200;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        DeviceState st = Application::GetInstance().GetDeviceState();
        if (st == kDeviceStateIdle) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    ESP_LOGW(TAG, "wait idle timeout — proceed (state=%d)",
             (int)Application::GetInstance().GetDeviceState());
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
    (void)nodelio_layout_start_poll(&cfg);
    ensure_pwr_task();

    /* 勿 SetPowerSaveLevel(PERFORMANCE)：会 WakeUp + 打乱小智刚设的 LOW_POWER 时序 */
    wait_if_busy_talking(60000);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (!nodelio_wifi_has_ip() && !nodelio_wifi_wait_ip(5000)) {
        ESP_LOGW(TAG, "no IP before register — skip cloud bring-up");
        s_started = true;
        s_starting = false;
        return ESP_ERR_INVALID_STATE;
    }

    bool cloud = false;
    bool reg_ok = false;
    err = nodelio_device_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register: %s", esp_err_to_name(err));
        if (cfg.device_id[0] == '\0' || cfg.device_key[0] == '\0') {
            return err;
        }
        s_cfg = cfg;
    } else {
        (void)nodelio_nvs_save(&cfg);
        s_cfg = cfg;
        cloud = true;
        reg_ok = true;
    }

    if (nodelio_wifi_is_asleep()) {
        ESP_LOGW(TAG, "WiFi asleep during start — defer hb/mqtt/layout to net wake");
        s_cfg = cfg;
        s_started = true;
        s_starting = false;
        ensure_pwr_task();
        return ESP_OK;
    }

    if (reg_ok) {
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
        }
    } else {
        ESP_LOGW(TAG, "skip hb/mqtt/layout: register not OK — 检查绑定码/账号是否与 APP 一致");
        nodelio_mqtt_panel_stop();
    }

    s_started = true;
    s_starting = false;
    bool wifi_ok = nodelio_wifi_is_connected();
    nodelio_panel_ui_set_status(wifi_ok, cloud || nodelio_mqtt_panel_connected(), "v0");
    ESP_LOGI(TAG, "NodeLio panel up device_id=%s server=%s reg_ok=%d", cfg.device_id, cfg.server_url,
             (int)reg_ok);
    return ESP_OK;
}

static void start_task(void *arg)
{
    (void)arg;
    /* 让小智 ActivationDone（提示音/协议）先跑完，再占局域网 HTTP */
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_err_t err = start_client();
    if (err != ESP_OK) {
        s_starting = false;
        ESP_LOGW(TAG, "start_client: %s", esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

esp_err_t nodelio_bridge_start(void)
{
    if (s_started || s_starting) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nodelio_nvs_init());
    s_starting = true;
    /* 勿在 Application 主任务里阻塞 HTTP：否则息屏 Suspend 进不了主循环 → sync 超时 UAF */
    if (xTaskCreate(start_task, "nl_start", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_starting = false;
        return start_client();
    }
    return ESP_OK;
}

esp_err_t nodelio_bridge_restart(void)
{
    nodelio_bridge_stop();
    s_starting = true;
    if (xTaskCreate(start_task, "nl_start", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_starting = false;
        return start_client();
    }
    return ESP_OK;
}

void nodelio_bridge_stop(void)
{
    if (!s_started && !s_starting) {
        return;
    }
    nodelio_mqtt_panel_stop();
    s_started = false;
    s_starting = false;
}
