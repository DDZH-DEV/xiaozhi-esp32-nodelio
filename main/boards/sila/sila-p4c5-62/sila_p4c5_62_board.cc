#include "application.h"
#include "audio/codecs/es8389_audio_codec.h"
#include "axs15260_init.h"
#include "axs15260_touch.h"
#include "backlight.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "ppa_fill.h"
#include "wifi_board.h"
#if CONFIG_NODELIO_PANEL
#include "board_info.h"
#include "media_player.h"
#include "nvs_config.h"
#include "panel_ui.h"
#include "skill_result_ui.h"
#endif

#include <driver/gpio.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

// Hosted streaming RX needs contiguous DMA buffers after display/audio startup.
// Reject stale sdkconfig files instead of reaching an allocation abort at boot.
#if !CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM || !CONFIG_CACHE_L2_CACHE_LINE_64B
#error \
    "SILA P4C5 requires Hosted DMA PSRAM and 64-byte L2 cache lines; rebuild with scripts/build.py"
#endif

namespace {
constexpr const char* kTag = "SilaP4C5";
#if CONFIG_NODELIO_P4_FULL_DRAW_BUFFERS
constexpr uint32_t kDrawBufferPixels = DISPLAY_PHYSICAL_WIDTH * DISPLAY_PHYSICAL_HEIGHT;
#else
constexpr uint32_t kDrawBufferPixels = DISPLAY_PHYSICAL_WIDTH * DISPLAY_PHYSICAL_HEIGHT / 4;
#endif
#if CONFIG_LVGL_PORT_ENABLE_PPA
constexpr const char* kRotationBackend = "PPA";
#else
constexpr const char* kRotationBackend = "CPU";
#endif
// esp_lvgl_port 2.9's rotation path describes packed RGB888 rows to PPA.
static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1, "P4 rotation requires packed pixel rows");

void LogMemory(const char* stage) {
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "%s: internal free/min/largest=%u/%u/%u, PSRAM free/min/largest=%u/%u/%u", stage,
             unsigned(heap_caps_get_free_size(internal)),
             unsigned(heap_caps_get_minimum_free_size(internal)),
             unsigned(heap_caps_get_largest_free_block(internal)),
             unsigned(heap_caps_get_free_size(external)),
             unsigned(heap_caps_get_minimum_free_size(external)),
             unsigned(heap_caps_get_largest_free_block(external)));
    const uint32_t dma_internal = internal | MALLOC_CAP_DMA;
    const uint32_t dma_external = external | MALLOC_CAP_DMA;
    ESP_LOGI(kTag, "%s: DMA internal free/largest=%u/%u, DMA PSRAM free/largest=%u/%u", stage,
             unsigned(heap_caps_get_free_size(dma_internal)),
             unsigned(heap_caps_get_largest_free_block(dma_internal)),
             unsigned(heap_caps_get_free_size(dma_external)),
             unsigned(heap_caps_get_largest_free_block(dma_external)));
}

// The generic MipiLcdDisplay uses RGB565. This panel needs RGB888 throughout.
class SilaP4Display final : public LcdDisplay {
public:
    SilaP4Display(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, Backlight& backlight)
        : LcdDisplay(io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT), backlight_(backlight) {
        lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
        port_config.task_stack = 16384;
        port_config.task_affinity = 1;  /* LVGL on core1; Hosted/audio stay free to use core0 */
        ESP_ERROR_CHECK(lvgl_port_init(&port_config));
        bool fill_acceleration = false;
        if (lvgl_port_lock(0)) {
            fill_acceleration = SilaPpaFillInit();
            lvgl_port_unlock();
        }
        lvgl_port_display_cfg_t config = {};
        config.io_handle = io;
        config.panel_handle = panel;  // Actual IDF DPI handle, never a vendor wrapper.
        config.buffer_size = kDrawBufferPixels;
        config.double_buffer = true;
        config.hres = DISPLAY_PHYSICAL_WIDTH;
        config.vres = DISPLAY_PHYSICAL_HEIGHT;
        config.color_format = LV_COLOR_FORMAT_RGB888;
        config.flags.buff_spiram = true;
        config.flags.sw_rotate = true;
        // DMA, direct_mode and full_refresh are deliberately false with rotation.
        lvgl_port_display_dsi_cfg_t dsi_config = {};
        display_ = lvgl_port_add_disp_dsi(&config, &dsi_config);
        ESP_ERROR_CHECK(display_ ? ESP_OK : ESP_ERR_NO_MEM);
        if (lvgl_port_lock(0)) {
            lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_90);
#if CONFIG_NODELIO_P4_RENDER_STATS
            stats_report_at_ = esp_timer_get_time();
            lv_display_add_event_cb(display_, OnRefreshEvent, LV_EVENT_REFR_START, this);
            lv_display_add_event_cb(display_, OnRefreshEvent, LV_EVENT_RENDER_START, this);
            lv_display_add_event_cb(display_, OnRefreshEvent, LV_EVENT_REFR_READY, this);
#endif
            lvgl_port_unlock();
        }
        /* vendor PPA_IMG 依赖 LV_USE_PPA；关 PPA 时宏可能根本不生成 */
#if defined(LV_USE_PPA)
        constexpr int kVendorPpa = LV_USE_PPA;
#else
        constexpr int kVendorPpa = 0;
#endif
#if defined(LV_USE_PPA_IMG)
        constexpr int kImagePpa = LV_USE_PPA_IMG;
#else
        constexpr int kImagePpa = 0;
#endif
        ESP_LOGI(kTag,
                 "LVGL %d.%d.%d: RGB888 partial, draw=%lu pixels x2, rotate=%s, "
                 "board_fill_PPA=%d vendor_PPA=%d image_PPA=%d align=%d stride_align=%d",
                 LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                 static_cast<unsigned long>(kDrawBufferPixels), kRotationBackend, fill_acceleration,
                 kVendorPpa, kImagePpa, LV_DRAW_BUF_ALIGN, LV_DRAW_BUF_STRIDE_ALIGN);
        LogMemory("display buffers");
    }

    void SetupUI() override {
        LcdDisplay::SetupUI();
        if (lvgl_port_lock(0)) {
            lv_refr_now(display_);
            lvgl_port_unlock();
        }
        backlight_.RestoreBrightness();
        LogMemory("first UI");
    }

    lv_display_t* Handle() const { return display_; }

    bool SetIdleRefresh(bool idle) {
        if (!lvgl_port_lock(100)) {
            return false;
        }
#if CONFIG_NODELIO_PANEL
        lv_timer_set_period(
            lv_display_get_refr_timer(display_),
            idle ? CONFIG_NODELIO_UI_IDLE_REFRESH_MS : CONFIG_NODELIO_UI_ACTIVE_REFRESH_MS);
#else
        lv_timer_set_period(lv_display_get_refr_timer(display_), idle ? 200 : 30);
#endif
        lvgl_port_unlock();
        return true;
    }

private:
#if CONFIG_NODELIO_P4_RENDER_STATS
    // All fields are owned by the LVGL task/display lock. No sampling task or
    // forced invalidations: a static or sleeping screen stays quiet.
    int64_t stats_report_at_ = 0;
    int64_t refresh_started_at_ = 0;
    int64_t refresh_total_us_ = 0;
    int64_t refresh_max_us_ = 0;
    uint32_t refresh_count_ = 0;
    bool refresh_drew_ = false;

    static void OnRefreshEvent(lv_event_t* event) {
        auto* self = static_cast<SilaP4Display*>(lv_event_get_user_data(event));
        switch (lv_event_get_code(event)) {
            case LV_EVENT_REFR_START:
                self->refresh_started_at_ = esp_timer_get_time();
                self->refresh_drew_ = false;
                break;
            case LV_EVENT_RENDER_START:
                self->refresh_drew_ = true;
                break;
            case LV_EVENT_REFR_READY: {
                if (!self->refresh_drew_) {
                    break;
                }
                const int64_t now = esp_timer_get_time();
                const int64_t elapsed = now - self->refresh_started_at_;
                self->refresh_total_us_ += elapsed;
                self->refresh_max_us_ = std::max(self->refresh_max_us_, elapsed);
                ++self->refresh_count_;
                if (now - self->stats_report_at_ >= 5000000) {
                    ESP_LOGI(kTag, "refresh samples=%lu wall_us avg=%lld max=%lld",
                             static_cast<unsigned long>(self->refresh_count_),
                             static_cast<long long>(self->refresh_total_us_ / self->refresh_count_),
                             static_cast<long long>(self->refresh_max_us_));
                    LogMemory("refresh window");
                    const auto fills = SilaPpaFillGetStats();
                    ESP_LOGI(kTag, "PPA fill total=%lu fallback=%lu",
                             static_cast<unsigned long>(fills.completed),
                             static_cast<unsigned long>(fills.fallback));
                    self->stats_report_at_ = now;
                    self->refresh_total_us_ = 0;
                    self->refresh_max_us_ = 0;
                    self->refresh_count_ = 0;
                }
                break;
            }
            default:
                break;
        }
    }
#endif
    Backlight& backlight_;
};
}  // namespace

class SilaP4C562Board final : public WifiBoard {
private:
    // Board singleton owns buses and the display for the lifetime of the firmware.
    // Devices borrow buses; optional touch never resets/deletes a shared bus.
    i2c_master_bus_handle_t codec_bus_ = nullptr;
    i2c_master_bus_handle_t peripheral_bus_ = nullptr;
    esp_ldo_channel_handle_t phy_power_ = nullptr;
    esp_lcd_dsi_bus_handle_t dsi_bus_ = nullptr;
    PwmBacklight backlight_{DISPLAY_BACKLIGHT_PIN, false, 20000, 0};
    Button boot_button_{BOOT_BUTTON_GPIO};
    std::unique_ptr<SilaAxs15260Touch> touch_;
    i2c_master_dev_handle_t pmu_dev_ = nullptr;
    bool pmu_ready_ = false;
    bool batt_present_ = false;
    bool batt_absent_logged_ = false;
    bool pmu_read_warned_ = false;
    int batt_level_ = -1;
    bool batt_charging_ = false;
    bool batt_discharging_ = false;
    int64_t batt_cached_at_ = 0;
    std::atomic<bool> pmu_irq_pending_{false};
    bool pmu_irq_attached_ = false;
    i2c_master_dev_handle_t accel_dev_ = nullptr;
    bool accel_ready_ = false;
    bool accel_have_sample_ = false;
    bool accel_face_down_muted_ = false;
    float accel_prev_mag_ = 0.f;
    int64_t accel_last_sample_us_ = 0;
    SilaP4Display* display_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;
    std::atomic<TickType_t> last_activity_{0};
    std::atomic<bool> asleep_{false};
    std::atomic<bool> poll_pending_{false};
    std::atomic<int> idle_seconds_{60};
    std::atomic<int> idle_wifi_seconds_{60};
    std::atomic<bool> net_save_{true};
    std::atomic<bool> net_asleep_{false};
    std::atomic<int64_t> goodbye_sleep_at_{0};
    bool idle_timer_started_ = false;

    static void CreateBus(i2c_port_t port, gpio_num_t sda, gpio_num_t scl,
                          i2c_master_bus_handle_t* result) {
        i2c_master_bus_config_t config = {};
        config.i2c_port = port;
        config.sda_io_num = sda;
        config.scl_io_num = scl;
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        config.flags.enable_internal_pullup = true;
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, result));
    }

    void InitializeDisplay() {
        gpio_config_t reset = {};
        reset.pin_bit_mask = 1ULL << DISPLAY_RESET_PIN;
        reset.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&reset));
        ESP_ERROR_CHECK(gpio_set_level(DISPLAY_RESET_PIN, 0));
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(gpio_set_level(DISPLAY_RESET_PIN, 1));
        vTaskDelay(pdMS_TO_TICKS(170));

        esp_ldo_channel_config_t ldo = {};
        ldo.chan_id = 3;
        ldo.voltage_mv = 2500;
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &phy_power_));
        esp_lcd_dsi_bus_config_t bus = {};
        bus.bus_id = 0;
        bus.num_data_lanes = 2;
        bus.lane_bit_rate_mbps = 1000;
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus, &dsi_bus_));
        esp_lcd_dbi_io_config_t dbi = {};
        dbi.virtual_channel = 0;
        dbi.lcd_cmd_bits = 8;
        dbi.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi, &io));
        // DBI initialization must finish before starting DPI video mode.
        ESP_ERROR_CHECK(sila_axs15260_init(io));

        esp_lcd_dpi_panel_config_t dpi = {};
        dpi.virtual_channel = 0;
        dpi.dpi_clock_freq_mhz = 48;
        dpi.in_color_format = LCD_COLOR_FMT_RGB888;
        dpi.out_color_format = LCD_COLOR_FMT_RGB888;
        dpi.num_fbs = 2;
        dpi.video_timing.h_size = DISPLAY_PHYSICAL_WIDTH;
        dpi.video_timing.v_size = DISPLAY_PHYSICAL_HEIGHT;
        dpi.video_timing.hsync_back_porch = 90;
        dpi.video_timing.hsync_pulse_width = 10;
        dpi.video_timing.hsync_front_porch = 90;
        dpi.video_timing.vsync_back_porch = 10;
        dpi.video_timing.vsync_pulse_width = 50;
        dpi.video_timing.vsync_front_porch = 250;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_dpi(dsi_bus_, &dpi, &panel));
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        display_ = new SilaP4Display(io, panel, backlight_);
        LogMemory("display initialized");
    }

    void ScheduleIdlePoll() {
        if (!poll_pending_.exchange(true)) {
            Application::GetInstance().Schedule([this]() {
                // Clear first: an activity arriving during the check can queue
                // one more check, so a wake request is never lost.
                poll_pending_.store(false);
                const TickType_t elapsed = xTaskGetTickCount() - last_activity_.load();
                const int64_t goodbye_at = goodbye_sleep_at_.load();
                const bool timeout = elapsed >= pdMS_TO_TICKS(idle_seconds_.load() * 1000) ||
                                     (goodbye_at > 0 && esp_timer_get_time() >= goodbye_at);
                const bool can_sleep = Application::GetInstance().CanEnterSleepMode();
                /* 提示音会让播放队列非空，CanEnterSleepMode 短暂为假。
                 * 已息屏时不要因此 RestoreBrightness，否则每声提醒亮一下再灭。
                 * 只有新的触摸/活动（timeout 被清掉）才点亮。 */
                bool next_asleep = asleep_.load();
                if (!timeout) {
                    next_asleep = false;
                } else if (can_sleep) {
                    next_asleep = true;
                }
                if (next_asleep != asleep_.load() && display_->SetIdleRefresh(next_asleep)) {
                    asleep_.store(next_asleep);
                    if (next_asleep) {
                        backlight_.SetBrightness(0);
                    } else {
                        backlight_.RestoreBrightness();
                    }
#if CONFIG_NODELIO_PANEL
                    DisplayLockGuard lock(display_);
                    if (lock) {
                        nodelio_panel_ui_notify_screen(!next_asleep);
                    }
#endif
                }
                HandlePmuIrq();
                SampleAccelerometer();
#if CONFIG_NODELIO_PANEL
                const bool playing = nodelio_media_is_playing();
                const bool hold_net = nodelio_skill_support_holds_network();
                const bool want_net = net_save_.load() && !playing && !hold_net &&
                                      elapsed >= pdMS_TO_TICKS(idle_wifi_seconds_.load() * 1000) &&
                                      Application::GetInstance().CanEnterSleepMode();
                if (want_net) {
                    if (!net_asleep_.exchange(true)) {
                        nodelio_panel_ui_notify_net(false);
                    }
                } else if (net_asleep_.exchange(false)) {
                    nodelio_panel_ui_notify_net(true);
                }
#endif
            });
        }
    }

    static void PmuIrqIsr(void* arg) {
        // 仅置位；读 I2C / 刷新顶栏放在 1s idle 轮询里，ISR 不可拿 mutex。
        static_cast<SilaP4C562Board*>(arg)->pmu_irq_pending_.store(true);
    }

    void HandlePmuIrq() {
        if (!pmu_ready_ || !pmu_irq_pending_.exchange(false)) {
            return;
        }
        // Clear AXP2101 IRQ status (write 1s). Registers: INTSTS1..3 = 0x48..0x4A.
        WritePmuReg(0x48, 0xFF);
        WritePmuReg(0x49, 0xFF);
        WritePmuReg(0x4A, 0xFF);
        batt_cached_at_ = 0;
        int level = 0;
        bool charging = false;
        bool discharging = false;
        (void)GetBatteryLevel(level, charging, discharging);
#if CONFIG_NODELIO_PANEL
        // 息屏只更新缓存，不点亮背光。
        nodelio_panel_ui_refresh_battery();
#endif
    }

    void EnablePmuIrq() {
        if (!pmu_ready_ || pmu_irq_attached_) {
            return;
        }
        // INTEN1 0x40: VBUS insert/remove + battery insert/remove
        // INTEN2 0x41: enter/exit charge + charge done
        WritePmuReg(0x40, 0x0F);
        WritePmuReg(0x41, 0x07);
        WritePmuReg(0x42, 0x00);
        WritePmuReg(0x48, 0xFF);
        WritePmuReg(0x49, 0xFF);
        WritePmuReg(0x4A, 0xFF);

        gpio_config_t irq = {};
        irq.pin_bit_mask = 1ULL << PMU_IRQ_GPIO;
        irq.mode = GPIO_MODE_INPUT;
        irq.pull_up_en = GPIO_PULLUP_ENABLE;
        irq.intr_type = GPIO_INTR_NEGEDGE;
        if (gpio_config(&irq) != ESP_OK) {
            ESP_LOGW(kTag, "PMU IRQ gpio config failed");
            return;
        }
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "PMU IRQ isr service: %s", esp_err_to_name(err));
            return;
        }
        if (gpio_isr_handler_add(PMU_IRQ_GPIO, PmuIrqIsr, this) != ESP_OK) {
            ESP_LOGW(kTag, "PMU IRQ handler add failed");
            return;
        }
        pmu_irq_attached_ = true;
        ESP_LOGI(kTag, "AXP2101 IRQ on GPIO%d", static_cast<int>(PMU_IRQ_GPIO));
    }

    bool ReadAccelReg(uint8_t reg, uint8_t* value) {
        if (!accel_dev_ || !value) {
            return false;
        }
        return i2c_master_transmit_receive(accel_dev_, &reg, 1, value, 1, 50) == ESP_OK;
    }

    bool WriteAccelReg(uint8_t reg, uint8_t value) {
        if (!accel_dev_) {
            return false;
        }
        const uint8_t buf[2] = {reg, value};
        return i2c_master_transmit(accel_dev_, buf, sizeof(buf), 50) == ESP_OK;
    }

    void AttachAccelerometer() {
        constexpr uint8_t kAddrs[] = {0x18, 0x19};
        constexpr uint8_t kWhoAmI = 0x0F;
        constexpr uint8_t kId = 0x11;
        for (uint8_t addr : kAddrs) {
            if (i2c_master_probe(peripheral_bus_, addr, 50) != ESP_OK) {
                continue;
            }
            i2c_device_config_t device = {};
            device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
            device.device_address = addr;
            device.scl_speed_hz = 100000;
            i2c_master_dev_handle_t handle = nullptr;
            if (i2c_master_bus_add_device(peripheral_bus_, &device, &handle) != ESP_OK) {
                continue;
            }
            accel_dev_ = handle;
            uint8_t id = 0;
            if (!ReadAccelReg(kWhoAmI, &id) || id != kId) {
                ESP_LOGW(kTag, "accel 0x%02X whoami=0x%02X (want 0x%02X); disabled", addr, id, kId);
                i2c_master_bus_rm_device(accel_dev_);
                accel_dev_ = nullptr;
                continue;
            }
            // CTRL_REG1 100Hz XYZ; CTRL_REG4 ±2g
            if (!WriteAccelReg(0x20, 0x57) || !WriteAccelReg(0x23, 0x00)) {
                ESP_LOGW(kTag, "accel 0x%02X configure failed", addr);
                i2c_master_bus_rm_device(accel_dev_);
                accel_dev_ = nullptr;
                continue;
            }
            accel_ready_ = true;
            ESP_LOGI(kTag, "SC7A20 at 0x%02X", addr);
            return;
        }
        ESP_LOGI(kTag, "SC7A20 not found; pick-up wake disabled");
    }

    bool ReadAccelAxes(float* x, float* y, float* z) {
        if (!accel_ready_ || !x || !y || !z) {
            return false;
        }
        // Auto-increment from OUT_X_L
        uint8_t reg = 0x28 | 0x80;
        uint8_t raw[6] = {};
        if (i2c_master_transmit_receive(accel_dev_, &reg, 1, raw, 6, 50) != ESP_OK) {
            return false;
        }
        const auto to_g = [](uint8_t lo, uint8_t hi) {
            const int16_t v = static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
            return static_cast<float>(v) / 16384.f;
        };
        *x = to_g(raw[0], raw[1]);
        *y = to_g(raw[2], raw[3]);
        *z = to_g(raw[4], raw[5]);
        return true;
    }

    void SampleAccelerometer() {
        if (!accel_ready_) {
            return;
        }
        // 只在息屏采样：拿起亮屏 / 面朝下静音；亮屏沿用触摸与空闲计时。
        if (!asleep_.load()) {
            accel_have_sample_ = false;
            return;
        }
#if CONFIG_NODELIO_PANEL
        // 息屏语音唤醒开着时不采样，避免与唤醒/触摸抢 I2C。
        if (nodelio_wake_on_sleep_enabled()) {
            accel_have_sample_ = false;
            return;
        }
#endif
        const int64_t now = esp_timer_get_time();
        if (now - accel_last_sample_us_ < 200000) {
            return;
        }
        accel_last_sample_us_ = now;
        float ax = 0, ay = 0, az = 0;
        if (!ReadAccelAxes(&ax, &ay, &az)) {
            return;
        }
        const float mag = std::sqrt(ax * ax + ay * ay + az * az);
        const bool face_down = az < -0.7f && mag > 0.6f && mag < 1.4f;
        if (face_down && !accel_face_down_muted_) {
            accel_face_down_muted_ = true;
            if (auto* codec = GetAudioCodec()) {
                codec->SetOutputVolume(0);
            }
            ESP_LOGI(kTag, "face-down mute");
        } else if (!face_down) {
            accel_face_down_muted_ = false;
        }
        // 加速度突变或明显倾斜视为拿起 → 等同触摸唤醒；放下不另写睡眠。
        if (accel_have_sample_) {
            const float delta = std::fabs(mag - accel_prev_mag_);
            const bool tilted = std::fabs(az) < 0.55f && mag > 0.7f;
            if (delta > 0.35f || tilted) {
                ESP_LOGI(kTag, "pick-up wake d=%.2f z=%.2f", delta, az);
                NoteUserActivity();
            }
        }
        accel_prev_mag_ = mag;
        accel_have_sample_ = true;
    }

public:
    SilaP4C562Board() {
        backlight_.PreloadSavedBrightness();
        esp_chip_info_t chip = {};
        esp_chip_info(&chip);
        uint32_t flash_bytes = 0;
        ESP_ERROR_CHECK(esp_flash_get_size(nullptr, &flash_bytes));
        ESP_LOGI(kTag, "P4 revision %u.%u, detected Flash=%u MiB, PSRAM=%u MiB",
                 chip.revision / 100, chip.revision % 100, unsigned(flash_bytes / (1024 * 1024)),
                 unsigned(esp_psram_get_size() / (1024 * 1024)));
        LogMemory("before peripherals");

        // Release the C5 boot strap for normal boot. Hosted alone owns EN/reset.
        gpio_config_t boot = {};
        boot.pin_bit_mask = 1ULL << C5_BOOT_GPIO;
        boot.mode = GPIO_MODE_INPUT;
        boot.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&boot));
        CreateBus(AUDIO_CODEC_I2C_PORT, AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN,
                  &codec_bus_);
        CreateBus(PERIPHERAL_I2C_PORT, PERIPHERAL_I2C_SDA_PIN, PERIPHERAL_I2C_SCL_PIN,
                  &peripheral_bus_);
        // Read fuel gauge only. Do not write regulator, charge, or gauge registers.
        const esp_err_t pmu = i2c_master_probe(peripheral_bus_, PMU_I2C_ADDRESS, 50);
        if (pmu == ESP_OK) {
            AttachPmu();
        } else {
            ESP_LOGI(kTag, "PMU address 0x34 not responding; battery icon off");
        }
        AttachAccelerometer();
        InitializeDisplay();
        touch_ = std::make_unique<SilaAxs15260Touch>();
        const esp_err_t touch_result = touch_->Start(peripheral_bus_, display_->Handle(), [this]() {
            const bool wake_only = asleep_.load();
            NoteUserActivity();
            return wake_only;
        });
        if (touch_result != ESP_OK) {
            ESP_LOGW(kTag, "Touch unavailable: %s (BOOT button remains usable)",
                     esp_err_to_name(touch_result));
            touch_.reset();
        }
#if CONFIG_NODELIO_PANEL
        nodelio_board_set_touch_available(touch_result == ESP_OK);
#endif
        boot_button_.OnClick([this]() {
            const bool wake_only = asleep_.load();
            NoteUserActivity();
            if (!wake_only) {
                Application::GetInstance().Schedule(
                    []() { Application::GetInstance().ToggleChatState(); });
            }
        });
        boot_button_.OnLongPress([this]() {
            NoteUserActivity();
            Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); });
        });
        esp_timer_create_args_t timer = {};
        timer.callback = [](void* arg) { static_cast<SilaP4C562Board*>(arg)->ScheduleIdlePoll(); };
        timer.arg = this;
        timer.name = "sila_idle";
        timer.skip_unhandled_events = true;
        ESP_ERROR_CHECK(esp_timer_create(&timer, &idle_timer_));
        last_activity_.store(xTaskGetTickCount());
    }

    Display* GetDisplay() override { return display_; }
    Backlight* GetBacklight() override { return &backlight_; }

    /* TF 卡 Slot0：仅供 NodeLio 曲库读取，Hosted 继续独占 Slot1。
     * 引脚与 PWRn/CD 见 config.h；sd_library 负责挂载与掉电。 */
    SdCardPins GetSdCardPins() override {
        SdCardPins pins;
        pins.clk = SD_CARD_CLK_PIN;
        pins.cmd = SD_CARD_CMD_PIN;
        pins.d0 = SD_CARD_D0_PIN;
        pins.d1 = SD_CARD_D1_PIN;
        pins.d2 = SD_CARD_D2_PIN;
        pins.d3 = SD_CARD_D3_PIN;
        pins.power_en = SD_CARD_PWR_PIN;
        pins.power_active_low = SD_CARD_PWR_ACTIVE_LOW != 0;
        pins.card_detect = SD_CARD_CD_PIN;
        pins.cd_active_low = SD_CARD_CD_ACTIVE_LOW != 0;
        pins.max_freq_khz = SD_CARD_MAX_FREQ_KHZ;
        pins.four_bit = true;
        return pins;
    }

    AudioCodec* GetAudioCodec() override {
        // ES8389's configured serial output is MR (ADCL + internal DACR reference).
        // Two I2S slots do not imply two microphone channels.
        // Both speakers receive the assistant's mono stream. MCLK is wired on P4X.
        static Es8389AudioCodec codec(codec_bus_, AUDIO_CODEC_I2C_PORT, AUDIO_SAMPLE_RATE,
                                      AUDIO_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK,
                                      AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                      AUDIO_CODEC_PA_PIN, ES8389_CODEC_DEFAULT_ADDR, true, 2, 2);
        return &codec;
    }

    void StartNetwork() override {
        if (!idle_timer_started_) {
            last_activity_.store(xTaskGetTickCount());
            ESP_ERROR_CHECK(esp_timer_start_periodic(idle_timer_, 1000000));
            idle_timer_started_ = true;
        }
        LogMemory("before Hosted/Wi-Fi");
        ESP_LOGI(kTag, "Hosted %d.%d.%d: transport DMA PSRAM preferred, L2 cache line=64",
                 ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                 ESP_HOSTED_VERSION_PATCH_1);
        WifiBoard::StartNetwork();
        LogMemory("after Hosted/Wi-Fi init");
    }

    void NoteUserActivity() override {
        goodbye_sleep_at_.store(0);
        last_activity_.store(xTaskGetTickCount());
        if (asleep_.load() || net_asleep_.load()) {
            ScheduleIdlePoll();
        }
    }

    bool IsInSleepMode() const override { return asleep_.load(); }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        const auto state = Application::GetInstance().GetDeviceState();
        if (level == PowerSaveLevel::PERFORMANCE &&
            (state == kDeviceStateConnecting || state == kDeviceStateListening ||
             state == kDeviceStateSpeaking)) {
            NoteUserActivity();
        }
        // A network reconnect in idle does not wake the display. Screen timeout
        // never disconnects Hosted or stops the current playback.
        WifiBoard::SetPowerSaveLevel(level);
    }

    bool ReadPmuReg(uint8_t reg, uint8_t* value) {
        if (!pmu_dev_ || value == nullptr) {
            return false;
        }
        const esp_err_t err = i2c_master_transmit_receive(pmu_dev_, &reg, 1, value, 1, 50);
        if (err != ESP_OK) {
            if (!pmu_read_warned_) {
                pmu_read_warned_ = true;
                ESP_LOGW(kTag, "PMU reg 0x%02X read: %s", reg, esp_err_to_name(err));
            }
            return false;
        }
        return true;
    }

    void AttachPmu() {
        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = PMU_I2C_ADDRESS;
        device.scl_speed_hz = 100000;
        if (i2c_master_bus_add_device(peripheral_bus_, &device, &pmu_dev_) != ESP_OK) {
            ESP_LOGW(kTag, "PMU 0x34 ACK but device add failed; battery icon off");
            return;
        }
        uint8_t id = 0;
        if (!ReadPmuReg(0x03, &id) || id != 0x4A) {
            ESP_LOGW(kTag, "PMU 0x34 ACK but chip id 0x%02X (want 0x4A); battery icon off", id);
            return;
        }
        pmu_ready_ = true;
        ConfigureAxpCharging();
        EnablePmuIrq();
    }

    bool WritePmuReg(uint8_t reg, uint8_t value) {
        if (!pmu_dev_) {
            return false;
        }
        const uint8_t buf[2] = {reg, value};
        const esp_err_t err = i2c_master_transmit(pmu_dev_, buf, sizeof(buf), 50);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "PMU reg 0x%02X write 0x%02X failed: %s", reg, value, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void ConfigureAxpCharging() {
        uint8_t val = 0;
        // 1. 设置 VBUS 输入限流为 2000mA（0x16 寄存器 Bit[2:0] = 5，解除默认 500mA 限制）
        if (ReadPmuReg(0x16, &val)) {
            val = (val & 0xF8) | 0x05;
            WritePmuReg(0x16, val);
        }
        // 2. 确保主电池充电使能（0x18 寄存器 Bit 1 = 1）
        if (ReadPmuReg(0x18, &val)) {
            val |= (1 << 1);
            WritePmuReg(0x18, val);
        }
        // 3. 设置恒流充电（ICC）为 1000mA（0x62 寄存器 Bit[4:0] = 0x10，大幅提升充电功率）
        if (ReadPmuReg(0x62, &val)) {
            val = (val & 0xE0) | 0x10;
            WritePmuReg(0x62, val);
        }
        // 4. 设置预充电流为 200mA（0x61 寄存器）
        if (ReadPmuReg(0x61, &val)) {
            val = (val & 0xF0) | 0x03;
            WritePmuReg(0x61, val);
        }
        // 5. 设置充电截止电压为标准 4.2V（0x64 寄存器 Bit[2:0] = 2）
        if (ReadPmuReg(0x64, &val)) {
            val = (val & 0xF8) | 0x02;
            WritePmuReg(0x64, val);
        }
        ESP_LOGI(kTag, "AXP2101 fast charging configured: VBUS limit=2000mA, ICC=1000mA");
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!pmu_ready_) {
            return false;
        }
        const int64_t now = esp_timer_get_time();
        constexpr int64_t kFreshUs = 15LL * 1000 * 1000;
        // 息屏用缓存；PMU 中断把 batt_cached_at_ 置 0 后允许再读一次，不点亮屏幕。
        if (asleep_.load() && batt_cached_at_ != 0) {
            if (!batt_present_) {
                return false;
            }
            level = batt_level_;
            charging = batt_charging_;
            discharging = batt_discharging_;
            return true;
        }
        if (batt_cached_at_ != 0 && now - batt_cached_at_ < kFreshUs) {
            if (!batt_present_) {
                return false;
            }
            level = batt_level_;
            charging = batt_charging_;
            discharging = batt_discharging_;
            return true;
        }
        uint8_t st1 = 0;
        uint8_t st2 = 0;
        uint8_t pct = 0;
        if (!ReadPmuReg(0x00, &st1) || !ReadPmuReg(0x01, &st2) || !ReadPmuReg(0xA4, &pct)) {
            // Keep the last good sample; retry sooner than a full fresh window.
            batt_cached_at_ = now - (kFreshUs - 5LL * 1000 * 1000);
            if (!batt_present_) {
                return false;
            }
            level = batt_level_;
            charging = batt_charging_;
            discharging = batt_discharging_;
            return true;
        }
        batt_cached_at_ = now;
        pmu_read_warned_ = false;
        const bool present = (st1 & 0x08) != 0;
        if (!present) {
            batt_present_ = false;
            if (!batt_absent_logged_) {
                batt_absent_logged_ = true;
                ESP_LOGI(kTag, "AXP2101 battery absent (status1=0x%02X); status bar hidden", st1);
            }
            return false;
        }
        const int direction = (st2 >> 5) & 0x3;
        const int charge_state = st2 & 0x7;
        const bool is_charging =
            direction == 1 || charge_state == 1 || charge_state == 2 || charge_state == 3;
        const bool is_discharging = direction == 2 && !is_charging;
        const int lv = pct > 100 ? 100 : pct;
        if (!batt_present_ || lv != batt_level_ || is_charging != batt_charging_) {
            ESP_LOGI(kTag, "battery %d%% %s status1=0x%02X status2=0x%02X", lv,
                     is_charging ? "charging" : (is_discharging ? "discharging" : "idle"), st1, st2);
        }
        batt_absent_logged_ = false;
        batt_present_ = true;
        batt_level_ = lv;
        batt_charging_ = is_charging;
        batt_discharging_ = is_discharging;
        level = lv;
        charging = is_charging;
        discharging = is_discharging;
        return true;
    }

    void SetIdleSleepSeconds(int seconds) override {
        idle_seconds_.store(std::clamp(seconds, 1, 86400));
    }

    void SetIdleWifiSeconds(int seconds) override {
        idle_wifi_seconds_.store(std::clamp(seconds, 15, 300));
    }

    void SetPowerSaveEnabled(bool enabled) override {
        net_save_.store(enabled);
        if (!enabled) {
            ScheduleIdlePoll();
        }
    }

    void RequestGoodbyeSleep() override {
#if CONFIG_NODELIO_PANEL
        if (!asleep_.load()) {
            goodbye_sleep_at_.store(esp_timer_get_time() +
                                    int64_t(nodelio_goodbye_sleep_seconds()) * 1000000);
        }
#endif
    }
};

DECLARE_BOARD(SilaP4C562Board);
