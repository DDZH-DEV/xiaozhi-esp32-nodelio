#include "backlight.h"
#include "settings.h"

#include <driver/ledc.h>
#include <esp_log.h>
#include <nvs.h>
#include <algorithm>

#define TAG "Backlight"


Backlight::Backlight() {
    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<Backlight*>(arg);
            self->OnTransitionTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &transition_timer_));
}

Backlight::~Backlight() {
    if (transition_timer_ != nullptr) {
        esp_timer_stop(transition_timer_);
        esp_timer_delete(transition_timer_);
    }
}

void Backlight::PreloadSavedBrightness() {
    std::lock_guard<std::mutex> lock(brightness_mutex_);
    if (saved_on_loaded_) {
        return;
    }
    /* 仅冷启动 / 首次恢复走 NVS；须在内部 RAM 栈任务上调用（如 board 初始化）。 */
    Settings settings("display");
    int saved_brightness = settings.GetInt("brightness", 75);
    if (saved_brightness <= 0) {
        ESP_LOGW(TAG, "Brightness value (%d) is too small, setting to default (10)",
                 saved_brightness);
        saved_brightness = 10;
    }
    saved_on_brightness_ = static_cast<uint8_t>(std::min(saved_brightness, 100));
    saved_on_loaded_ = true;
}

void Backlight::RestoreBrightness() {
    PreloadSavedBrightness();
    uint8_t saved;
    {
        std::lock_guard<std::mutex> lock(brightness_mutex_);
        saved = saved_on_brightness_;
    }
    SetBrightness(saved);
}

esp_err_t Backlight::SaveBrightness(uint8_t brightness) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("display", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, "brightness", std::min<int>(brightness, 100));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save brightness failed: %s", esp_err_to_name(err));
    }
    return err;
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    // A preview may already have reached this value. Persistence must still run.
    if (permanent) {
        SaveBrightness(brightness);
    }
    std::lock_guard<std::mutex> lock(brightness_mutex_);

    if (brightness != 0) {
        saved_on_brightness_ = brightness;
        saved_on_loaded_ = true;
    }

    target_brightness_ = brightness;
    // Also cancel an older target when a drag returns to the current value.
    if (brightness_ == brightness) {
        esp_timer_stop(transition_timer_);
        return;
    }

    if (transition_timer_ != nullptr) {
        // 启动定时器，每 5ms 更新一次
        esp_timer_start_periodic(transition_timer_, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Backlight::OnTransitionTimer() {
    uint8_t next;
    {
        std::lock_guard<std::mutex> lock(brightness_mutex_);
        if (brightness_ == target_brightness_) {
            esp_timer_stop(transition_timer_);
            return;
        }
        // Switch off in one tick, without fading through every intermediate duty.
        brightness_ = target_brightness_ == 0
                          ? 0
                          : (brightness_ < target_brightness_ ? brightness_ + 1 : brightness_ - 1);
        next = brightness_;
        if (brightness_ == target_brightness_) {
            esp_timer_stop(transition_timer_);
        }
    }
    // Only the timer writes hardware. Never hold the state lock across a display
    // driver's own lock: callers may already hold the LVGL/display lock.
    SetBrightnessImpl(next);
}

void PwmBacklight::BindChannel(uint32_t duty) {
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = pin_,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = duty,
        .hpoint = 0,
        .flags = {
            .output_invert = output_invert_,
        }
    };
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

PwmBacklight::PwmBacklight(gpio_num_t pin, bool output_invert, uint32_t freq_hz, int off_level) : Backlight() {
    pin_ = pin;
    output_invert_ = output_invert;
    off_level_ = (off_level < 0) ? (output_invert ? 1 : 0) : off_level;
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));
    BindChannel(0);
    /* duty 0 仍可能有窄脉冲；开机先 GPIO 钉灭，等首页首帧再 Restore。 */
    SetBrightnessImpl(0);
}

PwmBacklight::~PwmBacklight() {
    /* 与亮灭极性一致：高亮板 idle=0；invert 板灭时脚为高 */
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, off_level_);
}

void PwmBacklight::SetBrightnessImpl(uint8_t brightness) {
    /* RY3730 / SY7200：duty 0 仍有窄脉冲时 EN 不会关机，输出电容一直被充。
     * 亮度为 0 时停 PWM，再把脚改成 GPIO 钉在灭电平。 */
    if (brightness == 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, off_level_);
        gpio_set_direction(pin_, GPIO_MODE_OUTPUT);
        gpio_set_level(pin_, off_level_);
        held_off_ = true;
        return;
    }
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * brightness) / 100;
    if (held_off_) {
        BindChannel(duty_cycle);
        held_off_ = false;
        return;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

