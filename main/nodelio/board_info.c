#include "board_info.h"

static const nodelio_board_t s_board = {
    .info =
        {
            .board_id = "sila_86",
            .display_w = 480,
            .display_h = 480,
            .touch = true,
            .audio = true,
            .wake_word_capable = true,
        },
};

const nodelio_board_t *nodelio_board_get(void)
{
    return &s_board;
}
