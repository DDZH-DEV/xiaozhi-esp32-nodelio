/*
 * lunar_date.h — 公历转农历，用于墙板日历顶栏。
 *
 * 表驱动实现，覆盖 1900-2100。无动态分配，可在 LVGL 回调里直接调用。
 * 农历月/日名称与干支生肖均由本模块格式化，调用方只负责显示。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int year;       /**< 农历年（干支所属年，非公历年） */
    int month;      /**< 1-12 */
    int day;        /**< 1-30 */
    bool leap;      /**< 该月为闰月 */
} nodelio_lunar_t;

/**
 * 公历转农历。超出 1900-2100 或日期非法时返回 false。
 * 注意：时区由调用方决定，本函数只做日历换算。
 */
bool nodelio_lunar_from_solar(int year, int month, int day, nodelio_lunar_t *out);

/**
 * 格式化农历月名，如「正月」「冬月」「闰六月」。返回写入长度。
 */
int nodelio_lunar_month_name(const nodelio_lunar_t *lunar, char *buf, size_t len);

/**
 * 格式化农历日名，如「初一」「十五」「廿四」「三十」。返回写入长度。
 */
int nodelio_lunar_day_name(const nodelio_lunar_t *lunar, char *buf, size_t len);

/**
 * 格式化干支年，如「丙午」。返回写入长度。
 */
int nodelio_lunar_ganzhi(const nodelio_lunar_t *lunar, char *buf, size_t len);

/**
 * 生肖，如「马」。返回写入长度。
 */
int nodelio_lunar_zodiac(const nodelio_lunar_t *lunar, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
