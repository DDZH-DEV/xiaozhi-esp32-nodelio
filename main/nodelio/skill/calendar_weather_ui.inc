/* Built-in daily view. Included by skill_result_ui.cc; no polling task or calendar dependency. */
static uint32_t s_daily_generation;
static bool s_daily_weather;
static int s_daily_year;
static int s_daily_month;
static int s_daily_date_key;

static bool daily_is_open(void) {
    return s_env && strcmp(s_env->card_type, "calendar_weather") == 0 && s_card &&
           !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_ui_closing;
}

static bool daily_today(struct tm* today) {
    const time_t now = time(nullptr);
    return localtime_r(&now, today) && today->tm_year >= 124;
}

static void daily_reset_month(void) {
    struct tm today{};
    if (daily_today(&today)) {
        s_daily_year = today.tm_year + 1900;
        s_daily_month = today.tm_mon + 1;
    } else {
        s_daily_year = s_daily_month = 0;
    }
}

static void fill_calendar_weather(void);
static void daily_pager_footer(lv_obj_t* parent);

static void daily_redraw(void) {
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock && daily_is_open()) {
            fill_calendar_weather();
        }
    });
}

static void daily_action(lv_event_t* e) {
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action == 10) {
        daily_reset_month();
    } else if (s_daily_year) {
        s_daily_month += action == 11 ? -1 : 1;
        if (s_daily_month < 1) {
            s_daily_month = 12;
            --s_daily_year;
        }
        if (s_daily_month > 12) {
            s_daily_month = 1;
            ++s_daily_year;
        }
        if (s_daily_year < 1970 || s_daily_year > 2099)
            daily_reset_month();
    }
    daily_redraw();
}

/* 两页之间左右滑动切换。日历在左、天气在右：向左滑进天气，向右滑回日历。 */
static void daily_gesture(lv_event_t*) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_RIGHT && dir != LV_DIR_LEFT)
        return;
    s_daily_weather = dir == LV_DIR_LEFT;
    lv_indev_wait_release(indev);
    daily_redraw();
}

/* ---- 基础构件 ---------------------------------------------------------- */

/** 排版容器：surface=true 时是一张无描边的卡片，只靠短阴影分层。
 *  不再画 1px 边框，避免整页到处是线条。 */
static lv_obj_t* daily_box(lv_obj_t* parent, lv_flex_flow_t flow, bool surface) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_style_pad_row(box, UI_GAP, 0);
    lv_obj_set_style_pad_column(box, UI_GAP, 0);
    lv_obj_clear_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (surface) {
        lv_obj_set_style_bg_color(box, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, UI_RADIUS_MD, 0);
        lv_obj_set_style_pad_all(box, UI_PAD, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        ui_style_card(box);
    }
    return box;
}

static lv_obj_t* daily_label(lv_obj_t* parent, const char* text, lv_color_t color,
                             bool heading = false) {
    lv_obj_t* label = lv_label_create(parent);
    nl_ov_set_font(label);
    if (heading)
        lv_obj_set_style_text_font(label, nl_ov_read_font(), 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text && text[0] ? text : "—");
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

/** 纯文字图标钮：无底色，靠 44px 触控区与前景色区分层级。 */
static lv_obj_t* daily_icon_btn(lv_obj_t* parent, const char* glyph, int action,
                                lv_color_t fg) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, daily_action, LV_EVENT_CLICKED, (void*)(intptr_t)action);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &font_material_symbols_20_4, 0);
    lv_label_set_text(label, glyph);
    lv_obj_set_style_text_color(label, fg, 0);
    lv_obj_center(label);
    return btn;
}

/** 文字钮：无底色无边线，只在按下时给一层淡底。用于「今天」。 */
static lv_obj_t* daily_icon_text_btn(lv_obj_t* parent, const char* text, int action,
                                     lv_color_t fg) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_left(btn, 12, 0);
    lv_obj_set_style_pad_right(btn, 12, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, daily_action, LV_EVENT_CLICKED, (void*)(intptr_t)action);
    lv_obj_t* label = daily_label(btn, text, fg);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return btn;
}

/* ---- 日历 -------------------------------------------------------------- */

/** 日期格高按结果卡可用高度倒推，避免整月溢出后还得滚动。
 *  头部已在精简模式下收起（只留浮动的关闭钮），固定开销只剩
 *  卡片 pad 16 + 月份条 44 + 两道间距 16 + 页脚 7 + 表头 18 + 行距 1。
 *  从最舒展的 60 往下试，直到整月确实落在卡内，留 6px 余量。 */
static int daily_cell_h(int rows) {
    const auto* p = nodelio_display_profile();
    const int card_h = p ? (int)p->result_max_height + NL_UI_STATUS_H : 460;
    const int fixed = 102;
    if (rows < 1)
        rows = 1;
    for (int cell = 60; cell > 30; --cell) {
        if (fixed + rows * (cell + 1) <= card_h - 6)
            return cell;
    }
    return 30;
}

/** 格内农历字：初一写月份（十月/冬月/腊月…），其余写日（初一/十五/廿四）。
 *  一律两字，避免单字「十」和「初十」混淆，也让每格宽度一致。 */
static void daily_lunar_cell_text(int year, int month, int day, char* buf, size_t len) {
    buf[0] = '\0';
    nodelio_lunar_t lunar{};
    if (!nodelio_lunar_from_solar(year, month, day, &lunar))
        return;
    if (lunar.leap) {
        snprintf(buf, len, "闰月");
        return;
    }
    if (lunar.day == 1) {
        /* 初一只显示月份，读作「这天进新的一月」。 */
        static const char* const kShortMonth[] = {"正月", "二月", "三月", "四月", "五月", "六月",
                                                  "七月", "八月", "九月", "十月", "冬月", "腊月"};
        snprintf(buf, len, "%s", kShortMonth[lunar.month - 1]);
        return;
    }
    /* 其余只显示日：初一…三十，都是两字。 */
    nodelio_lunar_day_name(&lunar, buf, len);
}

/** 星期表头：细一号、弱一级，只做方位提示，不画分隔线。 */
static void daily_week_head(lv_obj_t* grid) {
    static const char* week[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) {
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, lv_pct(14), 18);
        lv_obj_clear_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_t* label = daily_label(cell, week[i], i > 4 ? UI_TEXT_MUTED : UI_TEXT_DIM);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
}

static void daily_calendar(lv_obj_t* body) {
    struct tm today{};
    const bool synced = daily_today(&today);
    s_daily_date_key =
        synced ? (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday : 0;
    if (!s_daily_year && synced)
        daily_reset_month();
    if (!s_daily_year) {
        daily_label(body, "等待设备时间同步", UI_TEXT, true);
        daily_label(body, "联网校时后点击刷新。天气可右滑查看。", UI_TEXT_DIM);
        return;
    }

    /* 月份条：‹ 2026 年 11 月 › 与「今天」同行，右侧给浮动的关闭钮让出 44px。 */
    lv_obj_t* monthbar = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(monthbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(monthbar, 2, 0);
    lv_obj_set_style_pad_row(monthbar, 0, 0);
    lv_obj_set_style_pad_right(monthbar, 48, 0);
    daily_icon_btn(monthbar, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, 11, UI_TEXT_SUB);
    char title[64];
    snprintf(title, sizeof(title), "%d 年 %d 月", s_daily_year, s_daily_month);
    lv_obj_t* month = daily_label(monthbar, title, UI_TEXT, true);
    lv_obj_set_width(month, LV_SIZE_CONTENT);
    daily_icon_btn(monthbar, MATERIAL_SYMBOLS_KEYBOARD_ARROW_RIGHT, 12, UI_TEXT_SUB);
    lv_obj_t* spacer = lv_obj_create(monthbar);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_height(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    /* 「今天」改成无底色的文字钮，与两侧箭头同一视觉语言。 */
    daily_icon_text_btn(monthbar, "今天", 10, UI_ACCENT);

    struct tm first{};
    first.tm_year = s_daily_year - 1900;
    first.tm_mon = s_daily_month - 1;
    first.tm_mday = 1;
    first.tm_hour = 12;
    first.tm_isdst = -1;
    mktime(&first);
    const int offset = (first.tm_wday + 6) % 7;
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = s_daily_year % 4 == 0 && (s_daily_year % 100 != 0 || s_daily_year % 400 == 0);
    const int days = month_days[s_daily_month - 1] + (s_daily_month == 2 && leap ? 1 : 0);
    const int rows = (offset + days + 6) / 7;
    const int cells = rows * 7;

    /* 网格：无边框、无分隔线，靠留白分组。 */
    const int cell_h = daily_cell_h(rows);
    lv_obj_t* grid = daily_box(body, LV_FLEX_FLOW_ROW_WRAP, false);
    lv_obj_set_style_pad_row(grid, 1, 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    daily_week_head(grid);
    for (int i = 0; i < cells; ++i) {
        const int day = i - offset + 1;
        const bool valid = day > 0 && day <= days;
        const bool current = valid && synced && s_daily_year == today.tm_year + 1900 &&
                             s_daily_month == today.tm_mon + 1 && day == today.tm_mday;
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        /* 格子宽度必须保持 14%，否则整月会被挤成 6 列。 */
        lv_obj_set_size(cell, lv_pct(14), cell_h);
        lv_obj_clear_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        if (!valid)
            continue;

        /* 今天：整格淡绿底 + 圆角，不画圆盘，避免与两行文字打架。 */
        if (current) {
            lv_obj_set_style_bg_color(cell, UI_SELECTION, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, UI_RADIUS_SM, 0);
        }

        lv_obj_t* col = lv_obj_create(cell);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(col, 0, 0);
        lv_obj_clear_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_center(col);

        char number[12];
        snprintf(number, sizeof(number), "%d", day);
        lv_obj_t* num = daily_label(col, number, current ? UI_ACCENT : UI_TEXT);
        lv_obj_set_width(num, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
        if (current)
            lv_obj_set_style_text_font(num, nl_ov_read_font(), 0);

        char lunar_text[8];
        daily_lunar_cell_text(s_daily_year, s_daily_month, day, lunar_text, sizeof(lunar_text));
        if (lunar_text[0]) {
            lv_obj_t* sub = daily_label(col, lunar_text, current ? UI_ACCENT : UI_TEXT_MUTED);
            lv_obj_set_width(sub, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
            /* 农历小字固定用 14px 普惠，字号不随文本倍率放大，否则两行会撑破格子。 */
            lv_obj_set_style_text_font(sub, nl_ov_ui_font_compact(), 0);
        }
    }
}

/* ---- 天气 -------------------------------------------------------------- */

/* 天气图标：Material Symbols 里没有太阳/雨/雪，这里用 LVGL 图元自绘，
 * 不引入新字库。种类按 condition 文本归类，命中不了就用云。 */
typedef enum {
    DAILY_ICON_CLOUD = 0,
    DAILY_ICON_SUN,
    DAILY_ICON_PARTLY,
    DAILY_ICON_RAIN,
    DAILY_ICON_STORM,
    DAILY_ICON_SNOW,
    DAILY_ICON_FOG,
} daily_icon_t;

static daily_icon_t daily_icon_for(const char* condition) {
    if (!condition || !condition[0])
        return DAILY_ICON_CLOUD;
    if (strstr(condition, "雷"))
        return DAILY_ICON_STORM;
    if (strstr(condition, "雪") || strstr(condition, "冰"))
        return DAILY_ICON_SNOW;
    if (strstr(condition, "雨") || strstr(condition, "阵雨") || strstr(condition, "降水"))
        return DAILY_ICON_RAIN;
    if (strstr(condition, "雾") || strstr(condition, "霾") || strstr(condition, "沙") ||
        strstr(condition, "尘"))
        return DAILY_ICON_FOG;
    if (strstr(condition, "阴"))
        return DAILY_ICON_CLOUD;
    if (strstr(condition, "多云") || strstr(condition, "少云") || strstr(condition, "晴间"))
        return DAILY_ICON_PARTLY;
    if (strstr(condition, "晴"))
        return DAILY_ICON_SUN;
    return DAILY_ICON_CLOUD;
}

/** 图标主色：晴用暖黄、雨雪用信息蓝、雷用警示橙，其余走中性。 */
static uint32_t daily_icon_color(daily_icon_t icon) {
    switch (icon) {
    case DAILY_ICON_SUN:
    case DAILY_ICON_PARTLY:
        return ui_palette_is_dark() ? 0xFFD166 : 0xF5A623;
    case DAILY_ICON_RAIN:
    case DAILY_ICON_SNOW:
        return UI_C_INFO;
    case DAILY_ICON_STORM:
        return UI_C_WARN;
    default:
        return UI_C_TEXT_DIM;
    }
}

/** 一个纯色圆点，图标由若干圆/胶囊拼出来。 */
static lv_obj_t* daily_dot(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color,
                           int radius) {
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, w, h);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, radius, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return dot;
}

/**
 * 画天气图标。size 为图标外框边长，内部按比例摆放。
 * 只用圆与胶囊，避免引入图片资源与额外解码开销。
 */
static lv_obj_t* daily_weather_icon(lv_obj_t* parent, daily_icon_t icon, int size) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_clear_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    const uint32_t color = daily_icon_color(icon);
    const int u = size / 10; /* 一个比例单位 */
    const int cloud_w = size - u;
    const int cloud_h = u * 4;

    /* 云：一条胶囊底 + 两个凸起，避免画成一根实心条。
     * 云用中性灰而不是正文黑，否则在浅色卡上像一团墨。 */
    #define DAILY_CLOUD_C UI_C_TEXT_MUTED
    #define DAILY_DRAW_CLOUD(cx, cy)                                                          \
        do {                                                                                  \
            daily_dot(box, (cx), (cy) + cloud_h / 3, cloud_w, cloud_h - cloud_h / 3,          \
                      DAILY_CLOUD_C, (cloud_h - cloud_h / 3) / 2);                            \
            daily_dot(box, (cx) + cloud_w / 6, (cy), cloud_w / 2, cloud_h - cloud_h / 3,      \
                      DAILY_CLOUD_C, (cloud_h - cloud_h / 3) / 2);                            \
            daily_dot(box, (cx) + cloud_w / 2, (cy) + cloud_h / 6, cloud_w / 3, cloud_h / 2,  \
                      DAILY_CLOUD_C, cloud_h / 4);                                            \
        } while (0)

    switch (icon) {
    case DAILY_ICON_SUN:
        daily_dot(box, size / 2 - u * 2, size / 2 - u * 2, u * 4, u * 4, color, LV_RADIUS_CIRCLE);
        break;
    case DAILY_ICON_PARTLY:
        daily_dot(box, size / 2 - u / 2, u / 2, u * 4, u * 4, color, LV_RADIUS_CIRCLE);
        DAILY_DRAW_CLOUD(u / 2, size / 2 - u);
        break;
    case DAILY_ICON_RAIN:
    case DAILY_ICON_SNOW:
    case DAILY_ICON_STORM: {
        /* 云在上、雨滴紧贴云底，整组在图标框内垂直居中。
         * 组高 = 云高 + 雨滴高 - 重叠；用它算出顶部留白。 */
        const int group_h = cloud_h + u * 3 - u / 2;
        const int top = (size - group_h) / 2 > 0 ? (size - group_h) / 2 : 0;
        DAILY_DRAW_CLOUD(u / 2, top);
        const int rain_y = top + cloud_h - u / 2;
        for (int i = 0; i < 3; ++i) {
            daily_dot(box, u * 2 + i * u * 2, rain_y, u, u * 3, color, u / 2);
        }
        if (icon == DAILY_ICON_STORM) {
            daily_dot(box, size / 2 - u / 2, rain_y, u * 2, u * 3, color, u / 2);
        }
        break;
    }
    case DAILY_ICON_FOG:
        for (int i = 0; i < 3; ++i) {
            daily_dot(box, u, size / 4 + i * u * 2, size - u * 2, u, color, u / 2);
        }
        break;
    default:
        DAILY_DRAW_CLOUD(u / 2, size / 4);
        break;
    }
    #undef DAILY_DRAW_CLOUD
    #undef DAILY_CLOUD_C
    return box;
}

/** 把「US AQI 80 · 中等PM2.5 12 …」拆成数值、等级与颗粒物明细。
 *  只在 AQI 关键字之后找数字，否则「PM2.5 …」会把 2 当成 AQI。
 *  等级只取中文字符：服务器返回里「中等」与「PM2.5」之间可能没有空格。 */
static bool daily_split_aqi(const char* air, int* value, char* level, size_t level_len) {
    level[0] = '\0';
    if (!air || !air[0])
        return false;
    const char* base = strstr(air, "AQI");
    if (!base)
        return false;
    const char* num = nullptr;
    for (const char* p = base; *p; ++p) {
        if (*p >= '0' && *p <= '9') {
            /* 跳过 2.5 / 10 这类，只取第一个独立整数 */
            if (!(p[-1] >= '0' && p[-1] <= '9')) {
                num = p;
                break;
            }
        }
    }
    if (!num)
        return false;
    *value = atoi(num);
    /* 等级取 · 之后的中文字符串，遇到 ASCII（PM2.5 之类）即停。 */
    const char* dot = strstr(num, "·");
    if (!dot)
        dot = strstr(num, "•");
    if (dot) {
        const char* s = dot;
        while (*s && (unsigned char)*s >= 0x80)
            ++s; /* 跳过 · 本身 */
        while (*s == ' ')
            ++s;
        size_t n = 0;
        while (s[n] && (unsigned char)s[n] >= 0x80 && n + 1 < level_len) {
            level[n] = s[n];
            ++n;
        }
        level[n] = '\0';
    }
    return true;
}

/** 取颗粒物明细，即 AQI 等级之后剩下的那段；没有则返回 nullptr。 */
static const char* daily_pm_detail(const char* air) {
    if (!air)
        return nullptr;
    const char* pm = strstr(air, "PM2.5");
    if (!pm)
        pm = strstr(air, "PM10");
    return pm;
}

/** AQI 等级配色：优绿、良黄、轻度橙、中度红、重度紫。 */
static uint32_t daily_aqi_color(int aqi) {
    if (aqi <= 50)
        return UI_C_SUCCESS;
    if (aqi <= 100)
        return ui_palette_is_dark() ? 0xFFD166 : 0x985600;
    if (aqi <= 150)
        return UI_C_WARN;
    if (aqi <= 200)
        return UI_C_DANGER;
    return UI_C_PURPLE;
}

static void daily_weather(lv_obj_t* body) {
    const auto& daily = s_env->daily;

    /* 主视觉：左侧自绘天气图标，右侧城市 / 大号温度 / 体感描述。
     * 图标给了这一页第一个视觉锚点，温度用大字形成层级。 */
    lv_obj_t* hero = daily_box(body, LV_FLEX_FLOW_ROW, true);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hero, UI_PAD, 0);
    const daily_icon_t today_icon = daily_icon_for(daily.summary);
    daily_weather_icon(hero, today_icon, 72);

    lv_obj_t* hero_txt = daily_box(hero, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_set_width(hero_txt, 0);
    lv_obj_set_flex_grow(hero_txt, 1);
    lv_obj_set_style_pad_row(hero_txt, 1, 0);
    daily_label(hero_txt, daily.city[0] ? daily.city : "当地天气", UI_TEXT_DIM);
    daily_label(hero_txt, daily.temperature[0] ? daily.temperature : "—", UI_TEXT, true);
    daily_label(hero_txt, daily.summary[0] ? daily.summary : "等待天气数据", UI_TEXT_SUB);
    if (daily.message[0])
        daily_label(hero_txt, daily.message, UI_WARN);

    /* 空气质量：一行三列 —— AQI 数字 + 等级、颗粒物、更新时间。
     * 数字与等级并排，颗粒物与时间降到弱一级，避免整块散成三行。 */
    lv_obj_t* air = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_row(air, 6, 0);
    daily_label(air, "空气质量", UI_TEXT_DIM);

    int aqi = -1;
    char level[24];
    const bool has_aqi = daily_split_aqi(daily.air, &aqi, level, sizeof(level));
    const uint32_t aqi_c = daily_aqi_color(aqi < 0 ? 0 : aqi);

    lv_obj_t* air_row = daily_box(air, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(air_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(air_row, 0, 0);
    lv_obj_set_style_pad_column(air_row, UI_PAD_SM, 0);

    if (has_aqi) {
        /* 大号数字本身就是视觉主体，不再额外加圆点。 */
        char num[16];
        snprintf(num, sizeof(num), "%d", aqi);
        lv_obj_t* n = daily_label(air_row, num, UI_TEXT, true);
        lv_obj_set_width(n, LV_SIZE_CONTENT);
        lv_obj_set_style_text_color(n, lv_color_hex(aqi_c), 0);
        if (level[0]) {
            lv_obj_t* badge = lv_obj_create(air_row);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_left(badge, 8, 0);
            lv_obj_set_style_pad_right(badge, 8, 0);
            lv_obj_set_style_pad_top(badge, 2, 0);
            lv_obj_set_style_pad_bottom(badge, 2, 0);
            lv_obj_set_style_radius(badge, UI_RADIUS_XS, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(aqi_c), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
            lv_obj_clear_flag(badge,
                              (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_t* bl = daily_label(badge, level, UI_TEXT);
            lv_obj_set_width(bl, LV_SIZE_CONTENT);
            lv_obj_set_style_text_color(bl, lv_color_hex(aqi_c), 0);
            lv_obj_center(bl);
        }
    } else {
        daily_label(air_row, daily.air[0] ? daily.air : "暂不可用", UI_TEXT);
    }

    /* 颗粒物明细与更新时间同行靠右，弱一级。 */
    const char* pm = daily_pm_detail(daily.air);
    if (pm || daily.air_updated[0]) {
        lv_obj_t* meta = daily_box(air, LV_FLEX_FLOW_ROW, false);
        lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(meta, 0, 0);
        lv_obj_t* pm_lbl = daily_label(meta, pm ? pm : "", UI_TEXT_MUTED);
        lv_obj_set_width(pm_lbl, 0);
        lv_obj_set_flex_grow(pm_lbl, 1);
        lv_label_set_long_mode(pm_lbl, LV_LABEL_LONG_DOT);
        if (daily.air_updated[0]) {
            char updated[80];
            snprintf(updated, sizeof(updated), "更新 %s", daily.air_updated);
            lv_obj_t* u = daily_label(meta, updated, UI_TEXT_MUTED);
            lv_obj_set_width(u, LV_SIZE_CONTENT);
        }
    }

    /* 预报：每行一枚小图标 + 日期 / 天气 / 温度 / 降水。 */
    char forecast_title[80];
    snprintf(forecast_title, sizeof(forecast_title), "未来 %d 天", daily.day_n);
    daily_label(body, forecast_title, UI_TEXT);
    lv_obj_t* forecast = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_row(forecast, 0, 0);
    lv_obj_set_style_pad_top(forecast, 6, 0);
    lv_obj_set_style_pad_bottom(forecast, 6, 0);
    for (int i = 0; i < daily.day_n; ++i) {
        const auto& day = daily.days[i];
        const bool today_row = i == 0;
        lv_obj_t* row = daily_box(forecast, LV_FLEX_FLOW_ROW, false);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(row, 0, 0);
        lv_obj_set_style_pad_column(row, UI_GAP, 0);
        lv_obj_set_style_pad_top(row, 5, 0);
        lv_obj_set_style_pad_bottom(row, 5, 0);
        /* 不画分隔线：行距本身已经能区分，线条越少越干净。 */
        daily_weather_icon(row, daily_icon_for(day.condition), 28);

        char text[80];
        snprintf(text, sizeof(text), "%s%s", day.date + 5, today_row ? " 今天" : "");
        lv_obj_t* d = daily_label(row, text, today_row ? UI_ACCENT : UI_TEXT_SUB);
        lv_obj_set_width(d, lv_pct(30));
        lv_obj_t* c = daily_label(row, day.condition, UI_TEXT);
        lv_obj_set_width(c, 0);
        lv_obj_set_flex_grow(c, 1);
        snprintf(text, sizeof(text), "%s / %s", day.low[0] ? day.low : "—",
                 day.high[0] ? day.high : "—");
        lv_obj_t* t = daily_label(row, text, UI_TEXT);
        lv_obj_set_width(t, LV_SIZE_CONTENT);
        snprintf(text, sizeof(text), "%s", day.rain[0] ? day.rain : "—");
        lv_obj_t* r = daily_label(row, text, UI_INFO);
        lv_obj_set_width(r, 52);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
    }
    /* 数据来源压成一行，不再占两行。 */
    if (daily.updated[0]) {
        char foot[128];
        snprintf(foot, sizeof(foot), "Open-Meteo · CAMS · 更新 %s", daily.updated);
        daily_label(body, foot, UI_TEXT_MUTED);
    } else {
        daily_label(body, "天气 / 空气：Open-Meteo · CAMS", UI_TEXT_MUTED);
    }
}

/* ---- 页脚 -------------------------------------------------------------- */

/** 两页共用页脚：只有两枚小圆点，不加文字。手势提示交给卡片副标题。
 *  高度压到 10px，尽量把纵向空间让给日历格子。 */
static void daily_pager_footer(lv_obj_t* parent) {
    lv_obj_t* row = daily_box(parent, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 5, 0);
    lv_obj_set_style_pad_top(row, 2, 0);
    lv_obj_set_style_pad_bottom(row, 0, 0);
    for (int i = 0; i < 2; ++i) {
        const bool active = (i == 1) == s_daily_weather;
        lv_obj_t* dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, active ? UI_ACCENT : UI_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
}

static void fill_calendar_weather(void) {
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    /* 日历用精简头部：先收紧卡片内边距，daily_cell_h() 才是按 pad 8 算的。
     * apply_card_chrome() 在 fill 之后才跑，这里必须先行一步。 */
    if (s_card) {
        lv_obj_set_style_pad_all(s_card, 8, 0);
        lv_obj_set_style_pad_row(s_card, 0, 0);
    }
    lv_obj_clean(s_card_list);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(s_card_list, UI_PAD_SM, 0);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_AUTO);
    lv_label_set_text(s_card_sub,
                      s_daily_weather ? "15 天天气 · 当前空气质量" : "内置月历 · 左滑看天气");
    lv_obj_t* body = daily_box(s_card_list, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, daily_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 无标签：两页靠左右滑动切换，页脚用圆点与提示说明手势。 */
    if (s_daily_weather)
        daily_weather(body);
    else
        daily_calendar(body);
    daily_pager_footer(body);
    lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);
}

uint32_t nodelio_skill_daily_open(bool reset_view) {
    ++s_daily_generation;
    if (!s_daily_generation)
        ++s_daily_generation;
    if (!reset_view && daily_is_open()) {
        daily_redraw();
        return s_daily_generation;
    }
    auto* env = static_cast<nl_content_envelope_t*>(
        heap_caps_calloc(1, sizeof(nl_content_envelope_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!env) {
        nl_ov_toast("内存不足");
        return 0;
    }
    snprintf(env->status, sizeof(env->status), "ok");
    snprintf(env->app_id, sizeof(env->app_id), "builtin.daily");
    snprintf(env->card_type, sizeof(env->card_type), "calendar_weather");
    snprintf(env->display_title, sizeof(env->display_title), "日历与天气");
    snprintf(env->daily.message, sizeof(env->daily.message), "尚未获取天气，可联网后点击刷新");
    s_daily_weather = false;
    daily_reset_month();
    nodelio_skill_result_show(nullptr, env, false);
    return s_daily_generation;
}

void nodelio_skill_daily_clock_tick(void) {
    if (!daily_is_open() || s_daily_weather)
        return;
    struct tm today{};
    if (!daily_today(&today))
        return;
    const int key = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
    if (key != s_daily_date_key) {
        s_daily_date_key = key;
        daily_redraw();
    }
}

void nodelio_skill_daily_update(const nodelio_nvs_config_t* cfg, nl_content_envelope_t* env,
                                uint32_t generation) {
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || generation != s_daily_generation || !daily_is_open()) {
        free_owned_env(env);
        return;
    }
    const int scroll_y = s_card_list ? lv_obj_get_scroll_y(s_card_list) : 0;
    nodelio_skill_result_show(cfg, env, false);
    if (s_card_list && scroll_y > 0) {
        lv_obj_update_layout(s_card_list);
        lv_obj_scroll_to_y(s_card_list, scroll_y, LV_ANIM_OFF);
    }
}
