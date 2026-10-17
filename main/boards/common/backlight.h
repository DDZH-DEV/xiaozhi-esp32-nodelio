#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>


class Backlight {
public:
    Backlight();
    ~Backlight();

    void RestoreBrightness();
    void SetBrightness(uint8_t brightness, bool permanent = false);
    inline uint8_t brightness() const { return brightness_; }

protected:
    void OnTransitionTimer();
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
};


class PwmBacklight : public Backlight {
public:
    /* off_level < 0：沿用 invert（灭时脚为高）。RY3730 的 EN 低电平才关，须显式传 0。 */
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000, int off_level = -1);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;

private:
    void BindChannel(uint32_t duty);
    gpio_num_t pin_ = GPIO_NUM_NC;
    bool output_invert_ = false;
    int off_level_ = 0;
    bool held_off_ = false;
};
