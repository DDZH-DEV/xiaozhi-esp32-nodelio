#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "power_save_timer.h"
#include "config.h"

#if CONFIG_NODELIO_PANEL
#include "panel_ui.h"
#endif

#include <esp_lcd_st7701.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_io_additions.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>

#define TAG "Sila86Board"

/* SILA init_cmds from NodeLio ESP32-IDF boards/sila_86/lcd_st7701.c (not waveshare 4b) */
static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0B, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x37, 0x02}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x0F, 0x16, 0x0E, 0x11, 0x07, 0x09, 0x09, 0x08, 0x23, 0x05, 0x11, 0x0F, 0x28, 0x2D, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x0F, 0x16, 0x0E, 0x11, 0x07, 0x09, 0x08, 0x09, 0x23, 0x05, 0x11, 0x0F, 0x28, 0x2D, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x4D}, 1, 0},
    {0xB1, (uint8_t[]){0x33}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x4B}, 1, 0},
    {0xB7, (uint8_t[]){0x8C}, 1, 0},
    {0xB8, (uint8_t[]){0x20}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x00, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x02, 0xF0, 0x00, 0x00, 0x03, 0xF0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x10, 0x10, 0x40, 0x40, 0xF2, 0xF0, 0x00, 0x00, 0xF2, 0xF0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x07, 0xEF, 0xF0, 0xF0, 0x09, 0xF1, 0xF0, 0xF0, 0x03, 0xF3, 0xF0, 0xF0, 0x05, 0xED, 0xF0, 0xF0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0xF0, 0xF0, 0xF0, 0x0A, 0xF2, 0xF0, 0xF0, 0x04, 0xF4, 0xF0, 0xF0, 0x06, 0xEE, 0xF0, 0xF0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0xE4, 0xE4, 0x44, 0x88, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x78, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0x20, 0xF9, 0x87, 0x76, 0x65, 0x54, 0x4F, 0xFF, 0xFF, 0xF4, 0x45, 0x56, 0x67, 0x78, 0x9F, 0x02}, 16, 0},
    {0xEF, (uint8_t[]){0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F}, 6, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x20, (uint8_t[]){0x00}, 0, 0},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

class Sila86Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    esp_lcd_panel_handle_t panel_handle_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;
    esp_timer_handle_t touch_wake_timer_ = nullptr;

    void InitializePowerSaveTimer() {
        /* 空闲 60s 息屏（背光灭）；无关机（无 PMIC） */
        power_save_timer_ = new PowerSaveTimer(-1, 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(0);
            /* 原理图无 AMP_EN：尽量关 ES8311 输出，减轻 NS4150 空闲滋滋 */
            if (auto* codec = GetAudioCodec()) {
                codec->EnableOutput(false);
            }
            if (panel_handle_) {
                (void)esp_lcd_panel_disp_on_off(panel_handle_, false);
            }
            (void)lvgl_port_stop();
#if CONFIG_NODELIO_PANEL
            nodelio_panel_ui_notify_screen(false); /* 异步停 MQTT/WiFi */
#endif
            StartTouchWakePoll(); /* 息屏后 LVGL 不读触摸，单独轮询唤醒 */
        });
        power_save_timer_->OnExitSleepMode([this]() {
            StopTouchWakePoll();
            (void)lvgl_port_resume();
            if (panel_handle_) {
                (void)esp_lcd_panel_disp_on_off(panel_handle_, true);
            }
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
#if CONFIG_NODELIO_PANEL
            nodelio_panel_ui_notify_screen(true); /* 先亮屏再异步拉网 */
#endif
            /* 播报时 AudioService 会再 EnableOutput(true)；此处不强制开 */
        });
        power_save_timer_->SetEnabled(true);
    }

    void StartTouchWakePoll() {
        if (!touch_ || touch_wake_timer_) {
            return;
        }
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                auto* self = static_cast<Sila86Board*>(arg);
                if (!self->touch_ || !self->power_save_timer_) {
                    return;
                }
                esp_lcd_touch_read_data(self->touch_);
                esp_lcd_touch_point_data_t pt = {};
                uint8_t cnt = 0;
                if (esp_lcd_touch_get_data(self->touch_, &pt, &cnt, 1) == ESP_OK && cnt > 0) {
                    self->power_save_timer_->NoteActivity();
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sila86_tp_wake",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &touch_wake_timer_) != ESP_OK) {
            return;
        }
        /* 息屏触控轮询 80ms：捕获率 100%（人手轻触约 80-150ms），单次 I2C 读坐标耗时 <0.1ms，几乎不增功耗 */
        esp_timer_start_periodic(touch_wake_timer_, 80 * 1000);
    }

    void StopTouchWakePoll() {
        if (!touch_wake_timer_) {
            return;
        }
        esp_timer_stop(touch_wake_timer_);
        esp_timer_delete(touch_wake_timer_);
        touch_wake_timer_ = nullptr;
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeRGB() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;

        spi_line_config_t line_config = {
            .cs_io_type = IO_TYPE_GPIO,
            .cs_gpio_num = BSP_LCD_IO_SPI_CS,
            .scl_io_type = IO_TYPE_GPIO,
            .scl_gpio_num = BSP_LCD_IO_SPI_SCL,
            .sda_io_type = IO_TYPE_GPIO,
            .sda_gpio_num = BSP_LCD_IO_SPI_SDA,
            .io_expander = nullptr,
        };
        esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io_config, &panel_io));

        esp_lcd_panel_handle_t panel_handle = nullptr;
        /* Inline timings (C++): ST7701_480_480_PANEL_60HZ_RGB_TIMING uses .flags.x= which is invalid in C++) */
        esp_lcd_rgb_panel_config_t rgb_config = {
            .clk_src = LCD_CLK_SRC_DEFAULT,
            .timings = {
                .pclk_hz = 16 * 1000 * 1000,
                .h_res = DISPLAY_WIDTH,
                .v_res = DISPLAY_HEIGHT,
                .hsync_pulse_width = 10,
                .hsync_back_porch = 10,
                .hsync_front_porch = 20,
                .vsync_pulse_width = 10,
                .vsync_back_porch = 10,
                .vsync_front_porch = 10,
                .flags = {
                    .pclk_active_neg = false,
                },
            },
            .data_width = 16,
            .in_color_format = LCD_COLOR_FMT_RGB565,
            .out_color_format = LCD_COLOR_FMT_RGB565,
            /* LVGL avoid_tearing/direct_mode 会 get_frame_buffer(2)，必须双 FB */
            .num_fbs = 2,
            .bounce_buffer_size_px = RGB_BOUNCE_BUFFER_SIZE,
            .dma_burst_size = 64,
            .hsync_gpio_num = BSP_LCD_HSYNC,
            .vsync_gpio_num = BSP_LCD_VSYNC,
            .de_gpio_num = BSP_LCD_DE,
            .pclk_gpio_num = BSP_LCD_PCLK,
            .disp_gpio_num = BSP_LCD_DISP,
            .data_gpio_nums = {
                BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
                BSP_LCD_DATA4, BSP_LCD_DATA5, BSP_LCD_DATA6, BSP_LCD_DATA7,
                BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
                BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15,
            },
            .flags = {
                .fb_in_psram = 1,
            },
        };

        st7701_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .rgb_config = &rgb_config,
            .flags = {
                .mirror_by_cmd = 1,
                .auto_del_panel_io = 0,
            },
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = BSP_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 18;
        panel_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(panel_io, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        esp_lcd_panel_disp_on_off(panel_handle, true);
        panel_handle_ = panel_handle;

        display_ = new RgbLcdDisplay(panel_io, panel_handle,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeTouch() {
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
        tp_io_config.scl_speed_hz = 400000;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io) != ESP_OK) {
            ESP_LOGW(TAG, "FT5x06 touch IO failed, continuing without touch");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };

        if (esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch_) != ESP_OK || touch_ == nullptr) {
            ESP_LOGW(TAG, "FT5x06 not found, continuing without touch");
            esp_lcd_panel_io_del(tp_io);
            touch_ = nullptr;
            return;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = touch_,
        };
        if (touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
            ESP_LOGI(TAG, "FT5x06 touch ready");
        } else {
            ESP_LOGW(TAG, "LVGL display missing, touch not added");
        }
    }

public:
    Sila86Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeRGB();
        InitializeTouch();
        /* 无物理 BOOT 键（GPIO0=RGB DB0）；配网靠无 WiFi 凭据时自动进 SoftAP */
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER && power_save_timer_) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
        /* goodbye → 延迟息屏，便于看完最后一句/点屏；唤醒会取消 */
        if (level == PowerSaveLevel::LOW_POWER && power_save_timer_) {
            power_save_timer_->RequestSleepIn(15);
        }
    }

    virtual void NoteUserActivity() override {
        if (power_save_timer_) {
            power_save_timer_->NoteActivity();
        }
    }
};

DECLARE_BOARD(Sila86Board);
