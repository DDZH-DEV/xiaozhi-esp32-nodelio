#pragma once

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <functional>

// Borrows the board's shared touch/PMU bus. Never resets or deletes that bus.
class SilaAxs15260Touch {
public:
    ~SilaAxs15260Touch();
    // on_activity returns true when a gesture must only wake the screen.
    esp_err_t Start(i2c_master_bus_handle_t bus, lv_display_t* display,
                    std::function<bool()> on_activity);

private:
    static void OnInterrupt(void* arg);
    static void ReadCache(lv_indev_t* indev, lv_indev_data_t* data);
    void Run();
    void Publish(bool pressed, int x, int y);

    i2c_master_dev_handle_t device_ = nullptr;
    TaskHandle_t task_ = nullptr;
    lv_indev_t* indev_ = nullptr;
    std::function<bool()> on_activity_;
    bool irq_attached_ = false;
    bool suppress_gesture_ = false;
    // Accessed only while holding the LVGL port lock.
    lv_point_t point_ = {};
    lv_indev_state_t state_ = LV_INDEV_STATE_RELEASED;
};
