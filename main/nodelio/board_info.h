#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
