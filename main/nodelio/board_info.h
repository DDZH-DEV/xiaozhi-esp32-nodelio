#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* id;
    uint16_t width;
    uint16_t height;
    uint8_t grid_cols;
    uint8_t grid_rows;
    uint16_t nav_width; /* zero = bottom tabs */
    bool network_sleep;
    uint16_t result_width;
    uint16_t result_max_height;
    uint16_t chat_bottom;
} nodelio_display_profile_t;

const nodelio_display_profile_t* nodelio_display_profile(void);

typedef struct {
    const char *board_id;
    uint16_t display_w;
    uint16_t display_h;
    bool touch;
    bool audio;
    bool wake_word_capable;
} nodelio_board_info_t;

typedef struct {
    nodelio_board_info_t info;
} nodelio_board_t;

const nodelio_board_t *nodelio_board_get(void);
/** Board setup only, before registration/heartbeat tasks start. */
void nodelio_board_set_touch_available(bool available);

#ifdef __cplusplus
}
#endif
