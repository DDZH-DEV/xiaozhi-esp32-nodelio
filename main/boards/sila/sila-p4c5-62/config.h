#pragma once

#include <driver/gpio.h>
#include <driver/i2c_master.h>

// SCH_P4X_2026-10-13.pdf, sheets 1, 2, 8. Only for P4X silicon v3.x.
#define BOOT_BUTTON_GPIO GPIO_NUM_35
#define C5_BOOT_GPIO GPIO_NUM_53
// Hosted owns C5 EN (54) and SDIO Slot1 (14..19), configured in config.json.

#define AUDIO_CODEC_I2C_PORT I2C_NUM_1
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_8
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_WS GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_11
#define AUDIO_CODEC_PA_PIN GPIO_NUM_31
#define AUDIO_SAMPLE_RATE 24000

#define PERIPHERAL_I2C_PORT I2C_NUM_0
#define PERIPHERAL_I2C_SDA_PIN GPIO_NUM_28
#define PERIPHERAL_I2C_SCL_PIN GPIO_NUM_29
#define PMU_I2C_ADDRESS 0x34
#define PMU_IRQ_GPIO GPIO_NUM_6
#define TOUCH_I2C_ADDRESS 0x3B
#define TOUCH_IRQ_GPIO GPIO_NUM_27
#define DISPLAY_RESET_PIN GPIO_NUM_26  // Shared with touch. Reset exactly once.
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_30
#define DISPLAY_PHYSICAL_WIDTH 452
#define DISPLAY_PHYSICAL_HEIGHT 1280
#define DISPLAY_WIDTH 1280
#define DISPLAY_HEIGHT 452

/* TF/SD Slot0（SCH_P4X sheet 1）：4-bit + PWRn + CD。
 * Hosted 独占 SDMMC Slot1，Slot0 归本板曲库（对齐 host_sdcard_with_hosted）。
 * PWRn 低有效（AO3401）供卡 VDD；Slot0 IO/SD_VREF 须先开片内 LDO VO4@3.3V（IDF 文档）。 */
#define SD_CARD_CLK_PIN GPIO_NUM_43
#define SD_CARD_CMD_PIN GPIO_NUM_44
#define SD_CARD_D0_PIN GPIO_NUM_39
#define SD_CARD_D1_PIN GPIO_NUM_40
#define SD_CARD_D2_PIN GPIO_NUM_41
#define SD_CARD_D3_PIN GPIO_NUM_42
#define SD_CARD_PWR_PIN GPIO_NUM_51
#define SD_CARD_CD_PIN GPIO_NUM_52
#define SD_CARD_MAX_FREQ_KHZ 20000
/* 原理图写作 PWRn/CD，按低有效实现；若上机发现卡不上电/误判未插卡，
 * 只改这两行即可，无需动 sd_library 与板级逻辑。 */
#define SD_CARD_PWR_ACTIVE_LOW 1
#define SD_CARD_CD_ACTIVE_LOW 1
