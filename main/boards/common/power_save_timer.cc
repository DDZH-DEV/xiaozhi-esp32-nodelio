#include "power_save_timer.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PowerSaveTimer"


PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown)
    : cpu_max_freq_(cpu_max_freq), seconds_to_sleep_(seconds_to_sleep), seconds_to_shutdown_(seconds_to_shutdown) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

PowerSaveTimer::~PowerSaveTimer() {
    esp_timer_stop(power_save_timer_);
    esp_timer_delete(power_save_timer_);
}

void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        Settings settings("wifi", false);
        if (!settings.GetBool("sleep_mode", true)) {
            ESP_LOGI(TAG, "Power save timer is disabled by settings");
            return;
        }

        ticks_ = 0;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

void PowerSaveTimer::TryEnterSleepMode() {
    if (!enabled_ || in_sleep_mode_) {
        return;
    }
    auto& app = Application::GetInstance();
    if (!app.CanEnterSleepMode()) {
        return;
    }

    ESP_LOGI(TAG, "Enabling power save mode");
    in_sleep_mode_ = true;
    sleep_asap_ = false;
    ticks_ = 0;
    if (on_enter_sleep_mode_) {
        on_enter_sleep_mode_();
    }

    if (cpu_max_freq_ != -1) {
        auto& audio_service = app.GetAudioService();
        is_wake_word_running_ = audio_service.IsWakeWordRunning();
        if (is_wake_word_running_) {
            audio_service.EnableWakeWordDetection(false);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->EnableInput(false);
        }

        esp_pm_config_t pm_config = {
            .max_freq_mhz = cpu_max_freq_,
            .min_freq_mhz = 40,
            .light_sleep_enable = true,
        };
        esp_pm_configure(&pm_config);
    }
}

void PowerSaveTimer::EnterSleepMode() {
    if (!enabled_) {
        return;
    }
    seconds_to_sleep_override_ = -1;
    sleep_asap_ = true;
    TryEnterSleepMode();
}

void PowerSaveTimer::RequestSleepIn(int seconds) {
    if (!enabled_) {
        return;
    }
    if (seconds <= 0) {
        EnterSleepMode();
        return;
    }
    sleep_asap_ = false;
    ticks_ = 0;
    seconds_to_sleep_override_ = seconds;
    ESP_LOGI(TAG, "Sleep requested in %d s", seconds);
}

void PowerSaveTimer::PowerSaveCheck() {
    auto& app = Application::GetInstance();
    if (!in_sleep_mode_ && !app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    if (sleep_asap_) {
        TryEnterSleepMode();
        return;
    }

    ticks_++;
    const int sleep_after =
        (seconds_to_sleep_override_ > 0) ? seconds_to_sleep_override_ : seconds_to_sleep_;
    if (sleep_after != -1 && ticks_ >= sleep_after) {
        seconds_to_sleep_override_ = -1;
        TryEnterSleepMode();
    }
    if (seconds_to_shutdown_ != -1 && ticks_ >= seconds_to_shutdown_ && on_shutdown_request_) {
        on_shutdown_request_();
    }
}

void PowerSaveTimer::NoteActivity() {
    ticks_ = 0;
    sleep_asap_ = false;
    if (in_sleep_mode_) {
        WakeUp();
    }
}

void PowerSaveTimer::WakeUp() {
    ticks_ = 0;
    sleep_asap_ = false;
    seconds_to_sleep_override_ = -1;
    if (in_sleep_mode_) {
        ESP_LOGI(TAG, "Exiting power save mode");
        in_sleep_mode_ = false;

        if (cpu_max_freq_ != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = cpu_max_freq_,
                .min_freq_mhz = cpu_max_freq_,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            // Enable wake word detection
            auto& app = Application::GetInstance();
            auto& audio_service = app.GetAudioService();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
        }

        if (on_exit_sleep_mode_) {
            on_exit_sleep_mode_();
        }
    }
}
