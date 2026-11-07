/*
 * almanac.c — 节日、节气、黄道与建除。
 *
 * 节气日表来自寿星历（1970-2099），日柱用儒略日，与常见黄历对齐。
 * 月建以「节」交节当天切换：立春寅月、惊蛰卯月，直到大雪子月。
 */
#include "almanac.h"

#include "lunar_date.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint8_t kTermDay[130][24] = {
#include "almanac_terms.inc"
};

static const char *const kTermName[] = {
    "小寒", "大寒", "立春", "雨水", "惊蛰", "春分", "清明", "谷雨", "立夏", "小满", "芒种", "夏至",
    "小暑", "大暑", "立秋", "处暑", "白露", "秋分", "寒露", "霜降", "立冬", "小雪", "大雪", "冬至",
};

/* 十二节对应的月支：小寒丑 … 大雪子。子=0。 */
static const int kJieZhi[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0};

static const char *const kStem[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"};
static const char *const kZhi[] = {"子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"};
static const char *const kAnimal[] = {"鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪"};
static const char *const kXing[] = {"建", "除", "满", "平", "定", "执", "破", "危", "成", "收", "开", "闭"};
static const char *const kShen[] = {"青龙", "明堂", "天刑", "朱雀", "金匮", "天德",
                                    "白虎", "玉堂", "天牢", "玄武", "司命", "勾陈"};
/* 月支 → 天神偏移，与黄历十二天神一致。 */
static const int kShenOff[12] = {4, 2, 0, 10, 8, 6, 4, 2, 0, 10, 8, 6};
static const char *const kSha[] = {"南", "东", "北", "西", "南", "东", "北", "西", "南", "东", "北", "西"};
static const char *const kYi[] = {
    "祈福 上任 出行", "扫舍 沐浴 求医", "开市 嫁娶 祈福", "修造 涂饰",     "嫁娶 开市 入宅", "祭祀 捕捉",
    "破屋 求医",     "安床 祭祀",       "嫁娶 入学 开市", "纳财 收获 捕捉", "开市 出行 求财", "筑堤 安葬",
};
static const char *const kJi[] = {
    "动土 开仓", "嫁娶 远行", "栽种 服药", "嫁娶 开市", "诉讼 远行", "移居 开市",
    "嫁娶 签约", "登高 远行", "诉讼",     "开仓 安葬", "安葬",     "开市 出行",
};
static const char *const kPengGan[] = {
    "甲不开仓财物耗散", "乙不栽植千株不长", "丙不修灶必见灾殃", "丁不剃头头必生疮", "戊不受田田主不祥",
    "己不破券二比并亡", "庚不经络织机虚张", "辛不合酱主人不尝", "壬不泱水更难提防", "癸不词讼理弱敌强",
};
static const char *const kPengZhi[] = {
    "子不问卜自惹祸殃", "丑不冠带主不还乡", "寅不祭祀神鬼不尝", "卯不穿井水泉不香", "辰不哭泣必主重丧",
    "巳不远行财物伏藏", "午不苫盖屋主更张", "未不服药毒气入肠", "申不安床鬼祟入房", "酉不会客醉坐颠狂",
    "戌不吃犬作怪上床", "亥不嫁娶不利新郎",
};

static int term_day(int year, int index) {
    if (year < 1970 || year > 2099 || index < 0 || index > 23) {
        return 0;
    }
    return kTermDay[year - 1970][index];
}

static int term_on(int year, int month, int day) {
    for (int i = 0; i < 24; ++i) {
        if (month == i / 2 + 1 && day == term_day(year, i)) {
            return i;
        }
    }
    return -1;
}

static int month_zhi(int year, int month, int day) {
    int zhi = 0;
    for (int j = 0; j < 12; ++j) {
        const int idx = j * 2;
        const int tm = idx / 2 + 1;
        const int td = term_day(year, idx);
        if (td && (month > tm || (month == tm && day >= td))) {
            zhi = kJieZhi[j];
        }
    }
    return zhi;
}

static int julian_day(int year, int month, int day) {
    const int a = (14 - month) / 12;
    const int y = year + 4800 - a;
    const int m = month + 12 * a - 3;
    return day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
}

static bool next_solar(int year, int month, int day, int *oy, int *om, int *od) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day + 1;
    t.tm_hour = 12;
    t.tm_isdst = -1;
    if (mktime(&t) == (time_t)-1) {
        return false;
    }
    *oy = t.tm_year + 1900;
    *om = t.tm_mon + 1;
    *od = t.tm_mday;
    return true;
}

static void add_festival(nl_almanac_t *out, int *best, int rank, const char *full, const char *brief) {
    if (out->festivals[0]) {
        const size_t used = strlen(out->festivals);
        snprintf(out->festivals + used, sizeof(out->festivals) - used, " · %s", full);
    } else {
        snprintf(out->festivals, sizeof(out->festivals), "%s", full);
    }
    if (rank < *best) {
        *best = rank;
        snprintf(out->label, sizeof(out->label), "%s", brief);
        out->label_kind = NL_ALMANAC_FESTIVAL;
    }
}

static void fill_festivals(nl_almanac_t *out, int year, int month, int day, const nodelio_lunar_t *lunar) {
    int best = 100;
    if (month == 1 && day == 1) {
        add_festival(out, &best, 30, "元旦", "元旦");
    } else if (month == 3 && day == 8) {
        add_festival(out, &best, 40, "妇女节", "妇女");
    } else if (month == 3 && day == 12) {
        add_festival(out, &best, 41, "植树节", "植树");
    } else if (month == 5 && day == 1) {
        add_festival(out, &best, 20, "劳动节", "劳动");
    } else if (month == 5 && day == 4) {
        add_festival(out, &best, 42, "青年节", "青年");
    } else if (month == 6 && day == 1) {
        add_festival(out, &best, 43, "儿童节", "儿童");
    } else if (month == 7 && day == 1) {
        add_festival(out, &best, 21, "建党节", "建党");
    } else if (month == 8 && day == 1) {
        add_festival(out, &best, 44, "建军节", "建军");
    } else if (month == 9 && day == 10) {
        add_festival(out, &best, 45, "教师节", "教师");
    } else if (month == 10 && day == 1) {
        add_festival(out, &best, 10, "国庆节", "国庆");
    }

    if (term_on(year, month, day) == 6) {
        add_festival(out, &best, 3, "清明节", "清明");
    }

    if (!lunar->leap) {
        if (lunar->month == 1 && lunar->day == 1) {
            add_festival(out, &best, 1, "春节", "春节");
        } else if (lunar->month == 1 && lunar->day == 15) {
            add_festival(out, &best, 4, "元宵节", "元宵");
        } else if (lunar->month == 2 && lunar->day == 2) {
            add_festival(out, &best, 12, "龙抬头", "龙头");
        } else if (lunar->month == 3 && lunar->day == 3) {
            add_festival(out, &best, 13, "上巳节", "上巳");
        } else if (lunar->month == 5 && lunar->day == 5) {
            add_festival(out, &best, 2, "端午节", "端午");
        } else if (lunar->month == 7 && lunar->day == 7) {
            add_festival(out, &best, 5, "七夕", "七夕");
        } else if (lunar->month == 7 && lunar->day == 15) {
            add_festival(out, &best, 8, "中元节", "中元");
        } else if (lunar->month == 8 && lunar->day == 15) {
            add_festival(out, &best, 2, "中秋节", "中秋");
        } else if (lunar->month == 9 && lunar->day == 9) {
            add_festival(out, &best, 6, "重阳节", "重阳");
        } else if (lunar->month == 10 && lunar->day == 1) {
            add_festival(out, &best, 9, "寒衣节", "寒衣");
        } else if (lunar->month == 10 && lunar->day == 15) {
            add_festival(out, &best, 11, "下元节", "下元");
        } else if (lunar->month == 12 && lunar->day == 8) {
            add_festival(out, &best, 7, "腊八节", "腊八");
        } else if (lunar->month == 12 && lunar->day == 23) {
            add_festival(out, &best, 8, "北方小年", "小年");
        } else if (lunar->month == 12 && lunar->day == 24) {
            add_festival(out, &best, 8, "南方小年", "小年");
        }
    }

    int ny = 0, nm = 0, nd = 0;
    nodelio_lunar_t next;
    if (next_solar(year, month, day, &ny, &nm, &nd) && nodelio_lunar_from_solar(ny, nm, nd, &next) &&
        !next.leap && next.month == 1 && next.day == 1) {
        add_festival(out, &best, 1, "除夕", "除夕");
    }
}

static void lunar_label(const nodelio_lunar_t *lunar, char *buf, size_t len) {
    if (lunar->leap) {
        snprintf(buf, len, "闰月");
        return;
    }
    if (lunar->day == 1) {
        static const char *const kShortMonth[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                                  "七月", "八月", "九月", "十月", "冬月", "腊月"};
        snprintf(buf, len, "%s", kShortMonth[lunar->month - 1]);
        return;
    }
    nodelio_lunar_day_name(lunar, buf, len);
}

bool nl_almanac_build(int year, int month, int day, nl_almanac_t *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    nodelio_lunar_t lunar;
    if (!nodelio_lunar_from_solar(year, month, day, &lunar)) {
        return false;
    }

    char gz[8];
    char month_name[16];
    char day_name[16];
    char zodiac[8];
    nodelio_lunar_ganzhi(&lunar, gz, sizeof(gz));
    nodelio_lunar_month_name(&lunar, month_name, sizeof(month_name));
    nodelio_lunar_day_name(&lunar, day_name, sizeof(day_name));
    nodelio_lunar_zodiac(&lunar, zodiac, sizeof(zodiac));

    const int off = julian_day(year, month, day) - 11;
    int gan = off % 10;
    int zhi = off % 12;
    if (gan < 0) {
        gan += 10;
    }
    if (zhi < 0) {
        zhi += 12;
    }
    const int mz = month_zhi(year, month, day);
    int xing = zhi - mz;
    if (xing < 0) {
        xing += 12;
    }
    const int shen = (zhi + kShenOff[mz]) % 12;

    snprintf(out->lunar_line, sizeof(out->lunar_line), "%s%s日 · %s年%s%s · 属%s", kStem[gan], kZhi[zhi], gz,
             month_name, day_name, zodiac);
    snprintf(out->zhi_xing, sizeof(out->zhi_xing), "%s", kXing[xing]);
    snprintf(out->tian_shen, sizeof(out->tian_shen), "%s", kShen[shen]);
    out->huangdao = shen == 0 || shen == 1 || shen == 4 || shen == 5 || shen == 7 || shen == 10;
    snprintf(out->yi, sizeof(out->yi), "%s", kYi[xing]);
    snprintf(out->ji, sizeof(out->ji), "%s", kJi[xing]);
    snprintf(out->chong_sha, sizeof(out->chong_sha), "冲%s · 煞%s", kAnimal[(zhi + 6) % 12], kSha[zhi]);
    snprintf(out->pengzu, sizeof(out->pengzu), "%s · %s", kPengGan[gan], kPengZhi[zhi]);

    const int term = term_on(year, month, day);
    if (term >= 0) {
        snprintf(out->term, sizeof(out->term), "%s", kTermName[term]);
    }
    fill_festivals(out, year, month, day, &lunar);
    if (!out->label_kind) {
        if (out->term[0]) {
            snprintf(out->label, sizeof(out->label), "%s", out->term);
            out->label_kind = NL_ALMANAC_TERM;
        } else {
            lunar_label(&lunar, out->label, sizeof(out->label));
            if (out->label[0]) {
                out->label_kind = NL_ALMANAC_LUNAR;
            }
        }
    }
    return true;
}
