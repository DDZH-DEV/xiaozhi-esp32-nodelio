#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* SILA-86 — pins from NodeLio ESP32-IDF / D:\xz 1.5.6 SILA-86hezi */

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE    true

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_NC
#define AUDIO_I2S_GPIO_MCLK      GPIO_NUM_40
#define AUDIO_I2S_GPIO_WS        GPIO_NUM_42
#define AUDIO_I2S_GPIO_BCLK      GPIO_NUM_41
#define AUDIO_I2S_GPIO_DIN       GPIO_NUM_2
#define AUDIO_I2S_GPIO_DOUT      GPIO_NUM_1
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_5
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_4
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR

/* IO0 = RGB DB0：禁止当按键，否则 RGB 翻转会假触发 ToggleChat / 音频测试狂切 */
#define BOOT_BUTTON_GPIO         GPIO_NUM_NC

#define BSP_LCD_VSYNC            GPIO_NUM_21
#define BSP_LCD_HSYNC            GPIO_NUM_14
#define BSP_LCD_DE               GPIO_NUM_47
#define BSP_LCD_PCLK             GPIO_NUM_48
#define BSP_LCD_DISP             GPIO_NUM_NC
#define BSP_LCD_DATA0            GPIO_NUM_0
#define BSP_LCD_DATA1            GPIO_NUM_12
#define BSP_LCD_DATA2            GPIO_NUM_11
#define BSP_LCD_DATA3            GPIO_NUM_10
#define BSP_LCD_DATA4            GPIO_NUM_9
#define BSP_LCD_DATA5            GPIO_NUM_46
#define BSP_LCD_DATA6            GPIO_NUM_3
#define BSP_LCD_DATA7            GPIO_NUM_20
#define BSP_LCD_DATA8            GPIO_NUM_19
#define BSP_LCD_DATA9            GPIO_NUM_8
#define BSP_LCD_DATA10           GPIO_NUM_18
#define BSP_LCD_DATA11           GPIO_NUM_17
#define BSP_LCD_DATA12           GPIO_NUM_16
#define BSP_LCD_DATA13           GPIO_NUM_15
#define BSP_LCD_DATA14           GPIO_NUM_7
#define BSP_LCD_DATA15           GPIO_NUM_6

#define BSP_LCD_IO_SPI_CS        GPIO_NUM_45
#define BSP_LCD_IO_SPI_SCL       GPIO_NUM_38
#define BSP_LCD_IO_SPI_SDA       GPIO_NUM_39
#define BSP_LCD_RST              GPIO_NUM_NC

#define DISPLAY_WIDTH            480
#define DISPLAY_HEIGHT           480
#define DISPLAY_SWAP_XY          false
#define DISPLAY_MIRROR_X         false
#define DISPLAY_MIRROR_Y         false
#define DISPLAY_OFFSET_X         0
#define DISPLAY_OFFSET_Y         0

/* huang.bin：IO13、PWM 取反、25 kHz。RY3730 EN 低电平关断，与 PWM invert 不是同一电平。 */
#define DISPLAY_BACKLIGHT_PIN              GPIO_NUM_13
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT    true
#define DISPLAY_BACKLIGHT_FREQ_HZ          25000
#define DISPLAY_BACKLIGHT_OFF_LEVEL        0

#define RGB_BOUNCE_BUFFER_SIZE   (DISPLAY_WIDTH * 20)

#endif // _BOARD_CONFIG_H_
