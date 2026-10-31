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
    if (action == 0 || action == 1) {
        s_daily_weather = action == 1;
    } else if (action == 10) {
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

static void daily_gesture(lv_event_t*) {
    lv_indev_t* indev = lv_indev_active();
    if (!indev)
        return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_RIGHT && dir != LV_DIR_LEFT)
        return;
    s_daily_weather = dir == LV_DIR_RIGHT;
    lv_indev_wait_release(indev);
    daily_redraw();
}

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
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, UI_BORDER, 0);
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

static lv_obj_t* daily_button(lv_obj_t* parent, const char* title, int action,
                              bool selected = false) {
    lv_obj_t* button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, UI_BTN_H, UI_BTN_H);
    lv_obj_set_style_radius(button, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(button, selected ? UI_TILE_SWITCH : UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(button, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, selected ? 1 : 0, 0);
    lv_obj_set_style_border_color(button, UI_ACCENT, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, daily_action, LV_EVENT_CLICKED, (void*)(intptr_t)action);
    lv_obj_t* label = daily_label(button, title, selected ? UI_ACCENT_SOFT : UI_TEXT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
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
    lv_obj_t* monthbar = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(monthbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    daily_button(monthbar, "上月", 11);
    char title[64];
    snprintf(title, sizeof(title), "%d 年 %d 月", s_daily_year, s_daily_month);
    lv_obj_t* month = daily_label(monthbar, title, UI_TEXT, true);
    lv_obj_set_width(month, 0);
    lv_obj_set_flex_grow(month, 1);
    lv_obj_set_style_text_align(month, LV_TEXT_ALIGN_CENTER, 0);
    daily_button(monthbar, "下月", 12);
    lv_obj_set_width(daily_button(monthbar, "今天", 10), 56);

    lv_obj_t* grid = daily_box(body, LV_FLEX_FLOW_ROW_WRAP, true);
    lv_obj_set_style_pad_all(grid, UI_PAD_SM, 0);
    lv_obj_set_style_pad_row(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    static const char* week[] = {"一", "二", "三", "四", "五", "六", "日"};
    for (int i = 0; i < 7; ++i) {
        lv_obj_t* label = daily_label(grid, week[i], i > 4 ? UI_INFO : UI_TEXT_DIM);
        lv_obj_set_width(label, lv_pct(14));
        lv_obj_set_height(label, 20);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
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
    const int cells = ((offset + days + 6) / 7) * 7;
    for (int i = 0; i < cells; ++i) {
        const int day = i - offset + 1;
        const bool valid = day > 0 && day <= days;
        const bool current = valid && synced && s_daily_year == today.tm_year + 1900 &&
                             s_daily_month == today.tm_mon + 1 && day == today.tm_mday;
        lv_obj_t* cell = lv_obj_create(grid);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, lv_pct(14), 24);
        lv_obj_clear_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        if (current) {
            lv_obj_set_style_bg_color(cell, UI_TILE_SWITCH, 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, UI_RADIUS_XS, 0);
        }
        char number[8];
        snprintf(number, sizeof(number), "%d", day);
        lv_obj_t* label =
            daily_label(cell, valid ? number : " ", current ? UI_ACCENT_SOFT : UI_TEXT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
}

static void daily_weather(lv_obj_t* body) {
    const auto& daily = s_env->daily;
    lv_obj_t* hero = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_bg_color(hero, UI_TILE_PWM, 0);
    char headline[112];
    snprintf(headline, sizeof(headline), "%s   %s", daily.city[0] ? daily.city : "当地天气",
             daily.temperature[0] ? daily.temperature : "—");
    daily_label(hero, headline, UI_TEXT, true);
    daily_label(hero, daily.summary[0] ? daily.summary : "等待天气数据", UI_TEXT_SUB);
    if (daily.message[0])
        daily_label(hero, daily.message, UI_WARN);
    if (daily.updated[0]) {
        char updated[80];
        snprintf(updated, sizeof(updated), "天气更新 · %s（当地时间）", daily.updated);
        daily_label(hero, updated, UI_TEXT_DIM);
    }
    lv_obj_t* air = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    daily_label(air, "当前空气质量", UI_ACCENT_SOFT);
    daily_label(air, daily.air[0] ? daily.air : "空气质量暂不可用", UI_TEXT_SUB);
    if (daily.air_updated[0]) {
        char updated[80];
        snprintf(updated, sizeof(updated), "空气更新 · %s（当地时间）", daily.air_updated);
        daily_label(air, updated, UI_TEXT_DIM);
    }
    char forecast_title[80];
    snprintf(forecast_title, sizeof(forecast_title), "今天起 15 天天气 · 已获取 %d 天",
             daily.day_n);
    daily_label(body, forecast_title, UI_TEXT);
    lv_obj_t* forecast = daily_box(body, LV_FLEX_FLOW_ROW_WRAP, false);
    for (int i = 0; i < daily.day_n; ++i) {
        const auto& day = daily.days[i];
        lv_obj_t* tile = daily_box(forecast, LV_FLEX_FLOW_COLUMN, true);
        lv_obj_set_width(tile, lv_pct(48));
        lv_obj_set_style_pad_row(tile, 6, 0);
        char text[80];
        snprintf(text, sizeof(text), "%s%s", day.date + 5, i == 0 ? " · 今天" : "");
        daily_label(tile, text, i == 0 ? UI_ACCENT_SOFT : UI_TEXT_DIM);
        daily_label(tile, day.condition, UI_TEXT);
        snprintf(text, sizeof(text), "%s / %s", day.low[0] ? day.low : "—",
                 day.high[0] ? day.high : "—");
        daily_label(tile, text, UI_TEXT, true);
        snprintf(text, sizeof(text), "降水概率 %s", day.rain[0] ? day.rain : "—");
        daily_label(tile, text, UI_INFO);
    }
    daily_label(body, "天气 / 空气：Open-Meteo · CAMS\nUS AQI 为美国指数 · 左滑返回日历",
                UI_TEXT_DIM);
}

static void fill_calendar_weather(void) {
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    lv_obj_clean(s_card_list);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(s_card_list, UI_PAD_SM, 0);
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_AUTO);
    lv_label_set_text(s_card_sub,
                      s_daily_weather ? "15 天天气 · 当前空气质量" : "内置月历 · 右滑查看天气");
    lv_obj_t* body = daily_box(s_card_list, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, daily_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t* tabs = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_grow(daily_button(tabs, "日历", 0, !s_daily_weather), 1);
    lv_obj_set_flex_grow(daily_button(tabs, "天气与空气", 1, s_daily_weather), 1);
    if (s_daily_weather)
        daily_weather(body);
    else
        daily_calendar(body);
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
