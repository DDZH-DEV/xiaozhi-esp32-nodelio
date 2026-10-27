#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    /** 仅开关空闲断网；息屏计时不受影响（墙板「允许省电」只绑这个）。 */
    void SetNetSleepEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    /** 可选：与息屏独立的断网省电（-1=关闭，默认关闭，不影响其它板）。 */
    void OnEnterNetSleep(std::function<void()> callback);
    void OnExitNetSleep(std::function<void()> callback);
    void WakeUp();
    /** 用户操作：仅重置空闲计时；若已息屏则唤醒。不取消 goodbye 延迟窗口。 */
    void NoteActivity();
    /** 立刻息屏（若当前可进休眠）；否则标记 ASAP，idle 后下一拍进入。 */
    void EnterSleepMode();
    /** goodbye 后延迟息屏；秒数内有唤醒则取消。断网仍按 idle net sleep，不随 goodbye 同秒切断。 */
    void RequestSleepIn(int seconds);
    /** 运行时更新空闲息屏秒数（不含 goodbye 覆盖窗口）。 */
    void SetSecondsToSleep(int seconds);
    /** 运行时更新空闲断网秒数；-1=不单独断网。从最后一次活动起算。 */
    void SetSecondsToNetSleep(int seconds);
    int GetSecondsToSleep() const { return seconds_to_sleep_; }
    bool IsInSleepMode() const { return in_sleep_mode_; }
    bool IsInNetSleep() const { return in_net_sleep_; }

private:
    void PowerSaveCheck();
    void TryEnterSleepMode();
    void TryEnterNetSleep();
    void ExitNetSleep();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool net_sleep_enabled_ = true;
    bool in_sleep_mode_ = false;
    bool in_net_sleep_ = false;
    bool sleep_asap_ = false;
    bool is_wake_word_running_ = false;
    int ticks_ = 0;
    int activity_ticks_ = 0; /* 从最后活动起算；息屏不重置，供断网阈值 */
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    int seconds_to_sleep_override_ = -1;
    int seconds_to_net_sleep_ = -1;
    int seconds_to_net_override_ = -1;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
    std::function<void()> on_enter_net_sleep_;
    std::function<void()> on_exit_net_sleep_;
};
