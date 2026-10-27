#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "power_save_timer.h"
#include "config.h"

#if CONFIG_NODELIO_PANEL
#include "media_player.h"
#include "nvs_config.h"
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
    uint8_t touch_wake_hits_ = 0; /* 息屏触摸去抖：连续命中次数 */

    void InitializePowerSaveTimer() {
        /* 空闲息屏（背光灭）与空闲断网时刻独立；秒数来自 NVS */
#if CONFIG_NODELIO_PANEL
        int idle_s = (int)nodelio_idle_sleep_seconds();
        int wifi_s = (int)nodelio_idle_wifi_seconds();
#else
        int idle_s = 60;
        int wifi_s = -1;
#endif
        power_save_timer_ = new PowerSaveTimer(-1, idle_s, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            /* 先拉灭背光，再停 LVGL。反过来会先看到横条，背光还亮着。 */
            GetBacklight()->SetBrightness(0);
#if CONFIG_NODELIO_PANEL
            /* 播歌时不要停 LVGL / 关面板：停掉后触摸只靠定时器读 I2C，
             * 和功放/Codec 抢总线会读不到点，屏和播放都关不掉。
             * 背光已灭；LVGL 仍在，点一下走 NoteActivity 亮回来。 */
            if (nodelio_media_is_playing()) {
                return;
            }
#endif
            GetDisplay()->SetPowerSaveMode(true);
            /* 原理图无 AMP_EN：空闲关输出减滋滋；正在播媒体则勿掐功放 */
#if CONFIG_NODELIO_PANEL
            const bool keep_audio = nodelio_media_is_playing();
#else
            const bool keep_audio = false;
#endif
            if (!keep_audio) {
                if (auto* codec = GetAudioCodec()) {
                    codec->EnableOutput(false);
                }
            }
            if (panel_handle_) {
                (void)esp_lcd_panel_disp_on_off(panel_handle_, false);
            }
            (void)lvgl_port_stop();
#if CONFIG_NODELIO_PANEL
            nodelio_panel_ui_notify_screen(false); /* 仅息屏；断网见 OnEnterNetSleep */
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
            nodelio_panel_ui_notify_screen(true);
#endif
            /* 播报时 AudioService 会再 EnableOutput(true)；此处不强制开 */
        });
#if CONFIG_NODELIO_PANEL
        power_save_timer_->SetSecondsToNetSleep(wifi_s);
        power_save_timer_->OnEnterNetSleep([]() { nodelio_panel_ui_notify_net(false); });
        power_save_timer_->OnExitNetSleep([]() { nodelio_panel_ui_notify_net(true); });
        /* 息屏计时始终开；「允许省电」只控断网（load 后再 SetPowerSaveEnabled） */
        power_save_timer_->SetEnabled(true);
        power_save_timer_->SetNetSleepEnabled(nodelio_power_save_enabled());
#else
        power_save_timer_->SetEnabled(true);
#endif
    }

    void StartTouchWakePoll() {
        if (!touch_ || touch_wake_timer_) {
            return;
        }
        touch_wake_hits_ = 0;
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
                    /* 连续两拍（~160ms）才唤醒，滤毛刺/悬空误触 */
                    if (++self->touch_wake_hits_ >= 2) {
                        self->touch_wake_hits_ = 0;
                        self->power_save_timer_->NoteActivity();
                    }
                } else {
                    self->touch_wake_hits_ = 0;
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
        /* 息屏触控轮询 80ms：人手轻触约 80-150ms；去抖后需约 160ms 实触 */
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
        /* 先钉灭背光再点屏：否则 ST7701 垃圾帧 + 浮空 BL 会闪一下。 */
        GetBacklight()->PreloadSavedBrightness();
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeRGB();
        InitializeTouch();
        /* 无物理 BOOT 键（GPIO0=RGB DB0）；配网：无 WiFi/未绑定→SoftAP，或语音「进入配网」 */
        /* 亮度等 overlay 首帧 lv_refr_now 后再 RestoreBrightness */
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
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT,
                                      DISPLAY_BACKLIGHT_FREQ_HZ, DISPLAY_BACKLIGHT_OFF_LEVEL);
        return &backlight;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::LOW_POWER) {
            /* 唤醒建链/聆听/说话期间禁止 WiFi MAX_MODEM，否则 UDP 语音断续。
             * HTTP 拉流同样不能 MAX_MODEM：m f null + underrun 会把内部堆打到几 KB，
             * 唤醒时 esp-aes 分配失败并拆掉 MQTT。息屏不在这里 WakeUp。 */
            DeviceState st = Application::GetInstance().GetDeviceState();
#if CONFIG_NODELIO_PANEL
            /* 不要求仍是 Notifying：换曲间隙 / goodbye 迟到 Schedule 也可能打 LOW_POWER。 */
            if (nodelio_media_is_playing() || Application::GetInstance().IsHttpAudioBusy()) {
                WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                return;
            }
#endif
            if (st == kDeviceStateConnecting || st == kDeviceStateListening ||
                st == kDeviceStateSpeaking || st == kDeviceStateNotifying) {
                ESP_LOGW(TAG, "ignore LOW_POWER during voice (state=%d)", (int)st);
                if (power_save_timer_) {
                    power_save_timer_->WakeUp();
                }
                WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                return;
            }
        }
        if (level != PowerSaveLevel::LOW_POWER && power_save_timer_) {
#if CONFIG_NODELIO_PANEL
            /* 已息屏时：仅语音态才亮屏。否则息屏断 MQTT / 后台建链的 PERFORMANCE
             * 会立刻 WakeUp，表现为「刚灭又亮、断网又拉网」。 */
            if (power_save_timer_->IsInSleepMode()) {
                DeviceState st = Application::GetInstance().GetDeviceState();
                if (st == kDeviceStateConnecting || st == kDeviceStateListening ||
                    st == kDeviceStateSpeaking || st == kDeviceStateNotifying) {
                    power_save_timer_->WakeUp();
                } else {
                    ESP_LOGI(TAG, "PERFORMANCE while asleep state=%d — keep screen off", (int)st);
                }
            } else {
                power_save_timer_->WakeUp();
            }
#else
            power_save_timer_->WakeUp();
#endif
        }
        WifiBoard::SetPowerSaveLevel(level);
#if !CONFIG_NODELIO_PANEL
        /* 非面板：LOW_POWER 带短延迟息屏。墙板短延迟只走 RequestGoodbyeSleep，
         * 切勿在此 WakeUp——息屏关 MQTT 会触发 LOW_POWER，会把刚灭的屏立刻点亮。 */
        if (level == PowerSaveLevel::LOW_POWER && power_save_timer_) {
            power_save_timer_->RequestSleepIn(15);
        }
#endif
    }

    virtual void NoteUserActivity() override {
        if (power_save_timer_) {
            power_save_timer_->NoteActivity();
        }
    }

    virtual bool IsInSleepMode() const override {
        return power_save_timer_ && power_save_timer_->IsInSleepMode();
    }

    virtual void SetIdleSleepSeconds(int seconds) override {
        if (power_save_timer_) {
            power_save_timer_->SetSecondsToSleep(seconds);
        }
    }

    virtual void SetIdleWifiSeconds(int seconds) override {
        if (power_save_timer_) {
            power_save_timer_->SetSecondsToNetSleep(seconds);
        }
    }

    virtual void SetPowerSaveEnabled(bool enabled) override {
        if (!power_save_timer_) {
            return;
        }
#if CONFIG_NODELIO_PANEL
        /* 「允许省电」= 允许空闲断网；息屏仍走 idle_sleep / goodbye */
        power_save_timer_->SetEnabled(true);
        power_save_timer_->SetNetSleepEnabled(enabled);
#else
        power_save_timer_->SetEnabled(enabled);
#endif
    }

    virtual void RequestGoodbyeSleep() override {
        if (!power_save_timer_) {
            return;
        }
        /* 已息屏（关网关通道）不算「退下」，勿再排 15s 唤醒竞态 */
        if (power_save_timer_->IsInSleepMode()) {
            return;
        }
#if CONFIG_NODELIO_PANEL
        power_save_timer_->RequestSleepIn((int)nodelio_goodbye_sleep_seconds());
#else
        power_save_timer_->RequestSleepIn(15);
#endif
    }
};

DECLARE_BOARD(Sila86Board);
