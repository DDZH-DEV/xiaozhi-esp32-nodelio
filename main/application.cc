#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
#include "diol_lan.h"
#include "media_player.h"
#include "nl_time.h"
#include "nodelio_bridge.h"
#include "nodelio_mcp.h"
#include "nodelio_overlay.h"
#include "nvs_config.h"
#include "offline_cmd.h"
#include "offline_cmd_map.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
#include "wake_dram.h"
#include "wifi_provision.h"
#endif

#include <atomic>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#define TAG "Application"

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
extern "C" srmodel_list_t* nodelio_get_srmodels(void) {
    return Application::GetInstance().GetAudioService().GetModelsList();
}
static bool tts_looks_like_app_deny(const char *text);
/** 新闻 MCP 旧指令壳 / 【标】被模型念出时丢掉字幕；正文匹配不受影响。 */
static bool tts_looks_like_news_instruction(const char *text);
#endif

Application::Application() : notify_player_(audio_service_) {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    notify_player_.Stop();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 立刻建 NodeLio 壳，勿 Schedule：否则会闪一帧小智白顶+WiFi 栏 */
    (void)nodelio_overlay_init();
    nodelio_overlay_show(false);
#else
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());
#endif

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 先注册再 AddCommonTools：common 置顶（prompt cache），NodeLio 紧随其后，
     * 避免排在 UserOnly 后面被 tools/list 8KB 截断漏掉控家工具。 */
    nodelio_mcp_register_tools();
    nodelio_offline_cmd_set_done_cb([]() {
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            app.GetAudioService().EnableLocalCommandMode(false);
            const DeviceState st = app.GetDeviceState();
            if (st == kDeviceStateConnecting) {
                app.SetDeviceState(kDeviceStateIdle);
            } else if (st == kDeviceStateIdle || st == kDeviceStateNotifying) {
                /* 进本地口令时关过唤醒词；若已在 idle/播歌，须自己再打开 */
                app.GetAudioService().EnableWakeWordDetection(true);
            }
        });
    });
#endif
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    // Opening an installed app and reading it must both be visible on the first page.
    mcp_server.PromoteTool("self.nodelio.open_app");
    mcp_server.PromoteTool("self.skill.news");
#endif

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
#if !(CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL)
                /* 面板息屏断网再唤醒时，重连扫描会误置 DISCONNECTED，把刚建的会话掐掉 */
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
#endif
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* Assets：加载 WakeNet 等；MultiNet 已从产品拿掉（省 ~4MB PSRAM），不再 prewarm。
     * 勿 refresh_display_theme：此时 NodeLio 首页已渲染，SetTheme 会整屏脏刷闪白。 */
    {
        auto& assets = Assets::GetInstance();
        const int64_t t0 = esp_timer_get_time();
        nodelio_log_heap("boot assets begin");
        ESP_LOGI(TAG, "boot: assets apply begin part=%d", assets.partition_valid() ? 1 : 0);
        const bool applied = assets.partition_valid() && assets.Apply(false);
        ESP_LOGI(TAG, "boot: assets apply %s %ld ms", applied ? "ok" : "fail",
                 (long)((esp_timer_get_time() - t0) / 1000));
        nodelio_log_heap("boot assets done");
        (void)applied;
    }
#endif

    // Start network asynchronously
    ESP_LOGI(TAG, "boot: start network");
    nodelio_log_heap("boot start network");
    board.StartNetwork();

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 墙板开机跳过小智 OTA（其 server_time 曾是唯一时间源），改由 SNTP 补；
     * 须在 StartNetwork 之后：此时 netif/事件循环已就绪，GOT_IP 处理器能挂上。 */
    (void)nodelio_time_begin();
#endif

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 息屏播歌时 MQTT TLS 抢 DRAM 失败会报「发送失败」；勿因此掐 HTTP 流。 */
            if (nodelio_media_is_playing() || notify_player_.IsBusy()) {
                ESP_LOGW(TAG, "network error during media, keep playing: %s",
                         last_error_message_.c_str());
                last_error_message_.clear();
            } else
#endif
            {
                if (GetDeviceState() == kDeviceStateNotifying) {
                    StopNotification();
                }
                SetDeviceState(kDeviceStateIdle);
                Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                      Lang::Sounds::OGG_EXCLAMATION);
            }
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            /* 同轮已 Connected（唤醒重连：先 Scan 置位再 Got IP）→ 勿关刚开的音频会话 */
            if (!(bits & MAIN_EVENT_NETWORK_CONNECTED)) {
                HandleNetworkDisconnectedEvent();
            }
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            FinishNewsReadPlayback();
#endif
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                /* 仅在「开始说话」边沿清零；已提示退下后勿清，否则环境噪声会永远关不掉抽屉 */
                if (!listen_idle_pending_close_ && audio_service_.IsVoiceDetected()) {
                    listen_idle_ticks_ = 0;
                }
#endif
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            CheckNewsReadRequest();
            FinishNewsReadPlayback();
            HandleListenIdleTick();
#endif

            // Print debug info every 10 seconds（息屏跳过，少打 UART）
            if (clock_ticks_ % 10 == 0) {
                auto* bl = Board::GetInstance().GetBacklight();
                if (!bl || bl->brightness() > 0) {
                    SystemInfo::PrintHeapStats();
                }
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 有 IP 立刻起局域网 bridge，勿等小智 OTA/MQTT（否则首页再卡数秒） */
    Schedule([]() { (void)nodelio_bridge_start(); });
#endif

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 扫描/抖动误报：STA 已有关联则不当真断网（息屏唤醒首唤会被掐） */
    if (nodelio_wifi_is_connected()) {
        ESP_LOGW(TAG, "ignore network-disconnect while WiFi still associated");
        auto display = Board::GetInstance().GetDisplay();
        display->UpdateStatusBar(true);
        return;
    }
#endif
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateNotifying) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        /* 真断网后 HTTP 会自己超时；MQTT 抖动勿立刻掐歌 */
        if (!(nodelio_media_is_playing() || notify_player_.IsBusy())) {
            StopNotification();
        }
#else
        StopNotification();
#endif
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 墙板跳过开机 OTA，server_time 不再来自 CheckVersion；SNTP 异步补上，见 nl_time.c */
    has_server_time_ = nodelio_time_valid();
#else
    has_server_time_ = ota_->HasServerTime();
#endif

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty();
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    if (!has_error) {
        Schedule([this]() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 墙板：激活完成瞬间正开 AFE/开麦，PlaySound 会先开 TX 再被 Input 重配 I2S，
             * 易导致后续云端 TTS 全程无声。就绪提示改靠 UI，不播 OGG。 */
            (void)this;
#else
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
#endif
        });
    }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* bridge 已在 NetworkConnected 启动；此处幂等补一次（防漏） */
    Schedule([]() { (void)nodelio_bridge_start(); });
#endif
}

void Application::ActivationTask() {
    ESP_LOGI(TAG, "boot: activation begin");
    nodelio_log_heap("activation begin");
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* AFE 创建前占 12KB 片内坑；播中武装唤醒时再释放，减轻碎片导致 largest 过小 */
    (void)nodelio_wake_dram_reserve();
#endif
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /*
     * 墙板开机不做 OTA 检查（省电/省时）。设置页「检查更新」手工触发
     * RequestManualFirmwareCheck：拉 http://z-y.site/works/esp32s3-slia-86-update.json，
     * 用构建号 YYYYMMDD.NNN 比较后按清单 url 下载 xiaozhi.bin。
     *
     * 但 MarkCurrentVersionValid 不能一起跳过：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
     * 时，刚 OTA 上来的镜像处于 PENDING_VERIFY，不确认就会在下次重启回滚。
     */
    ota_->MarkCurrentVersionValid();
    ESP_LOGI(TAG, "skip xiaozhi OTA check on boot (manual check in settings)");
#else
    // Check for new firmware version
    CheckNewVersion();
#endif

    // Initialize the protocol
    ESP_LOGI(TAG, "boot: protocol start");
    nodelio_log_heap("protocol start");
    InitializeProtocol();

    // Signal completion to main loop
    ESP_LOGI(TAG, "boot: activation done");
    nodelio_log_heap("activation done");
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 开机已 Apply(false) 过；无新 assets 下载时禁止再 SetTheme（首页会闪）。
     * 刚下载完则只装模型/字库，仍不刷小智 chrome 主题。 */
    if (!download_url.empty()) {
        assets.Apply(false);
    }
    display->SetChatMessage("system", "");
#else
    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
#endif
}

void Application::CheckNewVersion() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 墙板：少重试、短退避；云端挂了也不能把 Activating 拖成一晚 */
    const int MAX_RETRY = 3;
    int retry_delay = 5;
#else
    const int MAX_RETRY = 10;
    int retry_delay = 10;  // Initial retry delay in seconds
#endif
    int retry_count = 0;

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        retry_delay = 5;
#else
        retry_delay = 10;  // Reset retry delay
#endif

        if (ota_->HasNewVersion()) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 墙板：开机不自动刷固件，改由设置页手工检查 */
            ESP_LOGI(TAG, "New firmware %s available — skip auto OTA (use settings)",
                     ota_->GetFirmwareVersion().c_str());
#else
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
#endif
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        bool activated = false;
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                activated = true;
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        /* 墙板：一轮激活失败就继续开机，禁止 while(true) 整夜占 Activating 挡息屏 */
        if (!activated) {
            ESP_LOGW(TAG, "activation incomplete — continue panel boot (power save)");
            break;
        }
#else
        (void)activated;
#endif
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /*
     * 墙板跳过开机 CheckVersion，ota_->HasMqttConfig()/HasWebsocketConfig() 恒为 false，
     * 继续用它们选协议会一律落到「默认 MQTT」。这两个配置本来就落在 NVS（mqtt/websocket
     * 命名空间），直接读 NVS 判断，语义与 CheckVersion 解析后写入的一致。
     */
    const bool has_mqtt_cfg = [] {
        Settings s("mqtt", false);
        return !s.GetString("endpoint").empty();
    }();
    const bool has_ws_cfg = [] {
        Settings s("websocket", false);
        return !s.GetString("url").empty();
    }();
    if (has_mqtt_cfg) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (has_ws_cfg) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol in NVS, using MQTT (manual fw check fills it)");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#else
    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }
#endif

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        if (news_awaiting_mcp_ || IsCloudTtsSuppressed()) {
            return;
        }
        /* 墙板主循环常被 snapshot/布局占住：UDP 比 MQTT tts-start 更早到。
         * 只收 Speaking 会把整段问候丢掉，屏上有字、喇叭没声。 */
        const DeviceState st = GetDeviceState();
        if (st == kDeviceStateSpeaking || st == kDeviceStateListening ||
            st == kDeviceStateConnecting) {
            static uint32_t s_rx;
            const uint32_t n = ++s_rx;
            if (n == 1 || (n % 40) == 0) {
                ESP_LOGI(TAG, "cloud audio rx #%u state=%d bytes=%u rate=%d", (unsigned)n,
                         (int)st, packet ? (unsigned)packet->payload.size() : 0,
                         packet ? packet->sample_rate : 0);
            }
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
                ESP_LOGW(TAG, "drop cloud audio: decode queue full/reset state=%d rx=%u", (int)st,
                         (unsigned)n);
            } else {
                cloud_audio_rx_count_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            ESP_LOGW(TAG, "drop cloud audio: bad state=%d", (int)st);
        }
#else
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
#endif
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        /* 先回 idle 再降功耗：避免 goodbye 瞬间 MAX_MODEM 把尾包 UDP 打丢 */
        Schedule([this, &board]() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            CancelNewsReadPlayback();
            news_awaiting_mcp_ = false;
            news_reading_ = false;
            nodelio_skill_news_sync_read_btn();
            news_detect_echo_.clear();
            pending_confirm_read_news_ = false;
#endif
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 歌已经在播时不要被通道关闭打回 Idle，否则唤醒词重开声卡把流掐掉 */
            if (GetDeviceState() != kDeviceStateNotifying) {
                SetDeviceState(kDeviceStateIdle);
            }
#else
            SetDeviceState(kDeviceStateIdle);
#endif
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 听歌 / 本地播报：勿 MAX_MODEM，也勿 goodbye 短息屏。
             * 技能卡仍开 ≠ 用户活动：息屏后省电断 MQTT 会进本回调，若 NoteActivity/
             * PERFORMANCE→WakeUp 会「刚灭又亮」。技能卡只跳过 goodbye，不重置 idle。 */
            const bool media_busy = nodelio_media_is_playing() || nodelio_media_resume_after_voice() ||
                                    notify_player_.IsBusy() || GetDeviceState() == kDeviceStateNotifying ||
                                    !pending_spoken_notify_url_.empty();
            const bool skill_up = nodelio_skill_result_is_visible();
            const bool asleep = board.IsInSleepMode();
            if (media_busy) {
                board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                if (!asleep) {
                    board.NoteUserActivity();
                }
            } else if (skill_up) {
                /* 技能卡残留：不 PERFORMANCE/NoteActivity（否则 MQTT 撕建链反复推迟息屏）；
                 * 也不 RequestGoodbyeSleep，交给空闲计时自然灭屏。 */
                ESP_LOGI(TAG, "audio channel closed skill_up asleep=%d — skip wake/goodbye",
                         asleep ? 1 : 0);
            } else {
                board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                board.RequestGoodbyeSleep();
            }
#else
            board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
#endif
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                    if (news_awaiting_mcp_) {
                        // Acknowledgements/denials are not a news tool result. Do not abort
                        // here: the server may still be dispatching the requested tool.
                        return;
                    }
                    if (IsCloudTtsSuppressed()) {
                        ESP_LOGI(TAG, "drop cloud tts start (suppressed remain_ms=%lld)",
                                 (long long)((suppress_cloud_tts_until_us_ - esp_timer_get_time()) /
                                             1000));
                        aborted_ = true;
                        if (protocol_) {
                            protocol_->SendAbortSpeaking(kAbortReasonNone);
                        }
                        if (GetDeviceState() == kDeviceStateSpeaking) {
                            SetDeviceState(kDeviceStateListening);
                        }
                        /* abort 后云端停收上行：须重新 listen.start，否则连说假死 */
                        ContinueListeningAfterLocalSkill();
                        return;
                    }
                    ESP_LOGI(TAG, "cloud tts start (pending_dsh=%d)",
                             pending_confirm_open_dsh_ ? 1 : 0);
                    news_tts_stopped_ = false;
#endif
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                    if (news_playback_token_ != 0 && !aborted_) {
                        news_tts_stopped_ = true;
                        FinishNewsReadPlayback();
                        return;
                    }
#endif
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                    // Only tool dispatch releases the read gate; article words are irrelevant.
                    if (news_awaiting_mcp_ || IsCloudTtsSuppressed()) {
                        ESP_LOGI(TAG, "drop cloud tts sentence: %s", text->valuestring);
                        return;
                    }
                    /* 指令壳勿上屏/跟读；音频若已编码仍可能出声，靠 MCP 回包去壳根治 */
                    if (nodelio_skill_news_ui_active() &&
                        tts_looks_like_news_instruction(text->valuestring)) {
                        ESP_LOGI(TAG, "drop news instruction tts: %s", text->valuestring);
                        return;
                    }
                    /* 否认句：本地已开卡则只丢弃；DSH 未开则本地补开 */
                    if (!nodelio_skill_news_ui_active() &&
                        tts_looks_like_app_deny(text->valuestring)) {
                        if (dsh_local_open_cooldown_until_us_ > 0 &&
                            esp_timer_get_time() < dsh_local_open_cooldown_until_us_) {
                            ESP_LOGI(TAG, "drop cloud deny (local app already open): %s",
                                     text->valuestring);
                            SuppressCloudTts(2000);
                            return;
                        }
                        /* 仅任务/DSH 否认才补开；新闻等勿误开任务 */
                        const char *t = text->valuestring;
                        if (strstr(t, "DSH") || strstr(t, "任务")) {
                            ESP_LOGW(TAG, "cloud deny dsh → suppress+local open: %s", t);
                            SuppressCloudTts(3000);
                            Schedule([this]() {
                                pending_confirm_open_dsh_ = false;
                                nodelio_panel_ui_open_app("查看任务");
                            });
                            return;
                        }
                        ESP_LOGI(TAG, "drop cloud deny (no local reopen): %s", t);
                        SuppressCloudTts(2000);
                        return;
                    }
#endif
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                        if (!nodelio_skill_news_ui_active()) {
                            MaybeArmPendingOpenDshFromTts(message.c_str());
                        }
                        if (IsCloudTtsSuppressed()) {
                            ESP_LOGI(TAG, "drop cloud tts sentence after local open: %s",
                                     message.c_str());
                            return;
                        }
                        if (nodelio_skill_news_ui_active() &&
                            tts_looks_like_news_instruction(message.c_str())) {
                            ESP_LOGI(TAG, "drop news instruction tts (ui): %s", message.c_str());
                            return;
                        }
                        OnNewsReadSentence(message.c_str());
#endif
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                    /* 新一轮识别先清抑制；本地命中后再 SuppressCloudTts。
                     * 勿清 pending_confirm_open_dsh_（要留给「是的」）。 */
                    ESP_LOGI(TAG,
                             "stt local-route begin phrase='%s' pending_dsh=%d suppress_was=%d",
                             message.c_str(), pending_confirm_open_dsh_ ? 1 : 0,
                             IsCloudTtsSuppressed() ? 1 : 0);
                    /* 本地 detect 回声：勿再本地拦截，交给云端调 MCP */
                    if (strncmp(message.c_str(), "请向用户简短播报", sizeof("请向用户简短播报") - 1) ==
                        0) {
                        ESP_LOGI(TAG, "stt local-route: skip self cloud-speak prompt");
                        return;
                    }
                    if (!news_detect_echo_.empty() &&
                        esp_timer_get_time() < news_mcp_deadline_us_ &&
                        message == news_detect_echo_) {
                        news_detect_echo_.clear();
                        ESP_LOGI(TAG, "stt local-route: skip news MCP detect echo");
                        return;
                    }
                    // A real user turn supersedes the previous read request, including its retry.
                    CancelNewsReadPlayback();
                    news_awaiting_mcp_ = false;
                    news_detect_echo_.clear();
                    suppress_cloud_tts_until_us_ = 0;
                    /* 派活正文可能含「第一个」「重启」；不能被面板导航/新闻确认抢走。 */
                    if (nodelio_offline_try_dsh_phrase(message.c_str())) {
                        pending_confirm_open_dsh_ = false;
                        pending_confirm_read_news_ = false;
                        SuppressCloudTts(5000);
                        ContinueListeningAfterLocalSkill();
                        return;
                    }
                    if (TryConfirmOpenDshFromStt(message.c_str())) {
                        ESP_LOGI(TAG, "stt local-route: confirm→dsh.status");
                        return;
                    }
                    /* 新闻「第 N 条」须先于 DSH/open_app，否则被当成应用名 */
                    {
                        bool news_keep = false;
                        if (nodelio_skill_try_news_pick_phrase(message.c_str(), &news_keep)) {
                            pending_confirm_read_news_ = false;
                            ESP_LOGI(TAG, "stt local-route: news pick keep_cloud=%d",
                                     news_keep ? 1 : 0);
                            if (!news_keep) {
                                SuppressCloudTts(3000);
                                ContinueListeningAfterLocalSkill();
                            }
                            return;
                        }
                    }
                    if (TryConfirmReadNewsFromStt(message.c_str())) {
                        ESP_LOGI(TAG, "stt local-route: confirm→news read");
                        return;
                    }
                    /* DSH 卡选条须先于 open_app：「打开第一个」否则被当成应用名并 suppress 云端 */
                    if (nodelio_skill_try_pick_phrase(message.c_str())) {
                        ESP_LOGI(TAG, "stt local-route: dsh pick");
                        SuppressCloudTts(3000);
                        ContinueListeningAfterLocalSkill();
                        return;
                    }
                    /* 本机口令旁路：信息卡保持会话；媒体/重启/配网结束语音 */
                    bool end_session = false;
                    bool keep_cloud_tts = false;
                    if (nodelio_offline_try_system_phrase(message.c_str(), &end_session,
                                                          &keep_cloud_tts)) {
                        ESP_LOGI(TAG,
                                 "stt local-route: system/open_app hit end_session=%d keep_cloud=%d",
                                 end_session ? 1 : 0, keep_cloud_tts ? 1 : 0);
                        if (end_session) {
                            /* 关播放器/开曲库等：退会话；冷却期内丢弃云端「找不到应用」 */
                            dsh_local_open_cooldown_until_us_ =
                                esp_timer_get_time() + (int64_t)20000 * 1000;
                            EndVoiceSessionAfterLocalSkill();
                        } else if (keep_cloud_tts) {
                            /* 问天气/空气：本地开卡，勿 Suppress/重开听（会 ResetDecoder 清掉云端 Opus） */
                        } else {
                            /* 设置导航/开关/翻页/开新闻追问：保持 Listening；丢弃「找不到应用」 */
                            SuppressCloudTts(5000);
                            dsh_local_open_cooldown_until_us_ =
                                esp_timer_get_time() + (int64_t)20000 * 1000;
                            ContinueListeningAfterLocalSkill();
                        }
                        return;
                    }
                    /* 关闭电台/播客/曲库…：本地先收弹窗。云端常跟「拜拜」并结束会话；
                     * 若只 suppress 仍停 Listening，唤醒词不开、上行也不再出字 → 假死。 */
                    if (nodelio_skill_try_dismiss_phrase(message.c_str())) {
                        ESP_LOGI(TAG, "stt local-route: skill dismiss → idle");
                        EndVoiceSessionAfterLocalSkill();
                        return;
                    }
                    ESP_LOGI(TAG, "stt local-route: miss → cloud continues");
#endif
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    Schedule(
                        [this, display, payload_str = std::string(payload_json.get())]() {
                            display->SetChatMessage("system", payload_str.c_str());
                        });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (!nodelio_xiaozhi_enabled()) {
        ESP_LOGI(TAG, "Xiaozhi disabled — skip cloud MQTT start (local cmd only)");
        protocol_->MarkTransportSuspended(true);
        /* 无小智 TLS：面板 MQTT 由 bridge 自起，此处放行 DIOL */
        nodelio_diol_boot_quiet_end();
    } else {
        /* 先等局域网 bridge（注册/layout），再起小智 TLS MQTT，最后起面板明文 MQTT。
         * DIOL 探测默认 hold，避免与 mbedtls SPIRAM 握手并行砸堆。
         * wait 超时仍 starting 时绝不能立刻 TLS——会与 bootstrap HTTP 抢片内堆，
         * 常见 mbedtls_ssl_setup 失败（api.tenclass.net:8883）。 */
        (void)nodelio_bridge_start();
        ESP_LOGI(TAG, "boot: wait NodeLio bridge before Xiaozhi MQTT");
        nodelio_log_heap("before bridge wait");
        if (!nodelio_bridge_wait_ready(20000)) {
            ESP_LOGW(TAG, "bridge still busy — extend wait before Xiaozhi TLS");
            nodelio_log_heap("bridge wait extend");
            (void)nodelio_bridge_wait_ready(45000);
        }
        nodelio_diol_wait_idle(2000);
        nodelio_log_heap("after bridge wait");
        protocol_->Start();
        nodelio_log_heap("after xz mqtt");
        nodelio_bridge_start_panel_mqtt();
        nodelio_diol_boot_quiet_end();
    }
#else
    protocol_->Start();
#endif
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        if (nodelio_media_is_playing()) {
            nodelio_media_pause_for_voice();
            nodelio_offline_cmd_yield_for_cloud_audio();
            audio_service_.EnableVoiceProcessing(false);
        } else {
            StopNotification();
        }
#else
        StopNotification();
#endif
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        /* 点「进入对话」：进聆听后再响提示音（须在 ResetDecoder 之后，见 StartListeningAudio） */
        play_popup_on_listening_ = true;
#endif
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
/** 息屏断网后首唤：先起 STA、解除 transport suspend，再 OpenAudioChannel */
static void EnsureNetReadyAfterScreenSleep()
{
    auto display = Board::GetInstance().GetDisplay();
    if (nodelio_wifi_is_asleep()) {
        ESP_LOGI(TAG, "screen-off net sleep — wake WiFi for voice");
        if (display) {
            display->SetStatus("唤醒网络…");
        }
        nodelio_wifi_wake();
    }
    if (!nodelio_wifi_has_ip()) {
        ESP_LOGI(TAG, "waiting WiFi/IP before cloud voice...");
        /* 短等即可：DHCP 卡死时勿占满口令窗；后台 WifiBoard 仍会继续拿 IP */
        static const char *k_steps[] = {"入网中…", "获取地址…", "接通语音…"};
        const uint32_t timeout_ms = 5000;
        const uint32_t step_ms = 100;
        const uint32_t tip_every_ms = 450;
        uint32_t waited = 0;
        int tip_i = 0;
        if (display) {
            display->SetStatus(k_steps[0]);
        }
        while (waited < timeout_ms) {
            if (nodelio_wifi_has_ip()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(step_ms));
            waited += step_ms;
            if (display && tip_every_ms > 0 && (waited % tip_every_ms) < step_ms) {
                tip_i = (tip_i + 1) % 3;
                display->SetStatus(k_steps[tip_i]);
            }
        }
        if (!nodelio_wifi_has_ip()) {
            ESP_LOGW(TAG, "WiFi not ready in %ums — skip cloud voice",
                     (unsigned)timeout_ms);
            if (display) {
                display->SetStatus("网络未就绪");
            }
            /* 清 suspended，避免后续 StartMqttClient skipped (suspended) 永久卡死 */
            Application::GetInstance().MarkTransportSuspended(false);
            return;
        }
    }
    if (display) {
        display->SetStatus("接通语音…");
    }
    if (nodelio_xiaozhi_enabled()) {
        Application::GetInstance().ResumeTransport();
    }
}
#endif

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (!nodelio_xiaozhi_enabled()) {
        ESP_LOGI(TAG, "Xiaozhi disabled — skip cloud listen");
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    EnsureNetReadyAfterScreenSleep();
    if (!nodelio_wifi_has_ip()) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
#endif

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        if (nodelio_media_is_playing()) {
            nodelio_media_pause_for_voice();
            nodelio_offline_cmd_yield_for_cloud_audio();
            audio_service_.EnableVoiceProcessing(false);
        } else {
            StopNotification();
        }
#else
        StopNotification();
#endif
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        if (nodelio_media_is_playing()) {
            nodelio_media_pause_for_voice();
            /* StopNotification 会 EnableVoiceProcessing；播歌后内部堆碎，MQTT hello/首包 TTS
             * 易 errno=12 → 听感「第一次没声」。Listening 再开处理。 */
            nodelio_offline_cmd_yield_for_cloud_audio();
            audio_service_.EnableVoiceProcessing(false);
        } else {
            StopNotification();
        }
#else
        StopNotification();
#endif
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 不编唤醒 Opus：33 包编码把内部最大块打到 ~5KB，随后 MQTT hello 的 esp-aes OOM 拆线。
     * 云端仍收 type=listen + wake word 文本。 */
    const bool skip_wake_encode = true;
#else
    const bool skip_wake_encode = false;
#endif
    if (skip_wake_encode) {
        audio_service_.DiscardWakeWordAudio();
    } else {
        audio_service_.EncodeWakeWord();
    }

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (!nodelio_xiaozhi_enabled()) {
        ESP_LOGI(TAG, "Xiaozhi disabled — skip cloud wake");
        SetDeviceState(kDeviceStateIdle);
        return;
    }
    /* 息屏断网后首唤：StartMqttClient skipped (suspended) → 必败 */
    EnsureNetReadyAfterScreenSleep();
    if (!nodelio_wifi_has_ip()) {
        SetDeviceState(kDeviceStateIdle);
        return;
    }
#endif

    if (!protocol_->IsAudioChannelOpened()) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        /* 唤醒坑专供播中武装；云对话 UDP/AES 更吃连续块，进会话前必须释放 */
        (void)nodelio_wake_dram_release_for_arm();
        nodelio_offline_cmd_yield_for_cloud_audio();
        ESP_LOGI(TAG, "wake dram before hello largest=%u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif
        if (!protocol_->OpenAudioChannel()) {
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 不发唤醒 Opus、不播本地叮（DRAM/解码器）。
     * 必须先 SetListeningMode（内部 ResetDecoder），再 SendWakeWordDetected：
     * 若先 detect，问候 UDP 会入队后被 Reset 清光 → 屏上有字喇叭无声。 */
    SetListeningMode(GetDefaultListeningMode());
    protocol_->SendWakeWordDetected(wake_word);
#elif CONFIG_SEND_WAKE_WORD_DATA
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    listen_idle_ticks_ = 0;
    listen_announce_hold_ticks_ = 0;
    listen_idle_pending_close_ = false;
#endif
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
                /* 有待播摘要时勿清对话，否则上屏 spoken 被立刻抹掉 */
                if (pending_spoken_notify_url_.empty()) {
                    display->ClearChatMessages();
                }
#else
                display->ClearChatMessages();  // Clear messages first
#endif
                display->SetEmotion(
                    "neutral");  // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 云会话结束：无播歌时重新占唤醒坑，供下次点歌武装 */
            if (!nodelio_media_is_playing() && !nodelio_media_is_paused()) {
                (void)nodelio_wake_dram_try_reclaim();
            }
            if (!pending_spoken_notify_url_.empty()) {
                std::string url = std::move(pending_spoken_notify_url_);
                pending_spoken_notify_url_.clear();
                const bool cont = spoken_notify_continue_;
                spoken_notify_continue_ = false;
                ESP_LOGI(TAG, "idle → play spoken notify cont=%d", cont ? 1 : 0);
                StartNotification(std::move(url), {}, {}, !cont);
            }
#endif
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            /* 唤醒建链即亮屏（sila PowerSaveTimer WakeUp） */
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* CONNECT 紧凑「唤醒中」晚 1s 再弹（overlay 内） */
            nodelio_overlay_show_connecting();
#endif
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            nodelio_overlay_show_chat();
            /* 首唤问候 UDP 常早于 speaking：先开输出，避免功放/解码未就绪吞掉第一句 */
            if (auto* codec = board.GetAudioCodec()) {
                codec->EnableOutput(true);
            }
#endif

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            if (!pending_cloud_speak_.empty()) {
                Schedule([this]() { TryFlushPendingCloudSpeak(); });
            }
#endif
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            /* 勿再 show_chat：会把标题打成「在听」；正文由 SetStatus/on_chat 维护 */

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            /* 云端 TTS 期间：关 voice + 关唤醒 AFE（与 Notifying 同策略）。
             * 若此时再 EnableWakeWord，AFE/AEC 重配共享 I2S，易把问候弄成全程哑音。 */
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            if (auto* codec = board.GetAudioCodec()) {
                codec->EnableOutput(true);
                ESP_LOGI(TAG, "speaking: output on vol=%d (wake off for cloud TTS)",
                         codec->output_volume());
            }
            /* 关唤醒后 audio_input 异步关麦会再动共享 I2S；下一拍再 unmute 一次 */
            Schedule([this]() {
                if (GetDeviceState() != kDeviceStateSpeaking) {
                    return;
                }
                if (auto* codec = Board::GetInstance().GetAudioCodec()) {
                    codec->EnableOutput(true);
                    ESP_LOGI(TAG, "speaking: deferred unmute vol=%d", codec->output_volume());
                }
            });
#else
            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
#endif
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            /* 须关 voice processing + 唤醒 AFE：HTTP 点歌开声瞬间若仍 feed，
             * AFE(FEED) ring 会堆满并抢核，听感就是前几秒「跳一下」。点停靠 UI。 */
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (news_awaiting_mcp_) {
        // A deferred state/playback event must not overwrite the just-sent detect turn.
        return;
    }
    /* 云端 UDP/AES 需连续内部堆；先放开口令 DRAM 占位，否则 errno=12 炸上行 */
    nodelio_offline_cmd_yield_for_cloud_audio();
#endif

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* EnableInput 会重配共享 I2S TX；开麦后再 unmute，避免问候 PCM 写进 mute DAC */
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        codec->EnableOutput(true);
    }
#endif

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                                    std::string device_key, bool play_popup) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    // Keep voice processing enabled during media playback so microphone works for wake word detection
    // audio_service_.EnableVoiceProcessing(false);
    // Keep wake word detection enabled during media playback so user can interrupt
    // audio_service_.EnableWakeWordDetection(false);
    // audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        return;
    }

    audio_service_.ResetDecoder();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 先挂 MQTT 再开 HTTP：避免在途 Opus 与首包抢队列；再清一次解码器 */
    MarkTransportSuspended(true);
    SuspendTransport();
    audio_service_.ResetDecoder();
#endif
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    if (play_popup) {
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        },
        std::move(device_key));

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
        return;
    }
}

void Application::StopNotification() {
    DeviceState st = GetDeviceState();
    notify_player_.Stop();
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 关技能弹窗会 media_stop→这里。聆听/说话中勿 ResetDecoder / EnableVoiceProcessing，
     * 否则 Speaking→Listening 见处理器已在跑会跳过 SendStartListening。
     * 但仍须解除播歌时的 transport suspend，否则 MQTT 假死。 */
    if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
        st == kDeviceStateConnecting) {
        ESP_LOGW(TAG, "StopNotification voice-safe (state=%d): resume transport only",
                 (int)st);
        MarkTransportSuspended(false);
        ResumeTransport();
        return;
    }
#endif
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 播歌结束或被语音打断：恢复小智 MQTT（打断后立刻要建链）。 */
    MarkTransportSuspended(false);
    ResumeTransport();
#endif
    // Re-enable voice processing and wake word detection after stopping notification
    audio_service_.EnableVoiceProcessing(true);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 先让媒体决定是否重试/下一首。若已开下一首，勿再 StopNotification 把新流掐掉。 */
    nodelio_media_on_playback_ended(success);
    if (nodelio_media_is_playing() || notification_playback_id_ != playback_id) {
        ESP_LOGI(TAG, "media continues after track end (playing=%d id %lu→%lu)",
                 (int)nodelio_media_is_playing(), static_cast<unsigned long>(playback_id),
                 static_cast<unsigned long>(notification_playback_id_));
        if (!success) {
            /* 失败重试：旧 NotifyPlayer 已结束，保持 notifying，只恢复唤醒 */
            auto& board = Board::GetInstance();
            board.GetDisplay()->SetChatMessage("assistant", "");
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            audio_service_.EnableWakeWordDetection(true);
        }
        return;
    }
    /* DSH 多段 spoken：先回 Idle 再播下一段（无 popup） */
    if (success && !spoken_notify_queue_.empty()) {
        StopNotification();
        Schedule([this]() { PlayNextSpokenNotify(); });
        return;
    }
    spoken_notify_queue_.clear();
    spoken_notify_continue_ = false;
#endif
    StopNotification();
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    CancelNewsReadPlayback();
#endif
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
void Application::SuppressCloudTts(int duration_ms)
{
    if (duration_ms < 500) {
        duration_ms = 500;
    }
    if (duration_ms > 8000) {
        duration_ms = 8000; /* 上限收紧：长抑制会卡 Listening、唤醒失灵 */
    }
    suppress_cloud_tts_until_us_ = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    ESP_LOGI(TAG, "suppress cloud tts for %d ms (until=%lld)", duration_ms,
             (long long)suppress_cloud_tts_until_us_);
    /* 已在 Listening：勿发 abort（会让云端停收上行）。等 tts start 再 drop+re-arm。 */
    if (GetDeviceState() == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetDeviceState(kDeviceStateListening);
    }
}

bool Application::IsCloudTtsSuppressed() const
{
    return suppress_cloud_tts_until_us_ > 0 && esp_timer_get_time() < suppress_cloud_tts_until_us_;
}

void Application::ContinueListeningAfterLocalSkill()
{
    aborted_ = false;
    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        return;
    }
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    ESP_LOGI(TAG, "re-arm SendStartListening after local skill");
    StartListeningAudio();
}

void Application::EndVoiceSessionAfterLocalSkill()
{
    suppress_cloud_tts_until_us_ = 0;
    aborted_ = false;
    pending_confirm_read_news_ = false;
    news_awaiting_mcp_ = false;
    news_detect_echo_.clear();
    pending_cloud_speak_.clear();
    ESP_LOGI(TAG, "end voice session after local skill (state=%d ch=%d)", (int)GetDeviceState(),
             (protocol_ && protocol_->IsAudioChannelOpened()) ? 1 : 0);
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
        return;
    }
    if (GetDeviceState() == kDeviceStateListening || GetDeviceState() == kDeviceStateSpeaking ||
        GetDeviceState() == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::TryFlushPendingCloudSpeak()
{
    if (pending_cloud_speak_.empty() || !protocol_ || !protocol_->IsAudioChannelOpened()) {
        return;
    }
    const auto st = GetDeviceState();
    if (st != kDeviceStateListening && st != kDeviceStateSpeaking) {
        return;
    }
    if (st == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    std::string text = std::move(pending_cloud_speak_);
    pending_cloud_speak_.clear();
    ESP_LOGI(TAG, "cloud speak via listen detect (%u bytes)", (unsigned)text.size());
    protocol_->SendWakeWordDetected(text);
}

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
/** 云端 detect 约 ≤16 字；超长会 Alert「仅用于唤醒词」。 */
static size_t utf8_char_count(const char *s)
{
    size_t n = 0;
    if (!s) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
        } else {
            p++;
        }
        n++;
    }
    return n;
}

static constexpr size_t kDetectMaxChars = 16;
#endif

void Application::QueueCloudSpeakPrompt(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    std::string prompt;
    static const char kPrefix[] = "请向用户简短播报";
    if (strncmp(text, kPrefix, sizeof(kPrefix) - 1) == 0) {
        prompt = text;
    } else {
        prompt.reserve(sizeof(kPrefix) + 2 + strlen(text));
        prompt = kPrefix;
        prompt += "：";
        prompt += text;
    }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 云端拒长 detect；新闻正文改走 self.skill.news，勿再塞 360B 原文 */
    if (utf8_char_count(prompt.c_str()) > kDetectMaxChars) {
        ESP_LOGW(TAG, "drop cloud speak: detect too long chars≈%u text=%.48s",
                 (unsigned)utf8_char_count(prompt.c_str()), prompt.c_str());
        return;
    }
#else
    constexpr size_t kMaxPrompt = 520;
    if (prompt.size() > kMaxPrompt) {
        size_t end = kMaxPrompt;
        while (end > 0 && (static_cast<unsigned char>(prompt[end]) & 0xC0) == 0x80) {
            end--;
        }
        prompt.resize(end);
    }
#endif
    pending_cloud_speak_ = std::move(prompt);
    suppress_cloud_tts_until_us_ = 0;
    aborted_ = false;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    nodelio_offline_cmd_yield_for_cloud_audio();
#endif
    ESP_LOGI(TAG, "queue cloud speak (%u bytes) state=%d ch=%d",
             (unsigned)pending_cloud_speak_.size(), (int)GetDeviceState(),
             (protocol_ && protocol_->IsAudioChannelOpened()) ? 1 : 0);
    TryFlushPendingCloudSpeak();
}

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
void Application::HandleListenIdleTick() {
    const uint32_t rx_count = cloud_audio_rx_count_.load(std::memory_order_relaxed);
    const bool received_audio = rx_count != listen_idle_last_audio_rx_count_;
    listen_idle_last_audio_rx_count_ = rx_count;
    // Count accepted cloud packets as activity even if the queue is empty at this tick.
    // During the farewell, only new cloud audio cancels closing: our own popup must not.
    const bool playing = !listen_idle_pending_close_ && !audio_service_.IsPlaybackIdle();
    /* 聆听中无对话：提示（抽屉按 farewell 即时收）→ 独立 1s 后关通道 */
    if (!received_audio && !playing && !news_awaiting_mcp_ &&
        GetDeviceState() == kDeviceStateListening && protocol_ &&
        protocol_->IsAudioChannelOpened()) {
        const int lim = (int)nodelio_listen_idle_seconds();
        constexpr int kAnnounceHoldS = 1;
        if (!listen_idle_pending_close_) {
            listen_idle_ticks_++;
            if (lim > 0 && listen_idle_ticks_ >= lim) {
                listen_idle_pending_close_ = true;
                listen_announce_hold_ticks_ = 0;
                ESP_LOGI(TAG, "listen idle %ds → announce then close", lim);
                auto* display = Board::GetInstance().GetDisplay();
                /* 「先不聊了/有事再叫我」走 farewell：同帧收抽屉；toast 仍可见 */
                display->SetChatMessage("assistant", "先不聊了，有事再叫我");
                nodelio_panel_ui_toast("无对话，小智先退下了");
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
        } else {
            listen_announce_hold_ticks_++;
            if (listen_announce_hold_ticks_ >= kAnnounceHoldS) {
                ESP_LOGI(TAG, "listen idle announce done → close channel");
                listen_idle_pending_close_ = false;
                listen_idle_ticks_ = 0;
                listen_announce_hold_ticks_ = 0;
                protocol_->CloseAudioChannel();
            }
        }
    } else {
        listen_idle_ticks_ = 0;
        listen_announce_hold_ticks_ = 0;
        listen_idle_pending_close_ = false;
    }
}

void Application::CheckNewsReadRequest() {
    if (!news_awaiting_mcp_) {
        return;
    }
    if (!protocol_ || !protocol_->IsAudioChannelOpened() || !nodelio_skill_news_ui_active()) {
        news_awaiting_mcp_ = false;
        news_reading_ = false;
        news_detect_echo_.clear();
        ContinueListeningAfterLocalSkill();
        nodelio_skill_news_sync_read_btn();
        return;
    }
    if (esp_timer_get_time() < news_mcp_deadline_us_) {
        return;
    }
    if (news_mcp_detect_retries_++ == 0) {
        ESP_LOGW(TAG, "news read: no tools/call after 12s, retry once");
        SendNewsReadRequest();
        return;
    }
    AbortSpeaking(kAbortReasonNone);
    audio_service_.ResetDecoder();
    news_awaiting_mcp_ = false;
    news_reading_ = false;
    news_detect_echo_.clear();
    ESP_LOGW(TAG, "news read: tools/call timeout; check tools/list and MCP send logs");
    nodelio_panel_ui_toast("云端未调用新闻工具，请重试");
    ContinueListeningAfterLocalSkill();
    nodelio_skill_news_sync_read_btn();
}

void Application::OnNewsReadToolCalled() {
    // Called on the application task, only after a validated tools/call is dispatched.
    news_awaiting_mcp_ = false;
    suppress_cloud_tts_until_us_ = 0;
    aborted_ = false;
    if (news_playback_token_ == 0) {
        news_playback_start_count_ = audio_service_.GetPlaybackCount();
    }
    ESP_LOGI(TAG, "news read: tools/call received, waiting for article result");
    nodelio_skill_news_sync_read_btn();
}

void Application::CancelNewsReadPlayback() {
    news_playback_token_ = 0;
    news_tts_stopped_ = false;
    nodelio_skill_news_cancel_read();
}

void Application::OnNewsReadSentence(const char* text) {
    // Runs on the application task. A start event can precede tools/call and be
    // discarded by the MCP gate; the server need not send another start for the body.
    const auto state = GetDeviceState();
    if (news_awaiting_mcp_ || aborted_ || IsCloudTtsSuppressed() || !protocol_ ||
        !protocol_->IsAudioChannelOpened() ||
        (state != kDeviceStateListening && state != kDeviceStateSpeaking)) {
        return;
    }
    if (const auto token = nodelio_skill_news_on_sentence(text)) {
        news_playback_token_ = token;
        news_reading_ = true;
        if (state == kDeviceStateListening && !news_tts_stopped_) {
            ESP_LOGI(TAG, "news article sentence: restore speaking token=%u", (unsigned)token);
            SetDeviceState(kDeviceStateSpeaking);
        }
    }
}

void Application::FinishNewsReadPlayback() {
    if (!news_tts_stopped_ || !audio_service_.IsPlaybackIdle()) {
        return;
    }
    const uint32_t token = news_playback_token_;
    news_playback_token_ = 0;
    news_tts_stopped_ = false;
    bool more = false;
    const bool played = audio_service_.GetPlaybackCount() != news_playback_start_count_;
    const bool finished = !aborted_ && nodelio_skill_news_finish_chunk(token, played, &more);
    const DeviceState st = GetDeviceState();
    ESP_LOGI(TAG, "news playback drained: token=%u audio=%d complete=%d more=%d state=%d",
             (unsigned)token, played, finished, more, (int)st);
    /* SILA 可在 Listening 收尾包（UDP 早于 tts.start / stop 后仍排水）；勿只认 Speaking。 */
    if (finished && more && protocol_ && protocol_->IsAudioChannelOpened() &&
        (st == kDeviceStateSpeaking || st == kDeviceStateListening)) {
        news_read_continue_ = true;
        news_reading_ = true;
        news_mcp_detect_retries_ = 0;
        listen_idle_ticks_ = 0;
        listen_idle_pending_close_ = false;
        SendNewsReadRequest(false);
        nodelio_skill_news_sync_read_btn();
        return;
    }
    if (finished && more) {
        /* 游标已前进；保留可点「继续」/说「继续读」，勿当失败 cancel。 */
        ESP_LOGW(TAG, "news auto-continue skipped: state=%d ch=%d (UI/voice continue still ok)",
                 (int)st, protocol_ && protocol_->IsAudioChannelOpened() ? 1 : 0);
        news_reading_ = false;
        nodelio_skill_news_sync_read_btn();
        if (st == kDeviceStateSpeaking) {
            SetDeviceState(listening_mode_ == kListeningModeManualStop ? kDeviceStateIdle
                                                                       : kDeviceStateListening);
        }
        return;
    }
    // Partial/failed audio must not silently advance the article cursor.
    news_reading_ = false;
    nodelio_skill_news_cancel_read();
    nodelio_skill_news_sync_read_btn();
    if (st == kDeviceStateSpeaking) {
        SetDeviceState(listening_mode_ == kListeningModeManualStop ? kDeviceStateIdle
                                                                   : kDeviceStateListening);
    }
}

void Application::SendNewsReadRequest(bool interrupt) {
    // Cancel the response to the original STT before submitting the explicit tool request,
    // even if its tts.start has not arrived yet.
    if (interrupt) {
        AbortSpeaking(kAbortReasonNone);
        audio_service_.ResetDecoder();
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateListening);
    }
    news_detect_echo_ = news_read_continue_ ? "请用工具继续读" : "请用工具读新闻";
    news_awaiting_mcp_ = true;
    news_mcp_deadline_us_ = esp_timer_get_time() + 12000000;
    aborted_ = false;
    suppress_cloud_tts_until_us_ = 0;
    nodelio_offline_cmd_yield_for_cloud_audio();
    ESP_LOGI(TAG, "news read: request op=%s attempt=%d", news_read_continue_ ? "continue" : "read",
             news_mcp_detect_retries_ + 1);
    protocol_->SendWakeWordDetected(news_detect_echo_);
    // Do not follow detect with listen.start: it can interrupt the new tool/TTS turn.
}

void Application::StartLocalNewsRead(const char *op)
{
    const char *use_op = (op && op[0]) ? op : "read";
    news_awaiting_mcp_ = false;
    news_detect_echo_.clear();
    news_mcp_detect_retries_ = 0;
    pending_confirm_read_news_ = false;
    pending_cloud_speak_.clear();

    if (strcmp(use_op, "read") == 0) {
        if (!nodelio_skill_news_prepare_read()) {
            nodelio_panel_ui_toast("暂无可读正文");
            return;
        }
    } else if (strcmp(use_op, "continue") != 0) {
        return;
    }

    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        nodelio_panel_ui_toast("请先唤醒再说「读这篇」");
        return;
    }

    news_read_continue_ = strcmp(use_op, "continue") == 0;
    news_reading_ = true;
    nodelio_panel_ui_set_voice_hint(news_read_continue_ ? "继续读" : "读这篇");
    SendNewsReadRequest();
    nodelio_skill_news_sync_read_btn();
}

void Application::PauseNewsRead()
{
    if (!news_reading_ && GetDeviceState() != kDeviceStateSpeaking) {
        nodelio_skill_news_sync_read_btn();
        return;
    }
    ESP_LOGI(TAG, "news read: pause (keep unread text)");
    news_reading_ = false;
    news_awaiting_mcp_ = false;
    news_mcp_deadline_us_ = 0;
    news_mcp_detect_retries_ = 0;
    news_detect_echo_.clear();
    const bool speaking = GetDeviceState() == kDeviceStateSpeaking;
    AbortSpeaking(kAbortReasonNone);
    if (speaking) {
        audio_service_.ResetDecoder();
        SetDeviceState(kDeviceStateListening);
    }
    nodelio_panel_ui_toast("已暂停朗读");
    ContinueListeningAfterLocalSkill();
    nodelio_skill_news_sync_read_btn();
}

static bool spoken_tts_url_ok(const char *audio_url)
{
    if (!audio_url || !audio_url[0]) {
        return false;
    }
    const char *p = strstr(audio_url, "/nodelio/dsh/tts/");
    const char *base = p ? p + sizeof("/nodelio/dsh/tts/") - 1 : nullptr;
    if (!base || !base[0]) {
        return false;
    }
    /* 文件名：{hex16}.wav|mp3（edge-tts→mp3，sapi→wav）；扩展名不参与 hex 校验 */
    const char *q = base;
    while (*q && *q != '?' && *q != '#') {
        ++q;
    }
    const char *dot = nullptr;
    for (const char *c = base; c < q; ++c) {
        if (*c == '.') {
            if (dot) {
                return false;
            }
            dot = c;
            break;
        }
        const bool hex = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f') ||
                         (*c >= 'A' && *c <= 'F');
        if (!hex) {
            return false;
        }
    }
    if (!dot || dot == base) {
        return false;
    }
    const size_t stem = (size_t)(dot - base);
    if (stem < 8 || stem > 32) {
        return false;
    }
    return (q - dot == 4 && dot[1] == 'w' && dot[2] == 'a' && dot[3] == 'v') ||
           (q - dot == 4 && (dot[1] == 'm' || dot[1] == 'M') && (dot[2] == 'p' || dot[2] == 'P') &&
            dot[3] == '3');
}

bool Application::PlayNextSpokenNotify()
{
    if (spoken_notify_queue_.empty()) {
        spoken_notify_continue_ = false;
        return false;
    }
    std::string url = std::move(spoken_notify_queue_.front());
    spoken_notify_queue_.pop_front();
    spoken_notify_continue_ = true;
    ESP_LOGI(TAG, "spoken notify continue %s remain=%u", url.c_str(),
             (unsigned)spoken_notify_queue_.size());
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        pending_spoken_notify_url_ = std::move(url);
        return true;
    }
    StartNotification(std::move(url), {}, {}, false);
    return true;
}
#endif

void Application::PlaySpokenAfterLocalSkill(const char *text, const char *audio_url)
{
    const char *one[1] = {audio_url};
    PlaySpokenPlaylistAfterLocalSkill(text, audio_url && audio_url[0] ? one : nullptr,
                                      audio_url && audio_url[0] ? 1 : 0);
}

void Application::PlaySpokenPlaylistAfterLocalSkill(const char *text, const char *const *urls, int n)
{
    /* 云 listen.detect 只接受唤醒词，勿再塞「请播报：…」 */
    pending_cloud_speak_.clear();
    suppress_cloud_tts_until_us_ = 0;
    aborted_ = false;
    spoken_notify_queue_.clear();
    spoken_notify_continue_ = false;
    dsh_local_open_cooldown_until_us_ = esp_timer_get_time() + (int64_t)20000 * 1000;
    Board::GetInstance().NoteUserActivity();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    (void)text;

    std::vector<std::string> good;
    for (int i = 0; i < n && urls; i++) {
        if (spoken_tts_url_ok(urls[i])) {
            good.emplace_back(urls[i]);
        } else if (urls[i] && urls[i][0]) {
            ESP_LOGW(TAG, "spoken notify skip bad url=%s", urls[i]);
        }
    }
    if (good.empty()) {
        ESP_LOGI(TAG, "spoken text-only (no bridge wav)");
        EndVoiceSessionAfterLocalSkill();
        return;
    }
    for (size_t i = 1; i < good.size(); i++) {
        spoken_notify_queue_.push_back(good[i]);
    }
    pending_spoken_notify_url_ = good[0];
    ESP_LOGI(TAG, "spoken notify playlist n=%u first=%s", (unsigned)good.size(),
             pending_spoken_notify_url_.c_str());
    if (GetDeviceState() == kDeviceStateIdle && !notify_player_.IsBusy()) {
        std::string url = std::move(pending_spoken_notify_url_);
        pending_spoken_notify_url_.clear();
        StartNotification(std::move(url), {}, {}, true);
        return;
    }
    EndVoiceSessionAfterLocalSkill();
}

void Application::LocalOpenDshStatus(const char *reason)
{
    ESP_LOGI(TAG, "local open dsh.status reason=%s", reason ? reason : "?");
    SuppressCloudTts(3000);
    pending_confirm_open_dsh_ = false;
    dsh_local_open_cooldown_until_us_ = esp_timer_get_time() + (int64_t)20000 * 1000;
    nodelio_panel_ui_open_app("查看任务");
    ContinueListeningAfterLocalSkill();
}

void Application::MaybeArmPendingOpenDshFromTts(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    /* 云端在问/承诺打开任务应用 → 下一句肯定答复本地开 */
    const bool offer = (strstr(text, "任务应用") != nullptr) || (strstr(text, "打开任务") != nullptr) ||
                       (strstr(text, "DSH任务") != nullptr) || (strstr(text, "DSH") != nullptr &&
                                                                strstr(text, "打开") != nullptr) ||
                       (strstr(text, "任务情况") != nullptr) || (strstr(text, "查看任务") != nullptr);
    if (offer) {
        pending_confirm_open_dsh_ = true;
        ESP_LOGI(TAG, "arm pending_confirm_open_dsh from tts: %s", text);
    }
}

static bool tts_looks_like_app_deny(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    const bool deny = (strstr(text, "找不到") != nullptr || strstr(text, "没有找到") != nullptr ||
                       strstr(text, "没找到") != nullptr || strstr(text, "无法打开") != nullptr ||
                       strstr(text, "打不开") != nullptr || strstr(text, "不能打开") != nullptr);
    if (!deny) {
        return false;
    }
    return strstr(text, "DSH") != nullptr || strstr(text, "任务") != nullptr ||
           strstr(text, "应用") != nullptr || strstr(text, "新闻") != nullptr ||
           strstr(text, "天气") != nullptr || strstr(text, "空气") != nullptr ||
           strstr(text, "电台") != nullptr || strstr(text, "曲库") != nullptr ||
           strstr(text, "播客") != nullptr || strstr(text, "音乐") != nullptr ||
           strstr(text, "名言") != nullptr;
}

static bool tts_looks_like_news_instruction(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    /* 旧回包前缀；或整句几乎只有设备尾标 */
    if (strstr(text, "逐字朗读") || strstr(text, "不要换篇") || strstr(text, "不要读出这些指令") ||
        strstr(text, "本轮不要再调") || strstr(text, "请先逐字朗读") ||
        strstr(text, "设备将在音频播完") || strstr(text, "不要自行调用 continue") ||
        strstr(text, "Speak ONLY") || strstr(text, "speak_body_only")) {
        return true;
    }
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        ++p;
    }
    static const char *const kTags[] = {"【摘要完】", "【摘要】", "【续】", "【完】", "【等】",
                                        "【缺】"};
    for (size_t i = 0; i < sizeof(kTags) / sizeof(kTags[0]); ++i) {
        const size_t n = strlen(kTags[i]);
        if (strncmp(p, kTags[i], n) != 0) {
            continue;
        }
        /* 尾标独句才丢；正文里偶然出现【 不在此判 */
        const char *rest = p + n;
        while (*rest) {
            if (*rest == ' ' || *rest == '\t' || *rest == '\n' || *rest == '\r' || *rest == '.' ||
                *rest == '!') {
                ++rest;
                continue;
            }
            /* 。 */
            if ((unsigned char)rest[0] == 0xE3 && (unsigned char)rest[1] == 0x80 &&
                (unsigned char)rest[2] == 0x82) {
                rest += 3;
                continue;
            }
            break;
        }
        return rest[0] == '\0';
    }
    return false;
}

void Application::ArmPendingConfirmReadNews()
{
    pending_confirm_read_news_ = true;
    dsh_local_open_cooldown_until_us_ = esp_timer_get_time() + (int64_t)20000 * 1000;
    ESP_LOGI(TAG, "arm pending_confirm_read_news");
}

bool Application::TryConfirmReadNewsFromStt(const char *text)
{
    if (!pending_confirm_read_news_ || !text || !text[0]) {
        return false;
    }
    char buf[48];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(buf); ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || c == '?' || c == '!' || c == ',' || c == ' ' || c == '\t') {
            continue;
        }
        if ((c == 0xE3 && (unsigned char)p[1] == 0x80 &&
             ((unsigned char)p[2] == 0x82 || (unsigned char)p[2] == 0x81)) ||
            (c == 0xEF && (unsigned char)p[1] == 0xBC &&
             ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81 ||
              (unsigned char)p[2] == 0x8C))) {
            p += 2;
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = '\0';
    const bool yes =
        strcmp(buf, "是的") == 0 || strcmp(buf, "是") == 0 || strcmp(buf, "好的") == 0 ||
        strcmp(buf, "好") == 0 || strcmp(buf, "嗯") == 0 || strcmp(buf, "行") == 0 ||
        strcmp(buf, "可以") == 0 || strcmp(buf, "要") == 0 || strcmp(buf, "对") == 0 ||
        strcmp(buf, "读") == 0 || strcmp(buf, "读吧") == 0 || strcmp(buf, "读取") == 0 ||
        strstr(buf, "读这篇") != nullptr || strstr(buf, "读一下") != nullptr ||
        strstr(buf, "朗读") != nullptr || strstr(buf, "念一下") != nullptr ||
        strstr(buf, "要读") != nullptr || strstr(buf, "读第一") != nullptr;
    const bool no = strcmp(buf, "不") == 0 || strcmp(buf, "不用") == 0 ||
                    strcmp(buf, "不要") == 0 || strcmp(buf, "不用了") == 0 ||
                    strcmp(buf, "算了") == 0 || strcmp(buf, "否") == 0 ||
                    strstr(buf, "不用读") != nullptr || strstr(buf, "别读") != nullptr ||
                    strstr(buf, "不要读") != nullptr || strstr(buf, "不读") != nullptr ||
                    strstr(buf, "不朗读") != nullptr || strstr(buf, "不要朗读") != nullptr ||
                    strstr(buf, "不用朗读") != nullptr;
    ESP_LOGI(TAG, "confirm_read_news pending=1 stt='%s' norm='%s' yes=%d no=%d", text, buf,
             yes ? 1 : 0, no ? 1 : 0);
    if (no) {
        pending_confirm_read_news_ = false;
        nodelio_panel_ui_toast("好的");
        SuppressCloudTts(2000);
        ContinueListeningAfterLocalSkill();
        return true;
    }
    if (!yes) {
        pending_confirm_read_news_ = false;
        return false;
    }
    pending_confirm_read_news_ = false;
    // prepare_read selects the first item only when no article is selected.
    // Preserve an article the user opened by touch while the read offer was pending.
    StartLocalNewsRead("read");
    return true;
}

bool Application::TryConfirmOpenDshFromStt(const char *text)
{
    if (!pending_confirm_open_dsh_ || !text || !text[0]) {
        return false;
    }
    /* 剥句末标点后比肯定词 */
    char buf[48];
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof(buf); ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || c == '?' || c == '!' || c == ',' || c == ' ' || c == '\t') {
            continue;
        }
        /* 。？！， */
        if ((c == 0xE3 && (unsigned char)p[1] == 0x80 &&
             ((unsigned char)p[2] == 0x82 || (unsigned char)p[2] == 0x81)) ||
            (c == 0xEF && (unsigned char)p[1] == 0xBC &&
             ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81 ||
              (unsigned char)p[2] == 0x8C))) {
            p += 2;
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = '\0';
    const bool yes =
        (strcmp(buf, "是的") == 0 || strcmp(buf, "是") == 0 || strcmp(buf, "好的") == 0 ||
         strcmp(buf, "好") == 0 || strcmp(buf, "嗯") == 0 || strcmp(buf, "行") == 0 ||
         strcmp(buf, "可以") == 0 || strcmp(buf, "打开") == 0 || strcmp(buf, "打开吧") == 0 ||
         strcmp(buf, "对") == 0 || strcmp(buf, "要") == 0 || strcmp(buf, "打开任务") == 0 ||
         strcmp(buf, "打开DSH") == 0);
    ESP_LOGI(TAG, "confirm_open_dsh pending=1 stt='%s' norm='%s' yes=%d", text, buf, yes ? 1 : 0);
    if (!yes) {
        /* 新意图结束旧询问，勿把之后的「是」误当成开 DSH。 */
        pending_confirm_open_dsh_ = false;
        return false;
    }
    LocalOpenDshStatus("stt_confirm_yes");
    return true;
}
#endif

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
void Application::RequestManualFirmwareCheck() {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) {
        nodelio_panel_ui_toast_defer("正在检查…");
        return;
    }
    if (GetDeviceState() != kDeviceStateIdle) {
        busy = false;
        nodelio_panel_ui_toast_defer("请先结束对话再检查");
        return;
    }
    nodelio_panel_ui_toast_defer("正在检查固件…");
    BaseType_t ok = xTaskCreate(
        [](void* /*arg*/) {
            Ota ota;
            auto check = ota.CheckVersion();
            if (!check) {
                ESP_LOGW(TAG, "manual firmware check failed: %s", check.error().ToString().c_str());
                nodelio_panel_ui_toast_defer("检查失败，请稍后重试");
                busy = false;
                vTaskDelete(nullptr);
                return;
            }
            if (!ota.HasNewVersion()) {
                ESP_LOGI(TAG, "manual check: current is latest (%s)", ota.GetCurrentVersion().c_str());
                nodelio_panel_ui_toast_defer("已是最新版本");
                busy = false;
                vTaskDelete(nullptr);
                return;
            }
            std::string url = ota.GetFirmwareUrl();
            std::string ver = ota.GetFirmwareVersion();
            ESP_LOGI(TAG, "manual check: new firmware %s", ver.c_str());
            Application::GetInstance().Schedule([url = std::move(url), ver = std::move(ver)]() {
                char tip[72];
                snprintf(tip, sizeof(tip), "发现新版本 %s，开始升级", ver.c_str());
                nodelio_panel_ui_toast_defer(tip);
                bool ok = Application::GetInstance().UpgradeFirmware(url, ver);
                busy = false; /* 成功会 reboot；失败回 idle */
                if (!ok) {
                    nodelio_panel_ui_toast_defer("升级失败");
                }
            });
            vTaskDelete(nullptr);
        },
        "fw_chk", 4096 * 3, nullptr, 2, nullptr);
    if (ok != pdPASS) {
        busy = false;
        nodelio_panel_ui_toast_defer("检查任务启动失败");
    }
}
#endif

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            nodelio_panel_ui_toast_defer(message.c_str());
#endif
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            /* pause_for_voice 会把 Notifying→Idle，故不能再要求仍为 Notifying */
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
            if (nodelio_media_is_playing()) {
                nodelio_media_pause_for_voice();
                nodelio_offline_cmd_yield_for_cloud_audio();
                audio_service_.EnableVoiceProcessing(false);
            } else if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
#else
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
#endif
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /* 墙板：息屏只挡对话/升级/配网。Activating（OTA/注册失败重试）不得锁死背光整晚发热。
     * 音乐 Notifying：若开启「播放可息屏」则允许关背光，不停播（断网另挡）。 */
    const bool media_ok = nodelio_media_is_playing() && nodelio_media_sleep_enabled();
    const auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking || state == kDeviceStateUpgrading ||
        state == kDeviceStateWifiConfiguring || state == kDeviceStateAudioTesting) {
        return false;
    }
    if (state == kDeviceStateNotifying && !media_ok) {
        return false;
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }
    /* 媒体播时 decode/playback 队列非空，仍允许息屏 */
    if (!media_ok && !audio_service_.IsIdle()) {
        return false;
    }
    return true;
#else
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }
    if (!audio_service_.IsIdle()) {
        return false;
    }
    return true;
#endif
}

void Application::SuspendTransport() {
    if (protocol_) {
        protocol_->SuspendTransport();
    }
}

void Application::ResumeTransport() {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (!nodelio_xiaozhi_enabled()) {
        ESP_LOGI(TAG, "ResumeTransport skipped (Xiaozhi disabled)");
        return;
    }
#endif
    if (protocol_) {
        protocol_->ResumeTransport();
    }
}

void Application::MarkTransportSuspended(bool suspended) {
    if (protocol_) {
        protocol_->MarkTransportSuspended(suspended);
    }
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
bool Application::IsHttpAudioBusy() const { return notify_player_.IsBusy(); }

bool Application::HasHttpAudioPlaybackStarted() const {
    return notify_player_.HasPlaybackStarted();
}

void Application::YieldVoiceForMedia() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
}

bool Application::PlayHttpAudio(std::string audio_url, std::string device_key) {
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "PlayHttpAudio ignored while busy");
        return false;
    }
    /* 点歌勿弹 OGG_POPUP：会往 Opus 解码队列塞包，与 MP3 PCM 抢开头，刷 ESP_OPUS_DEC */
    StartNotification(std::move(audio_url), std::vector<NotifySubtitle>{}, std::move(device_key),
                      false);
    return GetDeviceState() == kDeviceStateNotifying;
}

void Application::StopHttpAudio() { StopNotification(); }
#endif

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
