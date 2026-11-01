#pragma once
#include <lvgl.h>
// All calls require the display lock. The screen is the panel backdrop; global
// UI remains on layer_top so screenshots and existing modal ordering stay valid.
void nl_panel_shell_setup(const lv_font_t* font);
void nl_panel_shell_notify(const char* msg, const lv_font_t* font, int bottom_inset,
                           int duration_ms);
/** 丢掉已有提示，下次按当前调色板重建。 */
void nl_panel_shell_dismiss_toast(void);
/** 提示条可见时抬到最前。 */
void nl_panel_shell_raise_toast(void);
