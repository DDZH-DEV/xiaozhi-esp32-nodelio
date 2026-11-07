/*
 * lunar_date.c — 公历转农历（1900-2100）。
 *
 * 表项编码（每个农历年 20 bit）：
 *   bit 0-3   ：闰月月份，0 表示无闰月
 *   bit 4-15  ：正月至十二月的大小，1=30 天，0=29 天（bit4 为正月）
 *   bit 16    ：闰月大小，1=30 天，0=29 天
 *
 * 表来源为通行的 1900-2100 农历数据；已用 2020-2030 十个已知春节日期
 * 与中秋、除夕等锚点验证（见 tools 侧脚本）。
 */
#include "lunar_date.h"

#include <stdio.h>

#define LUNAR_BASE_YEAR 1900
#define LUNAR_BASE_DAY_OFFSET (-25537) /* 1900-01-31 相对 1970-01-01 的天数 */

static const uint32_t kLunarInfo[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0,
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0,
    0x0a2e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160,
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,
    0x0d520,
};

#define LUNAR_YEARS ((int)(sizeof(kLunarInfo) / sizeof(kLunarInfo[0])))

static const char *const kMonthNames[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                          "七月", "八月", "九月", "十月", "冬月", "腊月"};
static const char *const kDayTens[] = {"初", "十", "廿", "三"};
static const char *const kDayUnits[] = {"十", "一", "二", "三", "四",
                                        "五", "六", "七", "八", "九"};
static const char kStems[] = "甲乙丙丁戊己庚辛壬癸";
static const char kBranches[] = "子丑寅卯辰巳午未申酉戌亥";
static const char kZodiac[] = "鼠牛虎兔龙蛇马羊猴鸡狗猪";

static int leap_month(int year) { return (int)(kLunarInfo[year - LUNAR_BASE_YEAR] & 0xf); }

static int leap_days(int year) {
    if (!leap_month(year)) {
        return 0;
    }
    return (kLunarInfo[year - LUNAR_BASE_YEAR] & 0x10000) ? 30 : 29;
}

static int month_days(int year, int month) {
    return (kLunarInfo[year - LUNAR_BASE_YEAR] & (0x10000u >> month)) ? 30 : 29;
}

static int year_days(int year) {
    int total = 0;
    for (int m = 1; m <= 12; ++m) {
        total += month_days(year, m);
    }
    return total + leap_days(year);
}

/** 公历转「相对 1970-01-01 的天数」，Howard Hinnant 的 days_from_civil。 */
static int days_from_civil(int y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool nodelio_lunar_from_solar(int year, int month, int day, nodelio_lunar_t *out) {
    if (!out || year < LUNAR_BASE_YEAR || year >= LUNAR_BASE_YEAR + LUNAR_YEARS || month < 1 ||
        month > 12 || day < 1 || day > 31) {
        return false;
    }
    int offset = days_from_civil(year, month, day) - LUNAR_BASE_DAY_OFFSET;
    if (offset < 0) {
        return false;
    }
    int ly = LUNAR_BASE_YEAR;
    for (; ly < LUNAR_BASE_YEAR + LUNAR_YEARS; ++ly) {
        const int yd = year_days(ly);
        if (offset < yd) {
            break;
        }
        offset -= yd;
    }
    if (ly >= LUNAR_BASE_YEAR + LUNAR_YEARS) {
        return false;
    }
    const int leap = leap_month(ly);
    for (int lm = 1; lm <= 12; ++lm) {
        const int md = month_days(ly, lm);
        if (offset < md) {
            out->year = ly;
            out->month = lm;
            out->day = offset + 1;
            out->leap = false;
            return true;
        }
        offset -= md;
        if (leap && lm == leap) {
            const int ld = leap_days(ly);
            if (offset < ld) {
                out->year = ly;
                out->month = lm;
                out->day = offset + 1;
                out->leap = true;
                return true;
            }
            offset -= ld;
        }
    }
    return false;
}

int nodelio_lunar_month_name(const nodelio_lunar_t *lunar, char *buf, size_t len) {
    if (!lunar || !buf || !len || lunar->month < 1 || lunar->month > 12) {
        return 0;
    }
    const char *name = kMonthNames[lunar->month - 1];
    return snprintf(buf, len, "%s%s", lunar->leap ? "闰" : "", name);
}

int nodelio_lunar_day_name(const nodelio_lunar_t *lunar, char *buf, size_t len) {
    if (!lunar || !buf || !len || lunar->day < 1 || lunar->day > 30) {
        return 0;
    }
    if (lunar->day == 10) {
        return snprintf(buf, len, "初十");
    }
    if (lunar->day == 20) {
        return snprintf(buf, len, "二十");
    }
    if (lunar->day == 30) {
        return snprintf(buf, len, "三十");
    }
    return snprintf(buf, len, "%s%s", kDayTens[lunar->day / 10], kDayUnits[lunar->day % 10]);
}

/** 干支表是 UTF-8，每个汉字 3 字节。%c 只会写出首字节。 */
static int copy_han(const char *table, int index, char *buf, size_t len) {
    const char *s = table + index * 3;
    if (!buf || len < 4) {
        return 0;
    }
    buf[0] = s[0];
    buf[1] = s[1];
    buf[2] = s[2];
    buf[3] = '\0';
    return 3;
}

int nodelio_lunar_ganzhi(const nodelio_lunar_t *lunar, char *buf, size_t len) {
    if (!lunar || !buf || len < 7) {
        return 0;
    }
    const int idx = (lunar->year - 1984) % 60;
    const int pos = idx < 0 ? idx + 60 : idx;
    copy_han(kStems, pos % 10, buf, len);
    copy_han(kBranches, pos % 12, buf + 3, len - 3);
    return 6;
}

int nodelio_lunar_zodiac(const nodelio_lunar_t *lunar, char *buf, size_t len) {
    if (!lunar || !buf || !len) {
        return 0;
    }
    const int idx = (lunar->year - 1984) % 12;
    const int pos = idx < 0 ? idx + 12 : idx;
    return copy_han(kZodiac, pos, buf, len);
}
