#include "backlight.h"
#include "settings.h"

#include <esp_log.h>
#include <driver/ledc.h>

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
    saved_on_brightness_ = static_cast<uint8_t>(saved_brightness);
    saved_on_loaded_ = true;
}

void Backlight::RestoreBrightness() {
    PreloadSavedBrightness();
    SetBrightness(saved_on_brightness_);
}

void Backlight::SetBrightness(uint8_t brightness, bool permanent) {
    if (brightness > 100) {
        brightness = 100;
    }

    if (brightness_ == brightness) {
        return;
    }

    /* 息屏不要 5ms 一档往下爬：爬的过程升压电容一直被充，最后一档才关。 */
    if (brightness == 0) {
        if (permanent) {
            Settings settings("display", true);
            settings.SetInt("brightness", brightness);
        }
        if (transition_timer_ != nullptr) {
            esp_timer_stop(transition_timer_);
        }
        target_brightness_ = 0;
        brightness_ = 0;
        SetBrightnessImpl(0);
        ESP_LOGI(TAG, "Set brightness to 0");
        return;
    }

    saved_on_brightness_ = brightness;
    saved_on_loaded_ = true;

    if (permanent) {
        Settings settings("display", true);
        settings.SetInt("brightness", brightness);
    }

    target_brightness_ = brightness;
    step_ = (target_brightness_ > brightness_) ? 1 : -1;

    if (transition_timer_ != nullptr) {
        // 启动定时器，每 5ms 更新一次
        esp_timer_start_periodic(transition_timer_, 5 * 1000);
    }
    ESP_LOGI(TAG, "Set brightness to %d", brightness);
}

void Backlight::OnTransitionTimer() {
    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
        return;
    }

    brightness_ += step_;
    SetBrightnessImpl(brightness_);

    if (brightness_ == target_brightness_) {
        esp_timer_stop(transition_timer_);
    }
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

