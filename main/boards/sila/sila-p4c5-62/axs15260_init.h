#pragma once

#include <esp_lcd_panel_io.h>

#ifdef __cplusplus
extern "C" {
#endif
// Send before creating the DPI panel. Caller owns reset, DBI and DSI lifetime.
esp_err_t sila_axs15260_init(esp_lcd_panel_io_handle_t io);
#ifdef __cplusplus
}
#endif
