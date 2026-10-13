#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

class PowerSaveTimer {
public:
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1);
    ~PowerSaveTimer();

    void SetEnabled(bool enabled);
    void OnEnterSleepMode(std::function<void()> callback);
    void OnExitSleepMode(std::function<void()> callback);
    void OnShutdownRequest(std::function<void()> callback);
    void WakeUp();
    /** 用户操作：仅重置空闲计时；若已息屏则唤醒。不取消 goodbye 延迟窗口。 */
    void NoteActivity();
    /** 立刻息屏（若当前可进休眠）；否则标记 ASAP，idle 后下一拍进入。 */
    void EnterSleepMode();
    /** goodbye 后延迟息屏；秒数内有唤醒则取消。 */
    void RequestSleepIn(int seconds);

private:
    void PowerSaveCheck();
    void TryEnterSleepMode();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool sleep_asap_ = false;
    bool is_wake_word_running_ = false;
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;
    int seconds_to_sleep_override_ = -1;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
