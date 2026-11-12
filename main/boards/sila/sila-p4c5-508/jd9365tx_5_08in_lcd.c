// -*- coding: utf-8 -*-
// JD9365TX 5.08 寸 720x720 MIPI-DSI（IVO G050PH 模组，Jadard In-Cell 触摸），
// NodeLio sila-p4c5-508 显示初始化；触摸在 jd9365tx_touch.*；背光由板级 PwmBacklight。
//
// 初始化序列取自厂家 2 Lane 版《9084----code 2lane.txt》（54 条，脚本转换）；
// 与 4 Lane 版仅 Page 0 / 0xCC 一项不同：0x31=2 Lane，0x33=4 Lane。
// 原文为 SSD2828 写法（SSD_SEND(0x11,...) = Generic Long Write），桥接芯片与
// 外围电源控制已剔除，复位由本文件 GPIO 时序实现。
//
// ⚠️ 与 JD9365DA(5 寸) 命令集不同（TX 用 0xDF 解锁/0xDE 翻页），不能套用
//    esp_lcd_jd9365 组件；本文件直接建 DPI 面板并经 DBI 口下发序列。
//
// 时序（厂家 .dsw）：HSA/HBP/HFP=20/40/20，VSA/VBP/VFP=2/19/120，pclk 42MHz；
// P4 DPI 时钟按 240MHz 整数分频取 40MHz，刷新率约 58.1Hz。
// SSD_MODE(0,1) 要求非突发 + 同步脉冲视频模式。

#include "jd9365tx_5_08in_lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_hal.h"
#include "soc/mipi_dsi_host_reg.h"
#include "soc/soc.h"


static const char *TAG = "jd9365tx_5_08in";

// ============ MIPI DSI / DPI 参数 ============
#define LCD_MIPI_DSI_LANE_NUM           2      // P4 硬件上限
// ⚠️ 非突发模式下 lane 速率必须匹配像素流：pclk*bpp/lane = 40*16/2 = 320Mbps；
//    配过高（如 750）会导致行相位错开、画面水平回卷半屏。
#define LCD_MIPI_DSI_LANE_RATE_MBPS     320
#define DPI_CLK_FREQ_MHZ                40     // 240/6 整除，取整分频无偏差
#define MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

// 🖐️ TP 复位轮数：2 = 显示序列下发后再复位一次 TP 并重下序列
//    （厂家要求 LCD 先点亮再复位 TP；本板 TP_RESET 与 LCD_RST 共用 IO26）。
#ifndef JD9365TX_TP_RESET_PASSES
// aibox / 厂家：LCD 序列下发后再硬复位一次 TP 并重下序列；1 轮时 In-Cell 常无中断。
#define JD9365TX_TP_RESET_PASSES  2
#endif

#ifndef JD9365TX_TEST_PATTERN
#define JD9365TX_TEST_PATTERN  0
#endif

// 🧪 TEST_PATTERN=1 输出 DSI host 彩条（绕过帧缓冲，仅验证链路/时序）；
//    DSI_READBACK 依赖面板 BTA 支持，默认关闭。
#ifndef JD9365TX_ENABLE_DSI_READBACK
#define JD9365TX_ENABLE_DSI_READBACK  0
#endif

static esp_ldo_channel_handle_t s_phy_ldo = NULL;

// ESP-IDF 没有公开 Generic Long Write 的 esp_lcd API，只在 HAL 层提供发送函数；
// esp_lcd_dsi_bus_handle_t 不透明。按 IDF 6.1 mipi_dsi_priv.h 中
// esp_lcd_dsi_bus_t 前缀（bus_id + hal）取 HAL。
typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} jd9365tx_dsi_bus_prefix_t;

#define JD9365TX_MAX_GENERIC_PAYLOAD  64

static mipi_dsi_hal_context_t *jd9365tx_get_dsi_hal(esp_lcd_dsi_bus_handle_t dsi_bus)
{
    return &((jd9365tx_dsi_bus_prefix_t *)dsi_bus)->hal;
}

static esp_err_t jd9365tx_generic_long_write(esp_lcd_dsi_bus_handle_t dsi_bus,
                                              uint8_t cmd, const uint8_t *data,
                                              size_t data_bytes)
{
    uint8_t payload[JD9365TX_MAX_GENERIC_PAYLOAD] = {cmd};
    ESP_RETURN_ON_FALSE(data_bytes + 1 <= sizeof(payload), ESP_ERR_INVALID_SIZE, TAG,
                        "Generic Long Write 载荷过长：%u 字节", (unsigned)(data_bytes + 1));
    if (data_bytes) {
        memcpy(payload + 1, data, data_bytes);
    }
    mipi_dsi_hal_host_gen_write_long_packet(jd9365tx_get_dsi_hal(dsi_bus), 0,
                                            MIPI_DSI_DT_GENERIC_LONG_WRITE,
                                            payload, data_bytes + 1);
    return ESP_OK;
}

static void jd9365tx_generic_short_write_command(esp_lcd_dsi_bus_handle_t dsi_bus,
                                                  uint8_t cmd)
{
    mipi_dsi_hal_host_gen_write_short_packet(jd9365tx_get_dsi_hal(dsi_bus), 0,
                                             MIPI_DSI_DT_GENERIC_SHORT_WRITE_1, cmd);
}

// 一条初始化命令：命令字 + 参数 + 参数字节数 + 之后的延时(ms)
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    size_t data_bytes;
    unsigned int delay_ms;
} jd9365tx_init_cmd_t;

// ------------------------------------------------------------------
// 厂家初始化序列（54 条，逐条转换自厂家代码，勿凭直觉改）
// ------------------------------------------------------------------
static const jd9365tx_init_cmd_t lcd_init_cmds[] = {
    {0xDF, (uint8_t[]){0x90, 0x84, 0x14}, 3, 1},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x44, 0x67, 0xB5, 0x22, 0x22, 0xAA}, 6, 1},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xB7, (uint8_t[]){0x16, 0x00, 0x6E, 0x00, 0x33}, 5, 1},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0xB3, (uint8_t[]){0x00, 0x01, 0x53, 0x53, 0x2B, 0x2B, 0x50, 0x00, 0x70, 0xB6}, 10, 1},
    {0xBC, (uint8_t[]){0x0C, 0x38, 0x88}, 3, 1},
    {0xBD, (uint8_t[]){0x00, 0x41, 0x41}, 3, 1},
    {0xBF, (uint8_t[]){0x10, 0x35, 0xC3}, 3, 1},
    {0xC0, (uint8_t[]){0xBD, 0xBD}, 2, 1},
    {0xC3, (uint8_t[]){0x03, 0x09, 0x00, 0x05, 0x30, 0xFD}, 6, 1},
    {0xC4, (uint8_t[]){0x06, 0xFE, 0x0E, 0xEA, 0x06, 0x31, 0x07, 0x75, 0x07, 0x75, 0x07, 0x75, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0}, 24, 1},
    {0xC5, (uint8_t[]){0x01, 0x00, 0x8C, 0x00, 0x5E, 0x01, 0x7D, 0x09, 0x60, 0x02, 0xD0, 0x02, 0xD0, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x0E, 0x01, 0x92, 0x02, 0x00, 0x02, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 36, 1},
    {0xC6, (uint8_t[]){0x01, 0x47, 0x00, 0x78, 0x00, 0x14, 0x28, 0x82, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01}, 16, 1},
    {0xC8, (uint8_t[]){0x10, 0x68, 0xB4}, 3, 1},
    {0xCB, (uint8_t[]){0x7C, 0x60, 0x50, 0x3A, 0x2A, 0x21, 0x13, 0x19, 0x06, 0x24, 0x28, 0x2B, 0x4C, 0x3C, 0x46, 0x3A, 0x39, 0x2E, 0x20, 0x14, 0x06, 0x7C, 0x60, 0x50, 0x3A, 0x2A, 0x21, 0x13, 0x19, 0x06, 0x24, 0x28, 0x2B, 0x4C, 0x3C, 0x46, 0x3A, 0x39, 0x2E, 0x20, 0x14, 0x06}, 42, 1},
    {0xCC, (uint8_t[]){0x31}, 1, 1}, // 厂家 2 Lane 版；4 Lane 版为 0x33
    {0xCD, (uint8_t[]){0x34, 0x06, 0x34, 0x06, 0x34, 0x06, 0x34, 0x06, 0x1A, 0x1A, 0x1A, 0x1A}, 12, 1},
    {0xCE, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 56, 1},
    {0xCF, (uint8_t[]){0x40, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFF, 0xFF, 0x00, 0x00, 0x00}, 18, 1},
    {0xD0, (uint8_t[]){0x00, 0x3F, 0x07, 0x3F, 0x24, 0x1B, 0x19, 0x17, 0x15, 0x13, 0x11, 0x01, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36}, 23, 1},
    {0xD1, (uint8_t[]){0x00, 0x3F, 0x3F, 0x06, 0x3F, 0x24, 0x1A, 0x18, 0x16, 0x14, 0x12, 0x10, 0x00, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36}, 23, 1},
    {0xD2, (uint8_t[]){0x00, 0x3F, 0x00, 0x24, 0x3F, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x06, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36}, 23, 1},
    {0xD3, (uint8_t[]){0x00, 0xBF, 0xBF, 0x81, 0xA4, 0xBF, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x07, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36}, 23, 1},
    {0xD4, (uint8_t[]){0x00, 0x20, 0x0D, 0x01, 0x03, 0x20, 0x04, 0x00, 0x00, 0x00, 0x05, 0x81, 0x02, 0xE7, 0x01, 0x23, 0x45, 0x60, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x05, 0x20, 0x02, 0xE9, 0x01, 0x03, 0x80, 0x0D, 0x00, 0x02, 0x02, 0xD8, 0x02, 0xD8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00, 0x00, 0x20, 0x00}, 56, 1},
    {0xD5, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x0A, 0x78, 0x00, 0x00, 0x03, 0x5D, 0xB4, 0xA0, 0x11, 0x00, 0x00, 0x00, 0x00, 0x2F, 0x04}, 28, 1},
    {0xD7, (uint8_t[]){0x00, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF, 0x01, 0xFF}, 15, 1},
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x43, 0x00, 0x00, 0x00, 0x08}, 5, 1},
    {0xC7, (uint8_t[]){0x14, 0x14, 0x04, 0x04}, 4, 1},
    {0xCD, (uint8_t[]){0x30, 0x53}, 2, 1},
    {0xCE, (uint8_t[]){0x09, 0x00, 0xE0, 0x00, 0xE0}, 5, 1},
    {0xCF, (uint8_t[]){0x00, 0x65}, 2, 1},
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xB3, (uint8_t[]){0x4A, 0xA3, 0xE2, 0x2F, 0x43}, 5, 1},
    {0xB4, (uint8_t[]){0x7F}, 1, 1},
    {0xBB, (uint8_t[]){0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x40, 0x43, 0x04}, 11, 1},
    {0xBD, (uint8_t[]){0x1B}, 1, 1},
    {0xBF, (uint8_t[]){0x0F, 0x03}, 2, 1},
    {0xC1, (uint8_t[]){0x50, 0x40, 0x00, 0x00, 0x00, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00}, 13, 1},
    {0xC3, (uint8_t[]){0x20, 0xFF}, 2, 1},
    {0xC4, (uint8_t[]){0x00, 0x11, 0x07, 0x00, 0x18, 0x01, 0x08}, 7, 1},
    {0xC6, (uint8_t[]){0x48, 0x00}, 2, 1},
    {0xE5, (uint8_t[]){0x00, 0x3C, 0x3C, 0x27, 0x8D, 0x09, 0x00, 0xC2, 0x01, 0x30, 0x05, 0x00, 0x00, 0x05, 0x00}, 15, 1},
    {0xE6, (uint8_t[]){0x10, 0x0B, 0x4E, 0x00, 0x00}, 5, 1},
    {0xE9, (uint8_t[]){0x10, 0xA0, 0xA0}, 3, 1},
    {0xEC, (uint8_t[]){0x0C, 0x77, 0x0D}, 3, 1},
    {0xDE, (uint8_t[]){0x03}, 1, 0},
    {0xD1, (uint8_t[]){0x00, 0x00, 0x39, 0xFF, 0x08}, 5, 1},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 50},
    {0x11, (uint8_t[]){0x00}, 0, 150},
    {0x29, (uint8_t[]){0x00}, 0, 50},
};

// ------------------------------------------------------------------
// 面板上电 + 初始化
// ------------------------------------------------------------------
esp_err_t sila_jd9365tx_5_08in_init(gpio_num_t pin_rst,
                                      esp_ldo_channel_handle_t *out_phy_ldo,
                                      esp_lcd_dsi_bus_handle_t *out_dsi_bus,
                                      esp_lcd_panel_io_handle_t *out_io,
                                      esp_lcd_panel_handle_t *out_panel)
{
    ESP_RETURN_ON_FALSE(out_panel && out_dsi_bus && out_io && out_phy_ldo, ESP_ERR_INVALID_ARG, TAG,
                        "out handles required");

    // 背光由板级 PwmBacklight 保持关闭至 UI/彩条就绪，避免上电花屏。

    // 1. 启用 DSI PHY 电源（内部 LDO）
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_phy_ldo), TAG, "申请 DSI PHY LDO 失败");
    ESP_LOGI(TAG, "⚡ MIPI DSI PHY 电源已启用（LDO ch%d, %dmV）",
             MIPI_DSI_PHY_PWR_LDO_CHAN, MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV);

    // 2. DSI 总线
    // phy_clk_src 必须留 0：IDF 对 P4 rev>=3 会选 XTAL。
    // 勿写 MIPI_DSI_PHY_CLK_SRC_DEFAULT（实为 LEGACY=PLL_F20M），HAL 不认会 abort。
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
        .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_RATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG, "新建 DSI 总线失败");

    // 3. DBI 命令接口（下发厂家序列用）
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &io), TAG, "新建 DBI IO 失败");

    // 4. DPI 视频时序（来自厂家配套 .dsw，42MHz 按 IDF 整数分频约束取 40MHz）
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLK_FREQ_MHZ,
        // P4 DSI bridge 输入/输出同 raw_type：全链路 RGB565（aibox 已验证）。
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = JD9365TX_5_08IN_H_RES,
            .v_size = JD9365TX_5_08IN_V_RES,
            .hsync_pulse_width = 20,
            .hsync_back_porch = 40,
            .hsync_front_porch = 20,   // htotal = 800
            .vsync_pulse_width = 2,
            .vsync_back_porch = 19,
            .vsync_front_porch = 120,  // vtotal = 861
        },
        .flags = {
            .disable_lp = 1,  // 对齐厂家 SSD_MODE(0,1) 连续 HS
        },
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(dsi_bus, &dpi_config, &panel), TAG, "新建 DPI 面板失败");
    // RGB565→RGB565 无需 DMA2D CSC；启用后会与 JPEG/其它 DMA2D 客户端抢通道，
    // 实机曾在首帧 UI 刷新时于 async_color_convert / free_up_channels 触发 Store fault。
    // 走 CPU draw_bitmap 即可（720² 局部刷新可接受）。
    ESP_LOGI(TAG, "DPI DMA2D fb-copy disabled (RGB565 identity path)");

    // 厂家 SSD_MODE(0,1) 明确要求非突发 + 同步脉冲；IDF 的 DPI 驱动在
    // esp_lcd_new_panel_dpi() 中固定选择突发模式，因此需在启动视频流前修正寄存器。
    // 此处依赖 ESP32-P4 私有寄存器定义，升级 ESP-IDF 后必须复核字段与调用时序。
    REG_SET_FIELD(DSI_HOST_VID_MODE_CFG_REG, DSI_HOST_VID_MODE_TYPE, 0);
    ESP_LOGI(TAG, "📡 DSI 已配置为 %d Lane @ %dMbps，非突发同步脉冲模式",
             LCD_MIPI_DSI_LANE_NUM, LCD_MIPI_DSI_LANE_RATE_MBPS);

    // 5. 硬复位（低 10ms → 高 200ms，厂家时序为 RESET 低 100ms 后拉高再等 200ms）
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pin_rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "复位脚配置失败");
    gpio_set_level(pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

#if JD9365TX_ENABLE_DSI_READBACK
    // 5.5 读面板 ID：验证 DSI LP 链路双向可通（期望 93 65 xx）
    {
        uint8_t id[3] = {0};
        esp_err_t rd = esp_lcd_panel_io_rx_param(io, 0x04, id, sizeof(id));   // RDDID
        if (rd == ESP_OK) {
            ESP_LOGW(TAG, "🔍 面板 ID(0x04) = %02X %02X %02X", id[0], id[1], id[2]);
        } else {
            ESP_LOGW(TAG, "🔍 面板 ID 读取失败：%s（DSI LP 链路可能不通）",
                     esp_err_to_name(rd));
        }
    }
#endif

    // 6. 下发厂家 2 Lane 初始化序列（LP 模式，视频流尚未启动）
    for (int pass = 0; pass < JD9365TX_TP_RESET_PASSES; pass++) {
    if (pass == 1) {
        // 🖐️ 厂家要求「LCD 先点亮 → 再复位 TP」；本板复位脚共用 IO26，
        //    故首轮显示序列下发后补一次复位，再重下序列恢复画面。
        ESP_LOGW(TAG, "🖐️ 显示序列已下发，按厂家要求补一次 TP 复位后重下序列");
        gpio_set_level(pin_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(pin_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(pin_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    for (size_t i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        if (lcd_init_cmds[i].data_bytes) {
            ESP_RETURN_ON_ERROR(jd9365tx_generic_long_write(dsi_bus, lcd_init_cmds[i].cmd,
                                                            lcd_init_cmds[i].data,
                                                            lcd_init_cmds[i].data_bytes),
                                TAG, "Generic Long Write 命令 0x%02X 失败", lcd_init_cmds[i].cmd);
        } else {
            jd9365tx_generic_short_write_command(dsi_bus, lcd_init_cmds[i].cmd);
        }
        if (lcd_init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(lcd_init_cmds[i].delay_ms));
        }
    }
    }
    ESP_LOGI(TAG, "📡 厂家 2 Lane 面板参数已生效（Page 0 / CC=0x31）");
    ESP_LOGI(TAG, "📜 厂家 Generic Write 初始化序列下发完成（%u 条）",
             (unsigned)(sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0])));

#if JD9365TX_ENABLE_DSI_READBACK
    // 初始化后再读一次电源/显示状态，确认序列是否真的被执行
    {
        uint8_t pm = 0, dm = 0;
        if (esp_lcd_panel_io_rx_param(io, 0x0A, &pm, 1) == ESP_OK) {   // RDDPM
            ESP_LOGW(TAG, "🔍 电源模式(0x0A) = 0x%02X（bit4=睡眠退出 bit2=显示开）", pm);
        }
        if (esp_lcd_panel_io_rx_param(io, 0x0D, &dm, 1) == ESP_OK) {   // RDDIM
            ESP_LOGW(TAG, "🔍 显示模式(0x0D) = 0x%02X", dm);
        }
    }
#endif

    // 7. 启动 DPI 视频流
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "面板初始化失败");

#if JD9365TX_TEST_PATTERN
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
    ESP_LOGW(TAG, "🧪 已启用 DPI 垂直彩条测试图案（JD9365TX_TEST_PATTERN=1）");
#else
    ESP_LOGI(TAG, "🖥️ DPI 硬件彩条已关闭，使用 LVGL 帧缓冲输出");
#endif

    *out_phy_ldo = s_phy_ldo;
    *out_dsi_bus = dsi_bus;
    *out_io = io;
    *out_panel = panel;
    ESP_LOGI(TAG, "JD9365TX 5.08in init done %dx%d RGB565 test_pattern=%d",
             JD9365TX_5_08IN_H_RES, JD9365TX_5_08IN_V_RES, JD9365TX_TEST_PATTERN);
    return ESP_OK;
}

