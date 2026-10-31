#pragma once

#include <cstdint>

struct SilaPpaFillStats {
    uint32_t completed;
    uint32_t fallback;
};

// Call after lvgl_port_init, under the LVGL lock. The draw unit lives until
// lv_deinit; all drawing and counters are owned by the LVGL context.
bool SilaPpaFillInit();
SilaPpaFillStats SilaPpaFillGetStats();
