#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nodelio_battery_summary {
    int pct;          /* 0–100；无电池为 -1 */
    bool charging;
    bool ready;       /* EWMA 已可信 */
    int i_avg_ma;     /* ready 时有效 */
    int remain_min;   /* ready 且放电时有效；充电中为 -1 */
    bool screen_on;
    bool wake_on_sleep;
    bool net_asleep;
} nodelio_battery_summary_t;

void nodelio_battery_stats_init(void);

/**
 * 由板级 idle 轮询调用（约 1Hz）。内部按亮/息屏节流采 PMU。
 * 无电池时 no-op。
 */
void nodelio_battery_stats_tick(void);

void nodelio_battery_stats_get_summary(nodelio_battery_summary_t *out);

/** 写入 {"pct":..,"charging":..,...}（无外层 battery 键）。失败返回 -1。 */
int nodelio_battery_stats_format_json(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
