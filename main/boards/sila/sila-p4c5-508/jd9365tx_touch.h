#pragma once

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <functional>

// JD9365TX In-Cell touch (aibox protocol). Borrows the board peripheral I2C bus
// (shared with AXP2101). Never creates/deletes that bus; never pulses shared RST.
class SilaJd9365txTouch {
public:
    ~SilaJd9365txTouch();
    // on_activity returns true when a gesture must only wake the screen.
    esp_err_t Start(i2c_master_bus_handle_t bus, lv_display_t* display,
                    std::function<bool()> on_activity);

private:
    enum AccessMode {
        kAccessDirect,
        kAccessBackdoor,
    };

    static void OnInterrupt(void* arg);
    static void ReadCache(lv_indev_t* indev, lv_indev_data_t* data);
    void Run();
    void Publish(bool pressed, int x, int y);

    esp_err_t EnterBackdoor();
    esp_err_t SramRead(uint32_t reg, void* data, size_t data_bytes);
    esp_err_t RegRead(uint32_t reg, void* data, size_t data_bytes);
    esp_err_t RegWriteU8(uint32_t reg, uint8_t value);
    esp_err_t DetectProtocol(uint16_t* chip_id);
    static uint16_t ScaleAxis(uint16_t value, uint16_t source_size, uint16_t target_size);
    int ParseReport(const uint8_t* buf, size_t len, int* out_x, int* out_y);

    i2c_master_dev_handle_t device_ = nullptr;
    TaskHandle_t task_ = nullptr;
    lv_indev_t* indev_ = nullptr;
    std::function<bool()> on_activity_;
    bool irq_attached_ = false;
    bool suppress_gesture_ = false;
    portMUX_TYPE spinlock_ = portMUX_INITIALIZER_UNLOCKED;
    lv_point_t point_ = {};
    lv_indev_state_t state_ = LV_INDEV_STATE_RELEASED;

    AccessMode access_mode_ = kAccessBackdoor;
    uint8_t backdoor_read_option_ = 0x03;
    uint32_t report_reg_ = 0x20011120;
    uint16_t native_x_ = 720;
    uint16_t native_y_ = 720;
    uint16_t display_x_ = 720;
    uint16_t display_y_ = 720;
};
