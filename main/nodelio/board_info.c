#include "board_info.h"
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_SILA_P4C5_62
#define NL_BOARD_ID "sila_p4c5_62"
#define NL_WIDTH 1280
#define NL_HEIGHT 452
static const nodelio_display_profile_t s_profile = {
    .id = "wide_1280x452",
    .width = NL_WIDTH,
    .height = NL_HEIGHT,
    .grid_cols = 10,
    .grid_rows = 3,
    .nav_width = 96,
    .network_sleep = true,
    .result_width = 720,
    .result_max_height = 396,
    .chat_bottom = 80,
    .shell_style = "immersive",
    .dock_width = 72,
    .dock_rect = {14, 12, 72, 428},
    .hero_rect = {108, 12, 468, 428},
    .list_rect = {592, 12, 672, 428},
};
#elif CONFIG_BOARD_TYPE_SILA_P4C5_508
#define NL_BOARD_ID "sila_p4c5_508"
#define NL_WIDTH 720
#define NL_HEIGHT 720
static const nodelio_display_profile_t s_profile = {
    .id = "square_720x720",
    .width = NL_WIDTH,
    .height = NL_HEIGHT,
    .grid_cols = 6,
    .grid_rows = 6,
    .nav_width = 0,
    .network_sleep = true,
    .result_width = 620,
    .result_max_height = 640,
    .chat_bottom = 72,
    .shell_style = "immersive",
    .dock_width = 68,
    .dock_rect = {12, 12, 68, 696},
    .hero_rect = {92, 12, 612, 314},
    .list_rect = {92, 338, 612, 370},
};
#else
#define NL_BOARD_ID "sila_86"
#define NL_WIDTH 480
#define NL_HEIGHT 480
static const nodelio_display_profile_t s_profile = {
    .id = "square_480x480",
    .width = NL_WIDTH,
    .height = NL_HEIGHT,
    .grid_cols = 6,
    .grid_rows = 6,
    .nav_width = 0,
    .network_sleep = true,
    .result_width = 412,
    .result_max_height = 424,
    .chat_bottom = 52,
    .shell_style = "classic",
    .dock_width = 0,
    .dock_rect = {0, 0, 0, 0},
    .hero_rect = {0, 0, 0, 0},
    .list_rect = {0, 0, 0, 0},
};
#endif

static nodelio_board_t s_board = {
    .info = {.board_id = NL_BOARD_ID,
             .display_w = NL_WIDTH,
             .display_h = NL_HEIGHT,
             .touch = true,
             .audio = true,
             .wake_word_capable = true},
};

const nodelio_board_t* nodelio_board_get(void) { return &s_board; }
void nodelio_board_set_touch_available(bool available) { s_board.info.touch = available; }
const nodelio_display_profile_t* nodelio_display_profile(void) { return &s_profile; }
