#include "nodelio_bridge.h"
#include "media_player.h"

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "mqtt_panel.h"
#include "nodelio_overlay.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "register.h"
#include "sdkconfig.h"
#include "wifi_board.h"
#include "wifi_provision.h"

#include <esp_heap_caps.h>
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
static bool s_hb_task;
static StackType_t *s_hb_stack;
static StaticTask_t s_hb_tcb;
static constexpr uint32_t kHbStackBytes = 10240;

/* 服务端 WALL_PANEL 离线窗口 ≥90s（再连续两轮 5s）。须小于该窗口，单次 POST 失败仍能在下一次刷 last_seen。 */
static constexpr uint32_t kHeartbeatPeriodMs = 30000;

enum { PWR_SLEEP = 0, PWR_WAKE = 1 };

struct MainSyncGate {
    SemaphoreHandle_t sem = nullptr;
    std::atomic<bool> done{false};
};

/**
 * 在主任务执行并等待完成（息屏挂起小智 MQTT 须先于断网）。
 * 禁止超时后删 semaphore：主线程若卡在 HTTP，迟到 Give 会 UAF。
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

static void hb_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kHeartbeatPeriodMs));
        if (!s_started || s_cfg.device_id[0] == '\0') {
            continue;
        }
        /* 息屏断网：无链路，跳过；唤醒路径会立刻打一枪 */
        if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
            continue;
        }
        /* 播通知/点歌时别抢 HTTP，否则 NotifyPlayer 容易 underrun */
        if (Application::GetInstance().GetDeviceState() == kDeviceStateNotifying) {
            continue;
        }
        esp_err_t hb = nodelio_device_heartbeat(&s_cfg);
        if (hb != ESP_OK) {
            ESP_LOGW(TAG, "periodic heartbeat: %s", esp_err_to_name(hb));
        }
    }
}

static void ensure_hb_task(void)
{
    if (s_hb_task) {
        return;
    }
    /* 栈放 PSRAM。内部 RAM 再占 8KB 后，语音里 mcp_http（16KB）会创建失败，控家工具直接回错。 */
    if (!s_hb_stack) {
        s_hb_stack = static_cast<StackType_t *>(
            heap_caps_malloc(kHbStackBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!s_hb_stack) {
        ESP_LOGW(TAG, "hb stack alloc fail");
        return;
    }
    if (!xTaskCreateStatic(hb_task, "nl_hb", kHbStackBytes / sizeof(StackType_t), nullptr, 4, s_hb_stack,
                           &s_hb_tcb)) {
        ESP_LOGW(TAG, "hb task create fail");
        return;
    }
    s_hb_task = true;
}

/**
 * 有 IP 时补齐 register / layout / mqtt / hb。
 * 开机首轮失败（无 IP / 服务器未起）后必须可再走这里，不能只靠重启。
 */
static void bring_up_cloud_if_needed(void)
{
    if (s_cfg.server_url[0] == '\0' || s_cfg.owner[0] == '\0') {
        return;
    }
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
        return;
    }
    const bool need_reg =
        s_cfg.mqtt_server[0] == '\0' || s_cfg.device_key[0] == '\0' || s_cfg.device_id[0] == '\0';
    if (need_reg) {
        esp_err_t reg = nodelio_device_register(&s_cfg);
        if (reg != ESP_OK) {
            ESP_LOGW(TAG, "cloud register: %s", esp_err_to_name(reg));
            return;
        }
        (void)nodelio_nvs_save(&s_cfg);
        ensure_hb_task();
    }
    if (!nodelio_layout_cloud_ready()) {
        ESP_LOGI(TAG, "cloud layout pending — start_poll");
        esp_err_t ly = nodelio_layout_start_poll(&s_cfg);
        if (ly != ESP_OK) {
            ESP_LOGW(TAG, "layout poll: %s", esp_err_to_name(ly));
        }
    }
    if (!nodelio_mqtt_panel_connected()) {
        esp_err_t mq = nodelio_mqtt_panel_start(&s_cfg);
        if (mq != ESP_OK) {
            ESP_LOGW(TAG, "mqtt: %s", esp_err_to_name(mq));
        }
    }
    esp_err_t hb = nodelio_device_heartbeat(&s_cfg);
    if (hb != ESP_OK) {
        ESP_LOGW(TAG, "cloud heartbeat: %s", esp_err_to_name(hb));
    }
    bool wifi_ok = nodelio_wifi_is_connected();
    nodelio_panel_ui_set_status(wifi_ok, nodelio_layout_cloud_ready() || nodelio_mqtt_panel_connected(),
                                "v0");
}

static bool s_deferred_cloud;
static void deferred_cloud_task(void *arg)
{
    (void)arg;
    uint32_t delay_ms = 2000;
    for (int i = 0; i < 48; i++) { /* ~15min 内退避重试 */
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (nodelio_layout_cloud_ready() && nodelio_mqtt_panel_connected()) {
            ESP_LOGI(TAG, "deferred cloud done");
            break;
        }
        if (nodelio_wifi_is_asleep()) {
            continue;
        }
        if (!nodelio_wifi_has_ip() && !nodelio_wifi_wait_ip(15000)) {
            ESP_LOGW(TAG, "deferred cloud: still no IP (try %d)", i + 1);
            if (delay_ms < 20000) {
                delay_ms += 2000;
            }
            continue;
        }
        ESP_LOGI(TAG, "deferred cloud bring-up try %d", i + 1);
        bring_up_cloud_if_needed();
        if (nodelio_layout_cloud_ready()) {
            Application::GetInstance().Schedule([]() { nodelio_panel_ui_skills_changed(); });
            break;
        }
        if (delay_ms < 20000) {
            delay_ms += 2000;
        }
    }
    s_deferred_cloud = false;
    vTaskDelete(nullptr);
}

static void ensure_deferred_cloud(void)
{
    if (s_deferred_cloud || nodelio_layout_cloud_ready()) {
        return;
    }
    s_deferred_cloud = true;
    /* register + layout HTTP，栈须 ≥ nl_start / pwr */
    if (xTaskCreate(deferred_cloud_task, "nl_cloud", 12288, nullptr, 4, nullptr) != pdPASS) {
        s_deferred_cloud = false;
        ESP_LOGW(TAG, "deferred cloud task create fail");
    }
}

static void pwr_task(void *arg)
{
    (void)arg;
    int cmd = PWR_WAKE;
    ESP_LOGI(TAG, "pwr task up (net sleep → WiFi/MQTT off)");
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
            /* 先挡重连，再主任务挂起协议，最后停 STA（息屏超省电，无云端流量） */
            Application::GetInstance().MarkTransportSuspended(true);
            run_on_main_sync([]() { Application::GetInstance().SuspendTransport(); });
            nodelio_mqtt_panel_stop();
            nodelio_wifi_sleep();
            continue;
        }

        ESP_LOGI(TAG, "net wake: wifi + mqtt + heartbeat");
        nodelio_wifi_wake();

        /* 等 Got IP（含关联时间）；勿先 wait L2 再短等 DHCP，易在重认证窗口耗尽 */
        if (!nodelio_wifi_wait_ip(30000)) {
            ESP_LOGW(TAG, "WiFi not up — skip hb/mqtt");
            ensure_deferred_cloud();
            continue;
        }

        /* 异步恢复小智通道：勿 run_on_main_sync，否则主任务卡在首唤 wait/OpenAudio 时会死锁 */
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().ResumeTransport();
            /* 息屏期间跳过的钉选拉取，有 IP 后再补 */
            nodelio_panel_ui_skills_changed();
        });

        bring_up_cloud_if_needed();
        if (!nodelio_layout_cloud_ready()) {
            ensure_deferred_cloud();
        }
    }
}

static void on_net(bool on)
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
    /* 断网与息屏时刻可不同；仅挂 net_cb */
    nodelio_panel_ui_set_net_cb(on_net);
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
        ESP_LOGW(TAG, "未绑定：进 SoftAP 配服务器（亦可屏上设置 / 语音「进入配网」）");
        (void)nodelio_overlay_init();
        /* 未绑定也须开息屏计时，禁止常亮 */
        Board::GetInstance().SetIdleSleepSeconds((int)nodelio_idle_sleep_seconds());
        Board::GetInstance().SetIdleWifiSeconds((int)nodelio_idle_wifi_seconds());
        Board::GetInstance().SetPowerSaveEnabled(nodelio_power_save_enabled());
        s_started = true;
        s_starting = false;
        Application::GetInstance().Schedule([]() {
            auto &board = static_cast<WifiBoard &>(Board::GetInstance());
            if (!board.IsInWifiConfigMode()) {
                board.EnterWifiConfigMode();
            }
        });
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg = cfg;
    Board::GetInstance().SetIdleSleepSeconds((int)nodelio_idle_sleep_seconds());
    Board::GetInstance().SetIdleWifiSeconds((int)nodelio_idle_wifi_seconds());
    Board::GetInstance().SetPowerSaveEnabled(nodelio_power_save_enabled());
    (void)nodelio_panel_ui_start(&cfg); /* overlay 已可出缓存布局 */
    (void)nodelio_media_init();
    ensure_pwr_task();

    /* 勿 SetPowerSaveLevel(PERFORMANCE)：会 WakeUp + 打乱小智刚设的 LOW_POWER 时序 */
    wait_if_busy_talking(60000);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    if (!nodelio_wifi_has_ip() && !nodelio_wifi_wait_ip(8000)) {
        ESP_LOGW(TAG, "no IP before register — defer cloud bring-up");
        s_cfg = cfg;
        s_started = true;
        s_starting = false;
        ensure_pwr_task();
        ensure_deferred_cloud(); /* DHCP/服务器稍后就绪时补 register+layout，勿卡死「初始化中」 */
        return ESP_ERR_INVALID_STATE;
    }

    bool cloud = false;
    err = nodelio_device_register(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "register: %s — keep local UI, defer cloud retry",
                 esp_err_to_name(err));
        s_cfg = cfg;
        s_started = true;
        s_starting = false;
        nodelio_mqtt_panel_stop();
        bool wifi_ok = nodelio_wifi_is_connected();
        nodelio_panel_ui_set_status(wifi_ok, false, "v0");
        ensure_deferred_cloud();
        return err;
    }
    (void)nodelio_nvs_save(&cfg);
    s_cfg = cfg;
    cloud = true;
    ensure_hb_task();

    if (nodelio_wifi_is_asleep()) {
        ESP_LOGW(TAG, "WiFi asleep during start — defer hb/mqtt/layout to net wake");
        s_cfg = cfg;
        s_started = true;
        s_starting = false;
        ensure_pwr_task();
        return ESP_OK;
    }

    err = nodelio_device_heartbeat(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heartbeat: %s", esp_err_to_name(err));
    } else {
        cloud = true;
    }
    /* register 后再拉 layout+snapshot（一次），再开 MQTT，避免并行竞态与 notify 连打 */
    nodelio_log_heap("before layout");
    err = nodelio_layout_start_poll(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "layout poll: %s", esp_err_to_name(err));
    }
    nodelio_log_heap("before panel mqtt");
    err = ESP_FAIL;
    for (int i = 0; i < 3; i++) {
        err = nodelio_mqtt_panel_start(&cfg);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "mqtt panel try %d/3: %s", i + 1, esp_err_to_name(err));
        nodelio_log_heap("panel mqtt fail");
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    s_started = true;
    s_starting = false;
    bool wifi_ok = nodelio_wifi_is_connected();
    cloud = cloud || nodelio_layout_cloud_ready() || nodelio_mqtt_panel_connected();
    nodelio_panel_ui_set_status(wifi_ok, cloud, "v0");
    if (!nodelio_layout_cloud_ready()) {
        ESP_LOGW(TAG, "boot cloud layout not ready — defer retry");
        ensure_deferred_cloud();
    }
    nodelio_log_heap("panel up");
    ESP_LOGI(TAG, "NodeLio panel up device_id=%s server=%s reg_ok=1 mqtt=%s cloud=%d", cfg.device_id,
             cfg.server_url, nodelio_mqtt_panel_connected() ? "up" : "down",
             nodelio_layout_cloud_ready() ? 1 : 0);
    return ESP_OK;
}

static void start_task(void *arg)
{
    (void)arg;
    /* 局域网离线优先：WiFi 连通后无需等待小智激活，立即发起内网 HTTP 注册与 MQTT 启动 */
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_err_t err = start_client();
    if (err != ESP_OK) {
        s_starting = false;
        ESP_LOGW(TAG, "start_client: %s", esp_err_to_name(err));
    }
    vTaskDelete(nullptr);
}

bool nodelio_bridge_wait_ready(uint32_t timeout_ms)
{
    const uint32_t step = 100;
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (s_started && !s_starting) {
            ESP_LOGI(TAG, "bridge ready after %ums", (unsigned)waited);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
    }
    ESP_LOGW(TAG, "bridge wait timeout %ums started=%d starting=%d", (unsigned)timeout_ms,
             s_started ? 1 : 0, s_starting ? 1 : 0);
    return s_started && !s_starting;
}

esp_err_t nodelio_bridge_start(void)
{
    if (s_started || s_starting) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(nodelio_nvs_init());
    s_starting = true;
    nodelio_log_heap("bridge start");
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

bool nodelio_voice_session_active(void)
{
    DeviceState st = Application::GetInstance().GetDeviceState();
    return st == kDeviceStateConnecting || st == kDeviceStateListening ||
           st == kDeviceStateSpeaking || st == kDeviceStateNotifying;
}
