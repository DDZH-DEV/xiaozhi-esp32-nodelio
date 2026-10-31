#include "axs15260_touch.h"

#include "config.h"

#include <esp_check.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#include <algorithm>
#include <iterator>
#include <utility>

namespace {
constexpr const char* kTag = "SilaTouch";
}

SilaAxs15260Touch::~SilaAxs15260Touch() {
    if (irq_attached_) {
        gpio_isr_handler_remove(TOUCH_IRQ_GPIO);
    }
    if (task_) {
        vTaskDelete(task_);
    }
    if (lvgl_port_lock(0)) {
        lv_async_call_cancel(ReadIndevAsync, this);
        read_pending_ = false;
        if (indev_) {
            lv_indev_delete(indev_);
            indev_ = nullptr;
        }
        lvgl_port_unlock();
    }
    if (device_) {
        i2c_master_bus_rm_device(device_);
    }
}

esp_err_t SilaAxs15260Touch::Start(i2c_master_bus_handle_t bus, lv_display_t* display,
                                   std::function<bool()> on_activity) {
    ESP_RETURN_ON_FALSE(!device_ && display && on_activity, ESP_ERR_INVALID_STATE, kTag,
                        "Invalid start state");
    on_activity_ = std::move(on_activity);
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_I2C_ADDRESS, 50), kTag, "Probe touch address");
    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = TOUCH_I2C_ADDRESS;
    device_config.scl_speed_hz = 100000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &device_), kTag,
                        "Add touch device");

    // GPIO26 has already reset the LCD and touch together. Do not pulse it here.
    gpio_config_t irq_config = {};
    irq_config.pin_bit_mask = 1ULL << TOUCH_IRQ_GPIO;
    irq_config.mode = GPIO_MODE_INPUT;
    irq_config.pull_up_en = GPIO_PULLUP_ENABLE;
    irq_config.intr_type = GPIO_INTR_NEGEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), kTag, "Configure touch IRQ");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, kTag,
                        "Install GPIO interrupt service");

    ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, kTag, "LVGL lock");
    indev_ = lv_indev_create();
    if (indev_) {
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev_, display);
        lv_indev_set_user_data(indev_, this);
        lv_indev_set_read_cb(indev_, ReadCache);
        lv_indev_set_mode(indev_, LV_INDEV_MODE_EVENT);
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(indev_, ESP_ERR_NO_MEM, kTag, "Create LVGL input");
    // Driver task uses an internal stack. No HTTP, application or UI work here.
    ESP_RETURN_ON_FALSE(xTaskCreate([](void* arg) { static_cast<SilaAxs15260Touch*>(arg)->Run(); },
                                    "sila_touch", 4096, this, 4, &task_) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "Create touch task");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(TOUCH_IRQ_GPIO, OnInterrupt, this), kTag,
                        "Attach touch interrupt");
    irq_attached_ = true;
    return ESP_OK;
}

void SilaAxs15260Touch::OnInterrupt(void* arg) {
    auto* self = static_cast<SilaAxs15260Touch*>(arg);
    BaseType_t task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->task_, &task_woken);
    portYIELD_FROM_ISR(task_woken);
}

void SilaAxs15260Touch::ReadCache(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<SilaAxs15260Touch*>(lv_indev_get_user_data(indev));
    data->point = self->point_;
    data->state = self->state_;
}

void SilaAxs15260Touch::ReadIndevAsync(void* arg) {
    auto* self = static_cast<SilaAxs15260Touch*>(arg);
    self->read_pending_ = false;
    if (self->indev_) {
        lv_indev_read(self->indev_);
    }
}

void SilaAxs15260Touch::Publish(bool pressed, int x, int y) {
    /* 只更新缓存。lv_indev_read 会派发点击回调，禁止在 4KB sila_touch 栈上跑。 */
    if (lvgl_port_lock(0)) {
        point_.x = x;
        point_.y = y;
        state_ = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        if (indev_ && !read_pending_) {
            read_pending_ = true;
            if (lv_async_call(ReadIndevAsync, this) != LV_RESULT_OK) {
                read_pending_ = false;
            }
        }
        lvgl_port_unlock();
    }
}

void SilaAxs15260Touch::Run() {
    int last_x = 0;
    int last_y = 0;
    bool was_pressed = false;
    int consecutive_errors = 0;
    bool reported_error = false;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        uint8_t frame[8] = {};
        // AXS15260 requires an IRQ-driven raw read, not a register-address write
        // or idle polling (which can advance its internal pointer).
        const esp_err_t err = i2c_master_receive(device_, frame, sizeof(frame), 50);
        const bool empty = std::all_of(std::begin(frame), std::end(frame),
                                       [](uint8_t byte) { return byte == 0xFF; });
        const int count = frame[1] & 0x0F;
        const int event = frame[2] >> 6;
        const int x = ((frame[2] & 0x0F) << 8) | frame[3];
        const int y = ((frame[4] & 0x0F) << 8) | frame[5];
        const bool valid = frame[0] == 0 && (frame[1] & 0xF0) == 0 && count <= 5 &&
                           (count == 0 || (event <= 2 && x < DISPLAY_PHYSICAL_WIDTH &&
                                           y < DISPLAY_PHYSICAL_HEIGHT));
        if (err != ESP_OK || (!empty && !valid)) {
            consecutive_errors++;
            if (!reported_error) {
                ESP_LOGW(kTag, "Touch read/frame error (%s); keep last point",
                         esp_err_to_name(err));
                reported_error = true;
            }
            /* 单帧坏包勿抬手/勿掐整段手势，否则点按像卡顿；连续多次才强制释放防粘滞 */
            if (consecutive_errors >= 5 && was_pressed) {
                was_pressed = false;
                Publish(false, last_x, last_y);
            }
        } else if (empty) {
            consecutive_errors = 0;
            reported_error = false;
            if (was_pressed) {
                was_pressed = false;
                Publish(false, last_x, last_y);
                suppress_gesture_ = false;
            }
        } else {
            consecutive_errors = 0;
            reported_error = false;
            const bool pressed = count > 0 && event != 1;
            if (pressed) {
                last_x = x;
                last_y = y;
                suppress_gesture_ = on_activity_() || suppress_gesture_;
            }
            const bool report = pressed && !suppress_gesture_;
            was_pressed = report;
            Publish(report, last_x, last_y);
            if (!pressed) {
                suppress_gesture_ = false;
                was_pressed = false;
            }
        }
        // Bound CPU use even with a noisy interrupt line.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
