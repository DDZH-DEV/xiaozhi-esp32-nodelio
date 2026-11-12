#include "jd9365tx_touch.h"

#include "config.h"

#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_rom_sys.h>
#include <freertos/idf_additions.h>

#include <cstdint>
#include <cstring>
#include <utility>

namespace {
constexpr const char* kTag = "SilaJdTouch";
constexpr uint32_t kChipIdReg = 0x40008076;
constexpr uint32_t kSoftResetReg = 0x40008004;
constexpr uint32_t kPorInitReg = 0x40008081;
constexpr uint32_t kFwVersionReg = 0x20000F1C;
constexpr uint32_t kResolutionReg = 0x20000904;
// U2HTS/Jadard V1: ERAM base 0x20011000; +0xD8 holds live report pointer;
// default report 0x20011120. Slot = {x_be,y_be,w} × up to 10, empty=0xFFFF.
constexpr uint32_t kCoordAddrPtrReg = 0x200110D8;
constexpr uint32_t kDefaultReportReg = 0x20011120;
constexpr uint16_t kExpectedChipId = 0x9084;
constexpr size_t kReportBytes = 60;
constexpr size_t kReportHeaderBytes = 3;
constexpr size_t kPointStride = 5;
constexpr int kMaxPoints = 10;
// 单指 UI：最多看前 5 槽。tp_num≥8 且槽0 空视为悬空鬼帧，丢弃。
constexpr int kScanSlotCap = 5;
constexpr int kGhostTpNumFloor = 8;
// 轻点往往 1 帧按下即抬起，LVGL 来不及采样会丢点击；抬起前至少保持一帧（验收定稿 25ms；OTA017）。
constexpr TickType_t kMinPressHoldTicks = pdMS_TO_TICKS(25);
constexpr int kI2cTimeoutMs = 200;
constexpr uint32_t kI2cHz = 200000;
}  // namespace

SilaJd9365txTouch::~SilaJd9365txTouch() {
    if (irq_attached_) {
        gpio_isr_handler_remove(TOUCH_IRQ_GPIO);
    }
    if (task_) {
        vTaskDeleteWithCaps(task_);
        task_ = nullptr;
    }
    if (lvgl_port_lock(0)) {
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

esp_err_t SilaJd9365txTouch::EnterBackdoor() {
    ESP_RETURN_ON_FALSE(device_, ESP_ERR_INVALID_STATE, kTag, "touch I2C not ready");
    static const uint8_t command[] = {0xF2, 0xAA, 0x55, 0x0F, 0xF0, 0x68};
    const esp_err_t ret =
        i2c_master_transmit(device_, command, sizeof(command), kI2cTimeoutMs);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ret;
}

esp_err_t SilaJd9365txTouch::SramRead(uint32_t reg, void* data, size_t data_bytes) {
    ESP_RETURN_ON_FALSE(device_ && data && data_bytes, ESP_ERR_INVALID_ARG, kTag,
                        "invalid SRAM read");
    const uint8_t addr[4] = {
        static_cast<uint8_t>(reg >> 24),
        static_cast<uint8_t>(reg >> 16),
        static_cast<uint8_t>(reg >> 8),
        static_cast<uint8_t>(reg),
    };
    ESP_RETURN_ON_ERROR(i2c_master_transmit(device_, addr, sizeof(addr), kI2cTimeoutMs), kTag,
                        "SRAM addr 0x%08lx", static_cast<unsigned long>(reg));
    esp_rom_delay_us(50);
    return i2c_master_receive(device_, static_cast<uint8_t*>(data), data_bytes, kI2cTimeoutMs);
}

esp_err_t SilaJd9365txTouch::RegRead(uint32_t reg, void* data, size_t data_bytes) {
    ESP_RETURN_ON_FALSE(device_ && data && data_bytes, ESP_ERR_INVALID_ARG, kTag,
                        "invalid reg read");
    if ((reg >> 28) == 0x2) {
        return SramRead(reg, data, data_bytes);
    }

    uint8_t request[6];
    if (access_mode_ == kAccessBackdoor) {
        ESP_RETURN_ON_ERROR(EnterBackdoor(), kTag, "enter backdoor");
        request[0] = 0xF3;
        request[1] = static_cast<uint8_t>(reg >> 24);
        request[2] = static_cast<uint8_t>(reg >> 16);
        request[3] = static_cast<uint8_t>(reg >> 8);
        request[4] = static_cast<uint8_t>(reg);
        request[5] = ((reg >> 28) == 0x2) ? 0x01 : backdoor_read_option_;
    } else {
        ESP_RETURN_ON_FALSE(data_bytes <= UINT16_MAX, ESP_ERR_INVALID_SIZE, kTag,
                            "read too large");
        request[0] = static_cast<uint8_t>(reg >> 24);
        request[1] = static_cast<uint8_t>(reg >> 16);
        request[2] = static_cast<uint8_t>(reg >> 8);
        request[3] = static_cast<uint8_t>(reg);
        request[4] = static_cast<uint8_t>(data_bytes >> 8);
        request[5] = static_cast<uint8_t>(data_bytes);
    }
    return i2c_master_transmit_receive(device_, request, sizeof(request),
                                       static_cast<uint8_t*>(data), data_bytes, kI2cTimeoutMs);
}

esp_err_t SilaJd9365txTouch::RegWriteU8(uint32_t reg, uint8_t value) {
    ESP_RETURN_ON_FALSE(device_, ESP_ERR_INVALID_STATE, kTag, "touch I2C not ready");
    uint8_t packet[6];
    size_t packet_bytes;
    if (access_mode_ == kAccessBackdoor) {
        ESP_RETURN_ON_ERROR(EnterBackdoor(), kTag, "enter backdoor");
        packet[0] = 0xF2;
        packet[1] = static_cast<uint8_t>(reg >> 24);
        packet[2] = static_cast<uint8_t>(reg >> 16);
        packet[3] = static_cast<uint8_t>(reg >> 8);
        packet[4] = static_cast<uint8_t>(reg);
        packet[5] = value;
        packet_bytes = 6;
    } else {
        packet[0] = static_cast<uint8_t>(reg >> 24);
        packet[1] = static_cast<uint8_t>(reg >> 16);
        packet[2] = static_cast<uint8_t>(reg >> 8);
        packet[3] = static_cast<uint8_t>(reg);
        packet[4] = value;
        packet_bytes = 5;
    }
    return i2c_master_transmit(device_, packet, packet_bytes, kI2cTimeoutMs);
}

esp_err_t SilaJd9365txTouch::DetectProtocol(uint16_t* chip_id) {
    static const struct {
        AccessMode mode;
        uint8_t option;
    } candidates[] = {
        {kAccessDirect, 0x00},
        {kAccessBackdoor, 0x03},
        {kAccessBackdoor, 0x01},
    };

    esp_err_t last_error = ESP_OK;
    for (int recovery = 0; recovery < 2; ++recovery) {
        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            access_mode_ = candidates[i].mode;
            backdoor_read_option_ = candidates[i].option;

            if (recovery) {
                last_error = RegWriteU8(kSoftResetReg, 0xA5);
                if (last_error == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    last_error = RegWriteU8(kPorInitReg, 0x00);
                }
                if (last_error != ESP_OK) {
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            uint8_t id_frame[4] = {};
            last_error = RegRead(kChipIdReg, id_frame, sizeof(id_frame));
            if (last_error != ESP_OK) {
                continue;
            }
            *chip_id = static_cast<uint16_t>(id_frame[1] << 8) | id_frame[0];
            ESP_LOGI(kTag, "probe mode=%d opt=0x%02X recovery=%d chip_id=0x%04X",
                     static_cast<int>(access_mode_), backdoor_read_option_, recovery, *chip_id);
            if (*chip_id == kExpectedChipId) {
                return ESP_OK;
            }
        }
    }
    return last_error == ESP_OK ? ESP_ERR_INVALID_RESPONSE : last_error;
}

uint16_t SilaJd9365txTouch::ScaleAxis(uint16_t value, uint16_t source_size,
                                      uint16_t target_size) {
    if (source_size <= 1 || target_size <= 1) {
        return value;
    }
    if (value >= source_size) {
        value = source_size - 1;
    }
    return static_cast<uint16_t>(((uint32_t)value * (target_size - 1) + (source_size - 1) / 2) /
                                 (source_size - 1));
}

int SilaJd9365txTouch::ParseReport(const uint8_t* buf, size_t len, int* out_x, int* out_y) {
    if (!buf || !out_x || !out_y || len < kReportHeaderBytes + kPointStride) {
        return -1;
    }
    // V1: [tp_num][frame][event] + slots{x_be,y_be,w}. 实机槽0 偶发 FFFF，
    // 真点在后槽；只认槽0 会导致底栏要点两三下。在 tp_num 内取首个有效槽。
    const uint8_t tp_num = buf[0];
    if (tp_num > kMaxPoints) {
        return -1;
    }
    if (tp_num == 0) {
        return 0;
    }
    const uint16_t slot0_x = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
    const uint16_t slot0_y = (static_cast<uint16_t>(buf[5]) << 8) | buf[6];
    if (tp_num >= kGhostTpNumFloor &&
        (slot0_x == UINT16_MAX || slot0_y == UINT16_MAX)) {
        return 0;
    }
    const int slots_fit =
        static_cast<int>((len - kReportHeaderBytes) / kPointStride);
    int scan = static_cast<int>(tp_num);
    if (scan > kScanSlotCap) {
        scan = kScanSlotCap;
    }
    if (scan > slots_fit) {
        scan = slots_fit;
    }
    for (int i = 0; i < scan; ++i) {
        const size_t off = kReportHeaderBytes + static_cast<size_t>(i) * kPointStride;
        const uint16_t raw_x =
            (static_cast<uint16_t>(buf[off]) << 8) | buf[off + 1];
        const uint16_t raw_y =
            (static_cast<uint16_t>(buf[off + 2]) << 8) | buf[off + 3];
        if (raw_x == UINT16_MAX || raw_y == UINT16_MAX) {
            continue;
        }
        if (raw_x >= native_x_ || raw_y >= native_y_) {
            continue;
        }
        *out_x = ScaleAxis(raw_x, native_x_, display_x_);
        *out_y = ScaleAxis(raw_y, native_y_, display_y_);
        return 1;
    }
    return 0;
}

esp_err_t SilaJd9365txTouch::Start(i2c_master_bus_handle_t bus, lv_display_t* display,
                                   std::function<bool()> on_activity) {
    ESP_RETURN_ON_FALSE(!device_ && bus && display && on_activity, ESP_ERR_INVALID_STATE, kTag,
                        "invalid start state");
    on_activity_ = std::move(on_activity);
    display_x_ = DISPLAY_WIDTH;
    display_y_ = DISPLAY_HEIGHT;
    native_x_ = DISPLAY_WIDTH;
    native_y_ = DISPLAY_HEIGHT;

    // Only probe 0x68. Never write AXP2101 (0x34) on this shared bus.
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_I2C_ADDRESS, 100), kTag,
                        "probe touch 0x%02X", TOUCH_I2C_ADDRESS);

    i2c_device_config_t device_config = {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = TOUCH_I2C_ADDRESS;
    device_config.scl_speed_hz = kI2cHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &device_), kTag,
                        "add touch device");

    uint16_t chip_id = 0;
    esp_err_t ret = DetectProtocol(&chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "JD9365TX chip id not found: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return ret;
    }
    ESP_LOGI(kTag, "JD9365TX In-Cell confirmed chip_id=0x%04X", chip_id);

    // Shared RST already pulsed with LCD. Soft-reset TP firmware params only.
    ret = RegWriteU8(kSoftResetReg, 0xA5);
    if (ret == ESP_OK) {
        ret = RegWriteU8(kPorInitReg, 0x00);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "TP soft reset failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t resolution[4] = {};
    if (RegRead(kResolutionReg, resolution, sizeof(resolution)) == ESP_OK) {
        const uint16_t nx = (static_cast<uint16_t>(resolution[1]) << 8) | resolution[0];
        const uint16_t ny = (static_cast<uint16_t>(resolution[3]) << 8) | resolution[2];
        if (nx > 0 && nx <= 8192 && ny > 0 && ny <= 8192) {
            native_x_ = nx;
            native_y_ = ny;
        }
    }
    uint8_t fw_version[8] = {};
    if (RegRead(kFwVersionReg, fw_version, sizeof(fw_version)) == ESP_OK) {
        ESP_LOGI(kTag, "TP fw %02X%02X%02X%02X native=%ux%u", fw_version[0], fw_version[1],
                 fw_version[2], fw_version[3], native_x_, native_y_);
    }

    report_reg_ = kDefaultReportReg;
    uint8_t coord_ptr[4] = {};
    if (RegRead(kCoordAddrPtrReg, coord_ptr, sizeof(coord_ptr)) == ESP_OK) {
        const uint32_t le = static_cast<uint32_t>(coord_ptr[0]) |
                            (static_cast<uint32_t>(coord_ptr[1]) << 8) |
                            (static_cast<uint32_t>(coord_ptr[2]) << 16) |
                            (static_cast<uint32_t>(coord_ptr[3]) << 24);
        const uint32_t be = static_cast<uint32_t>(coord_ptr[3]) |
                            (static_cast<uint32_t>(coord_ptr[2]) << 8) |
                            (static_cast<uint32_t>(coord_ptr[1]) << 16) |
                            (static_cast<uint32_t>(coord_ptr[0]) << 24);
        auto looks_sram = [](uint32_t r) {
            return (r >> 28) == 0x2 && r >= 0x20010000 && r < 0x20020000;
        };
        if (looks_sram(le)) {
            report_reg_ = le;
        } else if (looks_sram(be)) {
            report_reg_ = be;
        }
        ESP_LOGI(kTag, "coord ptr raw %02X%02X%02X%02X → report=0x%08lX", coord_ptr[0],
                 coord_ptr[1], coord_ptr[2], coord_ptr[3],
                 static_cast<unsigned long>(report_reg_));
    } else {
        ESP_LOGW(kTag, "coord ptr read fail; using default report=0x%08lX",
                 static_cast<unsigned long>(report_reg_));
    }

    gpio_config_t irq_config = {};
    irq_config.pin_bit_mask = 1ULL << TOUCH_IRQ_GPIO;
    irq_config.mode = GPIO_MODE_INPUT;
    irq_config.pull_up_en = GPIO_PULLUP_ENABLE;
    irq_config.intr_type = GPIO_INTR_NEGEDGE;
    ESP_RETURN_ON_ERROR(gpio_config(&irq_config), kTag, "configure touch IRQ");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, kTag,
                        "install GPIO ISR");

    ESP_RETURN_ON_FALSE(lvgl_port_lock(1000), ESP_ERR_TIMEOUT, kTag, "LVGL lock");
    indev_ = lv_indev_create();
    if (indev_) {
        lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(indev_, display);
        lv_indev_set_user_data(indev_, this);
        lv_indev_set_read_cb(indev_, ReadCache);
        // TIMER + wake: never run lv_indev_read / lv_event_send on this task stack.
        lv_indev_set_mode(indev_, LV_INDEV_MODE_TIMER);
    }
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(indev_, ESP_ERR_NO_MEM, kTag, "create LVGL indev");

    // Depth is StackType_t words (×4 on P4). 2048 → 8 KiB in SPIRAM — keeps internal
    // DMA heap free for Hosted/DPI. Never lv_indev_read here; only lvgl_port_task_wake.
    ESP_RETURN_ON_FALSE(
        xTaskCreateWithCaps([](void* arg) { static_cast<SilaJd9365txTouch*>(arg)->Run(); },
                            "jd9365tx_tp", 2048, this, 5, &task_,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS,
        ESP_ERR_NO_MEM, kTag, "create touch task");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(TOUCH_IRQ_GPIO, OnInterrupt, this), kTag,
                        "attach touch IRQ");
    irq_attached_ = true;
    if (gpio_get_level(TOUCH_IRQ_GPIO) == 0) {
        xTaskNotifyGive(task_);
    }
    ESP_LOGI(kTag, "In-Cell touch ready addr=0x%02X irq=%d scan_cap=%d", TOUCH_I2C_ADDRESS,
             TOUCH_IRQ_GPIO, kScanSlotCap);
    return ESP_OK;
}

void SilaJd9365txTouch::OnInterrupt(void* arg) {
    auto* self = static_cast<SilaJd9365txTouch*>(arg);
    BaseType_t task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(self->task_, &task_woken);
    portYIELD_FROM_ISR(task_woken);
}

void SilaJd9365txTouch::ReadCache(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<SilaJd9365txTouch*>(lv_indev_get_user_data(indev));
    portENTER_CRITICAL(&self->spinlock_);
    data->point = self->point_;
    data->state = self->state_;
    portEXIT_CRITICAL(&self->spinlock_);
}

void SilaJd9365txTouch::Publish(bool pressed, int x, int y) {
    /* Update cache only; wake LVGL task. Never lv_event_send on 4KB touch stack. */
    portENTER_CRITICAL(&spinlock_);
    point_.x = x;
    point_.y = y;
    state_ = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    portEXIT_CRITICAL(&spinlock_);

    if (indev_) {
        lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, indev_);
    }
}

void SilaJd9365txTouch::Run() {
    int last_x = 0;
    int last_y = 0;
    bool reported_pressed = false;
    TickType_t press_at = 0;
    uint32_t read_errors = 0;
    uint8_t buf[kReportBytes];
    for (;;) {
        const bool by_int = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) > 0;
        const bool int_low = gpio_get_level(TOUCH_IRQ_GPIO) == 0;
        if (!by_int && !int_low) {
            // INT idle: still complete a pending short-tap release.
            if (reported_pressed &&
                (xTaskGetTickCount() - press_at) >= kMinPressHoldTicks) {
                reported_pressed = false;
                suppress_gesture_ = false;
                Publish(false, last_x, last_y);
            }
            continue;
        }

        std::memset(buf, 0, sizeof(buf));
        const esp_err_t rd = RegRead(report_reg_, buf, sizeof(buf));
        if (rd != ESP_OK) {
            if (++read_errors <= 5) {
                ESP_LOGW(kTag, "report read fail %lu/5: %s INT=%d",
                         static_cast<unsigned long>(read_errors), esp_err_to_name(rd),
                         gpio_get_level(TOUCH_IRQ_GPIO));
            }
            continue;
        }

        int x = 0;
        int y = 0;
        const int n = ParseReport(buf, sizeof(buf), &x, &y);
        if (n < 0) {
            continue;
        }

        const bool contact = n > 0;
        if (contact) {
            last_x = x;
            last_y = y;
            if (!reported_pressed) {
                suppress_gesture_ = on_activity_() || suppress_gesture_;
                reported_pressed = true;
                press_at = xTaskGetTickCount();
            }
            Publish(!suppress_gesture_, last_x, last_y);
            continue;
        }

        if (!reported_pressed) {
            continue;
        }
        // Short tap: stretch PRESSED so LVGL samples it, then release.
        const TickType_t held = xTaskGetTickCount() - press_at;
        if (held < kMinPressHoldTicks) {
            Publish(!suppress_gesture_, last_x, last_y);
            vTaskDelay(kMinPressHoldTicks - held);
        }
        reported_pressed = false;
        suppress_gesture_ = false;
        Publish(false, last_x, last_y);
    }
}
