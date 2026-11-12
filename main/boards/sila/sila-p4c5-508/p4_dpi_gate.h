#pragma once

#include <stdbool.h>

#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 空闲停 DPI 扫描。不拆 DSI 总线、不放 PHY LDO、不复位屏。 */
void p4_dpi_set_scan(esp_lcd_panel_handle_t panel, bool on);

#ifdef __cplusplus
}
#endif
