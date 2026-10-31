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
#include "nvs_config.h"
#include "panel_ui.h"
#endif

#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_ldo_regulator.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <memory>

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
}

// The generic MipiLcdDisplay uses RGB565. This panel needs RGB888 throughout.
class SilaP4Display final : public LcdDisplay {
public:
    SilaP4Display(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, Backlight& backlight)
        : LcdDisplay(io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT), backlight_(backlight) {
        lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
        port_config.task_stack = 16384;
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
        lv_timer_set_period(lv_display_get_refr_timer(display_), idle ? 200 : 30);
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
    SilaP4Display* display_ = nullptr;
    esp_timer_handle_t idle_timer_ = nullptr;
    std::atomic<TickType_t> last_activity_{0};
    std::atomic<bool> asleep_{false};
    std::atomic<bool> poll_pending_{false};
    std::atomic<int> idle_seconds_{60};
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
                const bool idle = timeout && Application::GetInstance().CanEnterSleepMode();
                if (idle != asleep_.load() && display_->SetIdleRefresh(idle)) {
                    asleep_.store(idle);
                    if (idle) {
                        backlight_.SetBrightness(0);
                    } else {
                        backlight_.RestoreBrightness();
                    }
#if CONFIG_NODELIO_PANEL
                    DisplayLockGuard lock(display_);
                    if (lock) {
                        nodelio_panel_ui_notify_screen(!idle);
                    }
#endif
                }
            });
        }
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
        // Presence only: do not change regulator, charge or battery configuration.
        const esp_err_t pmu = i2c_master_probe(peripheral_bus_, PMU_I2C_ADDRESS, 50);
        ESP_LOGI(kTag, "PMU address 0x34 %s; rails/charging left at hardware defaults",
                 pmu == ESP_OK ? "ACK" : "not responding");
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

    AudioCodec* GetAudioCodec() override {
        // Two microphone channels, no fabricated hardware reference channel.
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
        WifiBoard::StartNetwork();
    }

    void NoteUserActivity() override {
        goodbye_sleep_at_.store(0);
        last_activity_.store(xTaskGetTickCount());
        if (asleep_.load()) {
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

    void SetIdleSleepSeconds(int seconds) override {
        idle_seconds_.store(std::clamp(seconds, 1, 86400));
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
