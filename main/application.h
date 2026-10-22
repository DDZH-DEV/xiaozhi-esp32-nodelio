#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "notify/notify_player.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /** 设置页手工触发：查小智云固件版本，有新版则升级；后台任务，可重入挡. */
    void RequestManualFirmwareCheck();
#endif
    bool CanEnterSleepMode();
    /** 须在主任务调用：息屏断网前挂起协议；亮屏后恢复. */
    void SuspendTransport();
    void ResumeTransport();
    /** 任意任务：先挡重连，再 Schedule SuspendTransport. */
    void MarkTransportSuspended(bool suspended);
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    /** 本机 HTTP 音频（Ogg/Opus）；须在主任务或经 Schedule。与语音会话互斥。
     *  设备非 Idle 或上一首 HTTP 任务未退出时返回 false，调用方需稍后重试。 */
    bool PlayHttpAudio(std::string audio_url, std::string device_key = {});
    /** 点歌前关掉仍开着的语音通道，避免后续 TTS 抢解码器。 */
    void YieldVoiceForMedia();
    void StopHttpAudio();
    bool IsHttpAudioBusy() const;
    /**
     * 本地已处理口令（开 DSH/曲库等）：Abort + 短时丢弃云端否定 TTS。
     * 勿用过长窗口，否则会卡在 Listening、唤醒无响应。
     */
    void SuppressCloudTts(int duration_ms = 3000);
    bool IsCloudTtsSuppressed() const;
    /** 本地技能完成：清抑制、关语音通道回 Idle，恢复唤醒。 */
    void EndVoiceSessionAfterLocalSkill();
    /**
     * 本地技能摘要：走小智云 TTS（listen detect），不用桥侧 WAV（生硬且会挂 MQTT）。
     * text 为空则仅结束本轮语音回 Idle。
     */
    void PlaySpokenAfterLocalSkill(const char *text);
#endif
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    uint32_t notification_playback_id_ = 0;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    int listen_idle_ticks_ = 0; /* 聆听无语音累计秒；勿复用 clock_ticks_ */
    int listen_announce_hold_ticks_ = 0; /* 已提示后独立计时关通道，不受 VAD 清零 */
    bool listen_idle_pending_close_ = false; /* 已提示，待短暂展示后再关通道 */
    int64_t suppress_cloud_tts_until_us_ = 0; /* 本地口令处理后丢弃云端 TTS 截止时间 */
    /** 云端刚问「要不要打开任务应用」：下一句「是的」本地直接开 dsh.status */
    bool pending_confirm_open_dsh_ = false;
    void MaybeArmPendingOpenDshFromTts(const char *text);
    bool TryConfirmOpenDshFromStt(const char *text);
    void LocalOpenDshStatus(const char *reason);
    int64_t dsh_local_open_cooldown_until_us_ = 0; /* 本地已开列表，忽略云端否认再开 */
    std::string pending_spoken_notify_url_; /* Idle 后播报的 HTTP 音频（遗留；技能摘要改走云 TTS） */
    std::string pending_cloud_speak_;       /* 待 listen detect 交给小智云朗读 */
    void TryFlushPendingCloudSpeak();
#endif
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                           std::string device_key = {});
    void StopNotification();
    void HandleNotificationFinished(uint32_t playback_id, bool success);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
