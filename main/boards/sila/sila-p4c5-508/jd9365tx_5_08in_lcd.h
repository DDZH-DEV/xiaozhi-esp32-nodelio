// -*- coding: utf-8 -*-
// JD9365TX 5.08 寸 720x720 MIPI-DSI（HL050IHD9084ANT / IVO G050PH）。
// 点亮路径：Generic Long Write 厂家序列 + RGB565 DPI；触摸后置。
#pragma once

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_ldo_regulator.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JD9365TX_5_08IN_H_RES 720
#define JD9365TX_5_08IN_V_RES 720

/** 建 DSI/DPI、硬复位、下发厂家 init；背光由板级管。调用方拥有返回句柄生命周期。 */
esp_err_t sila_jd9365tx_5_08in_init(gpio_num_t pin_rst, esp_ldo_channel_handle_t *out_phy_ldo,
                                    esp_lcd_dsi_bus_handle_t *out_dsi_bus,
                                    esp_lcd_panel_io_handle_t *out_io,
                                    esp_lcd_panel_handle_t *out_panel);

#ifdef __cplusplus
}
#endif
