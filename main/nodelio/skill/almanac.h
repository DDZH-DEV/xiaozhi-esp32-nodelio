/*
 * almanac.h — 墙板月历格子与点日详情。
 *
 * 节日、节气、日柱、黄道黑道、建除宜忌都在设备上算，不联网。
 * 宜忌按值日十二神归纳，供日常参考，不是完整通书。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NL_ALMANAC_LUNAR = 1,
    NL_ALMANAC_FESTIVAL = 2,
    NL_ALMANAC_TERM = 3,
};

typedef struct {
    char label[16];      /**< 格子副文：节日、节气或农历，最多两字 */
    int label_kind;      /**< NL_ALMANAC_*，0 表示没有副文 */
    char festivals[64];  /**< 全日节日全称，多项用「 · 」连接 */
    char term[16];       /**< 当日节气，没有则为空 */
    char lunar_line[64]; /**< 甲申日 · 丙午年九月廿八 · 属马 */
    char zhi_xing[8];    /**< 建除 */
    char tian_shen[8];   /**< 青龙、明堂等 */
    bool huangdao;
    char yi[48];
    char ji[48];
    char chong_sha[32]; /**< 冲虎 · 煞南 */
    char pengzu[80];
} nl_almanac_t;

/** 公历日期。超出可算范围时返回 false。 */
bool nl_almanac_build(int year, int month, int day, nl_almanac_t *out);

typedef struct {
    uint16_t year;
    const char *text;
} nl_hist_item_t;

/** 历史上的今天。按年份从早到晚写入 out，返回条数。 */
int nl_history_on_day(int month, int day, nl_hist_item_t *out, int cap);

#ifdef __cplusplus
}
#endif
