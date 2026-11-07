#include "skill_ui_priv.h"
#include "almanac.h"

/* Built-in daily view. No polling task or calendar dependency. */
static uint32_t s_daily_generation;
static bool s_daily_weather;
static int s_daily_year;
static int s_daily_month;
static int s_daily_date_key;
static int s_daily_focus; /* 年月日，0 表示停在月历 */

/** 月历圆点：当前查看年月的有提醒日期（bit d = 日 d）。 */
static uint32_t s_rem_day_bits;
static int s_rem_bits_year;
static int s_rem_bits_month;
static bool s_rem_fetching;

enum { kDailyRemMax = 12 };
struct daily_rem_item {
    int id;
    char title[48];
    char time[8];
    char recur[12];
    bool missed;
};
static daily_rem_item s_rem_day_items[kDailyRemMax];
static int s_rem_day_n;
static int s_rem_day_key; /* 已加载的 focus 日 YYYYMMDD */

static bool daily_rem_has(int day) {
    return day >= 1 && day <= 31 && (s_rem_day_bits & (1u << day)) != 0;
}

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

static void daily_pager_footer(lv_obj_t* parent);

static void daily_redraw(void) {
    Application::GetInstance().Schedule([]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock && daily_is_open()) {
            fill_calendar_weather();
        }
    });
}

static void daily_reminders_fetch_month(void);
static void daily_reminders_fetch_day(int ymd);

/** 日详情：完成这一次，或删除整条（含以后的重复）。 */
static void daily_reminder_mutate(int id, bool cancel) {
    if (id <= 0)
        return;
    nodelio_nvs_config_t cfg{};
    nl_ov_cfg_for_http(&cfg);
    if (!cfg.server_url[0] || !cfg.device_id[0] || !cfg.device_key[0]) {
        nl_ov_toast("未绑定服务");
        return;
    }
    struct arg_t {
        int id;
        bool cancel;
    };
    auto* arg = static_cast<arg_t*>(malloc(sizeof(arg_t)));
    if (!arg)
        return;
    arg->id = id;
    arg->cancel = cancel;
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive,
            [](void* p) {
                auto* a = static_cast<arg_t*>(p);
                const int id = a->id;
                const bool cancel = a->cancel;
                free(p);
                nodelio_nvs_config_t cfg{};
                nl_ov_cfg_for_http(&cfg);
                char base[160];
                nl_url_strip_base(cfg.server_url, base, sizeof(base));
                char url[288];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/reminders/%d/%s", base,
                         cfg.device_id, id, cancel ? "cancel" : "done");
                char resp[256] = {0};
                int status = 0;
                (void)nodelio_http_post_json(url, cfg.device_key, "{}", &status, resp, sizeof(resp) - 1);
                Application::GetInstance().Schedule([status, cancel]() {
                    if (status == 200) {
                        nl_ov_toast(cancel ? "已删除" : "已完成");
                        s_rem_day_key = 0;
                        s_rem_bits_year = 0;
                        daily_reminders_fetch_month();
                        if (s_daily_focus)
                            daily_reminders_fetch_day(s_daily_focus);
                        daily_redraw();
                    } else {
                        nl_ov_toast("操作失败");
                    }
                });
            },
            arg, [](void* p) { free(p); })) {
        free(arg);
    }
}

static void daily_action(lv_event_t* e) {
    const int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action >= 300000) {
        daily_reminder_mutate(action - 300000, true);
        return;
    }
    if (action >= 200000) {
        daily_reminder_mutate(action - 200000, false);
        return;
    }
    if (action == 40) {
        s_daily_focus = 0;
    } else if (action >= 1 && action <= 31) {
        if (s_daily_year)
            s_daily_focus = s_daily_year * 10000 + s_daily_month * 100 + action;
    } else if (action == 10) {
        s_daily_focus = 0;
        daily_reset_month();
    } else if (s_daily_year && (action == 11 || action == 12)) {
        s_daily_focus = 0;
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
        s_rem_bits_year = 0;
    }
    daily_redraw();
    if (s_daily_focus)
        daily_reminders_fetch_day(s_daily_focus);
    else if (!s_daily_weather)
        daily_reminders_fetch_month();
}

/* 两页之间左右滑动切换。日历在左、天气在右：向左滑进天气，向右滑回日历。
 * 点开某一天时先看详情，滑动留给正文。 */
static void daily_gesture(lv_event_t*) {
    if (s_daily_focus)
        return;
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

/* ---- 提醒（账号级，HTTP；打开/换月/收到变更时拉） ---------------------- */

static void daily_reminders_fetch_month(void) {
    if (!s_daily_year || s_rem_fetching)
        return;
    if (s_rem_bits_year == s_daily_year && s_rem_bits_month == s_daily_month)
        return;
    nodelio_nvs_config_t cfg{};
    nl_ov_cfg_for_http(&cfg);
    if (!cfg.server_url[0] || !cfg.device_id[0] || !cfg.device_key[0])
        return;
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip())
        return;
    s_rem_fetching = true;
    struct job_t {
        int year;
        int month;
        nodelio_nvs_config_t cfg;
    };
    auto* job = static_cast<job_t*>(malloc(sizeof(job_t)));
    if (!job) {
        s_rem_fetching = false;
        return;
    }
    job->year = s_daily_year;
    job->month = s_daily_month;
    job->cfg = cfg;
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto* j = static_cast<job_t*>(p);
                char base[160];
                nl_url_strip_base(j->cfg.server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url),
                         "%s/api/devices/%s/panel/reminders?year=%d&month=%d", base,
                         j->cfg.device_id, j->year, j->month);
                char buf[2048];
                buf[0] = '\0';
                int status = 0;
                esp_err_t err =
                    nodelio_http_get_ms(url, j->cfg.device_key, nullptr, &status, buf,
                                       sizeof(buf) - 1, 6000);
                uint32_t bits = 0;
                if (err == ESP_OK && status == 200) {
                    cJSON* root = cJSON_Parse(buf);
                    const cJSON* days =
                        root ? cJSON_GetObjectItemCaseSensitive(root, "days") : nullptr;
                    if (cJSON_IsArray(days)) {
                        const cJSON* d = nullptr;
                        cJSON_ArrayForEach(d, days) {
                            if (cJSON_IsNumber(d)) {
                                const int day = (int)d->valuedouble;
                                if (day >= 1 && day <= 31)
                                    bits |= (1u << day);
                            }
                        }
                    }
                    if (root)
                        cJSON_Delete(root);
                }
                const int y = j->year;
                const int m = j->month;
                free(j);
                Application::GetInstance().Schedule([bits, y, m]() {
                    s_rem_fetching = false;
                    s_rem_day_bits = bits;
                    s_rem_bits_year = y;
                    s_rem_bits_month = m;
                    if (daily_is_open() && !s_daily_weather && !s_daily_focus &&
                        s_daily_year == y && s_daily_month == m)
                        daily_redraw();
                });
            },
            job, [](void* p) { free(p); })) {
        free(job);
        s_rem_fetching = false;
    }
}

static void daily_reminders_fetch_day(int ymd) {
    if (ymd <= 0)
        return;
    if (s_rem_day_key == ymd)
        return;
    nodelio_nvs_config_t cfg{};
    nl_ov_cfg_for_http(&cfg);
    if (!cfg.server_url[0] || !cfg.device_id[0] || !cfg.device_key[0])
        return;
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip())
        return;
    struct job_t {
        int ymd;
        nodelio_nvs_config_t cfg;
    };
    auto* job = static_cast<job_t*>(malloc(sizeof(job_t)));
    if (!job)
        return;
    job->ymd = ymd;
    job->cfg = cfg;
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto* j = static_cast<job_t*>(p);
                const int y = j->ymd / 10000;
                const int m = (j->ymd / 100) % 100;
                const int d = j->ymd % 100;
                char base[160];
                nl_url_strip_base(j->cfg.server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url),
                         "%s/api/devices/%s/panel/reminders?date=%04d-%02d-%02d", base,
                         j->cfg.device_id, y, m, d);
                char buf[4096];
                buf[0] = '\0';
                int status = 0;
                (void)nodelio_http_get_ms(url, j->cfg.device_key, nullptr, &status, buf,
                                          sizeof(buf) - 1, 6000);
                daily_rem_item items[kDailyRemMax];
                memset(items, 0, sizeof(items));
                int n = 0;
                if (status == 200) {
                    cJSON* root = cJSON_Parse(buf);
                    const cJSON* arr =
                        root ? cJSON_GetObjectItemCaseSensitive(root, "items") : nullptr;
                    if (cJSON_IsArray(arr)) {
                        const cJSON* it = nullptr;
                        cJSON_ArrayForEach(it, arr) {
                            if (n >= kDailyRemMax)
                                break;
                            const cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
                            const cJSON* title = cJSON_GetObjectItemCaseSensitive(it, "title");
                            const cJSON* tm = cJSON_GetObjectItemCaseSensitive(it, "time");
                            const cJSON* recur = cJSON_GetObjectItemCaseSensitive(it, "recur");
                            const cJSON* missed = cJSON_GetObjectItemCaseSensitive(it, "missed");
                            const cJSON* st = cJSON_GetObjectItemCaseSensitive(it, "status");
                            if (cJSON_IsString(st) && st->valuestring &&
                                strcmp(st->valuestring, "active") != 0)
                                continue;
                            if (!cJSON_IsNumber(id) || !cJSON_IsString(title) || !title->valuestring)
                                continue;
                            items[n].id = (int)id->valuedouble;
                            snprintf(items[n].title, sizeof(items[n].title), "%s", title->valuestring);
                            if (cJSON_IsString(tm) && tm->valuestring)
                                snprintf(items[n].time, sizeof(items[n].time), "%s", tm->valuestring);
                            if (cJSON_IsString(recur) && recur->valuestring)
                                snprintf(items[n].recur, sizeof(items[n].recur), "%s",
                                         recur->valuestring);
                            items[n].missed = cJSON_IsTrue(missed);
                            n++;
                        }
                    }
                    if (root)
                        cJSON_Delete(root);
                }
                const int key = j->ymd;
                free(j);
                auto* pack = static_cast<daily_rem_item*>(
                    malloc(sizeof(daily_rem_item) * (size_t)kDailyRemMax + sizeof(int) * 2));
                if (!pack)
                    return;
                memcpy(pack, items, sizeof(daily_rem_item) * (size_t)n);
                int* meta = reinterpret_cast<int*>(pack + kDailyRemMax);
                meta[0] = n;
                meta[1] = key;
                Application::GetInstance().Schedule([pack]() {
                    const int* meta = reinterpret_cast<const int*>(pack + kDailyRemMax);
                    const int n = meta[0];
                    const int key = meta[1];
                    memcpy(s_rem_day_items, pack, sizeof(daily_rem_item) * (size_t)n);
                    s_rem_day_n = n;
                    s_rem_day_key = key;
                    free(pack);
                    if (daily_is_open() && s_daily_focus == key)
                        daily_redraw();
                });
            },
            job, [](void* p) { free(p); })) {
        free(job);
    }
}

void nodelio_skill_daily_reminders_invalidate(void) {
    s_rem_bits_year = 0;
    s_rem_bits_month = 0;
    s_rem_day_key = 0;
    Application::GetInstance().Schedule([]() {
        if (!daily_is_open())
            return;
        if (s_daily_focus)
            daily_reminders_fetch_day(s_daily_focus);
        else if (!s_daily_weather)
            daily_reminders_fetch_month();
    });
}

/* ---- 日历 -------------------------------------------------------------- */

/** 日期行之间的留白。表头到首行、以及各日期行之间都走这一档。
 *  今天的色块铺满格子，这一档太小色块会贴上相邻日期。 */
static const int kCalRowGap = 12;

/** 日期格高按结果卡可用高度倒推，避免整月溢出后还得滚动。
 *  头部已在精简模式下收起（只留浮动的关闭钮），固定开销：
 *  卡片 pad、月份条、表头、点日期提示和页脚圆点。
 *  日历页把正文行距收到 2px，把省下的高度让给日期行间隔。
 *  从最舒展的 60 往下试，直到整月确实落在卡内，留 6px 余量。 */
static int daily_cell_h(int rows) {
    const auto* p = nodelio_display_profile();
    const int card_h = p ? (int)p->result_max_height + NL_UI_STATUS_H : 460;
    const int fixed = 106;
    if (rows < 1)
        rows = 1;
    for (int cell = 60; cell > 30; --cell) {
        if (fixed + rows * (cell + kCalRowGap) <= card_h - 6)
            return cell;
    }
    return 30;
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
    /* 月份条 / 网格 / 提示之间少留一点，把高度让给日期行间隔。 */
    lv_obj_set_style_pad_row(body, 2, 0);
    struct tm today{};
    const bool synced = daily_today(&today);
    s_daily_date_key =
        synced ? (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday : 0;
    if (!s_daily_year && synced)
        daily_reset_month();
    if (!s_daily_year) {
        daily_label(body, "等待设备时间同步", UI_TEXT, true);
        daily_label(body, "联网校时后点击刷新。左滑查看天气。", UI_TEXT_DIM);
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
    lv_obj_set_style_pad_row(grid, kCalRowGap, 0);
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
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        if (!valid) {
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            continue;
        }
        lv_obj_add_flag(cell, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
        lv_obj_add_event_cb(cell, daily_action, LV_EVENT_CLICKED, (void*)(intptr_t)day);
        lv_obj_set_style_bg_color(cell, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_STATE_PRESSED);

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

        nl_almanac_t info{};
        if (nl_almanac_build(s_daily_year, s_daily_month, day, &info) && info.label[0]) {
            lv_color_t sub_c = UI_TEXT_MUTED;
            if (info.label_kind == NL_ALMANAC_FESTIVAL)
                sub_c = UI_DANGER;
            else if (info.label_kind == NL_ALMANAC_TERM)
                sub_c = UI_WARN;
            else if (current)
                sub_c = UI_ACCENT;
            lv_obj_t* sub = daily_label(col, info.label, sub_c);
            lv_obj_set_width(sub, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
            /* 副文固定用 14px，字号不随文本倍率放大，否则两行会撑破格子。 */
            lv_obj_set_style_text_font(sub, nl_ov_ui_font_compact(), 0);
        }
        /* 有提醒的日期：副文下方一颗小圆点，不改公历/农历两行。 */
        if (daily_rem_has(day)) {
            lv_obj_t* dot = lv_obj_create(col);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 5, 5);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, UI_ACCENT, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        }
    }

    lv_obj_t* hint = daily_label(body, "点日期看黄历 · 左滑看天气", UI_TEXT_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, nl_ov_ui_font_compact(), 0);
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

static void format_time_clean(const char* in, char* out, size_t out_sz) {
    if (!in || !in[0]) {
        out[0] = '\0';
        return;
    }
    const char* t = strchr(in, 'T');
    if (t) {
        snprintf(out, out_sz, "%s", t + 1);
        char* colon = strrchr(out, ':');
        if (colon && colon - out > 3) {
            *colon = '\0';
        }
    } else {
        snprintf(out, out_sz, "%s", in);
    }
}

static void daily_weather(lv_obj_t* body) {
    const auto& daily = s_env->daily;
    const daily_icon_t today_icon = daily_icon_for(daily.summary);

    /* 1. 主天气卡片 (Hero Card) */
    lv_obj_t* hero = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_style_pad_row(hero, 10, 0);

    /* 顶行：城市名称 + 天气状况胶囊 */
    lv_obj_t* top_row = daily_box(hero, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* city_lbl = lv_label_create(top_row);
    nl_ov_set_font(city_lbl);
    lv_label_set_text(city_lbl, daily.city[0] ? daily.city : "当地天气");
    lv_obj_set_style_text_color(city_lbl, UI_TEXT, 0);
    lv_obj_set_style_text_font(city_lbl, nl_ov_ui_font_title(), 0);

    const char* cond_str = daily.summary[0] ? daily.summary : "多云";
    lv_obj_t* cond_badge = lv_obj_create(top_row);
    lv_obj_remove_style_all(cond_badge);
    lv_obj_set_size(cond_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(cond_badge, 10, 0);
    lv_obj_set_style_pad_right(cond_badge, 10, 0);
    lv_obj_set_style_pad_top(cond_badge, 4, 0);
    lv_obj_set_style_pad_bottom(cond_badge, 4, 0);
    lv_obj_set_style_radius(cond_badge, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(cond_badge, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(cond_badge, LV_OPA_COVER, 0);
    lv_obj_t* cbl = lv_label_create(cond_badge);
    nl_ov_set_font(cbl);
    lv_label_set_text(cbl, cond_str);
    lv_obj_set_style_text_color(cbl, UI_ACCENT, 0);
    lv_obj_center(cbl);

    /* 中间行：超大温度 + 气温范围/描述 + 右侧天气大图标 */
    lv_obj_t* main_row = daily_box(hero, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(main_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* temp_col = daily_box(main_row, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_set_flex_grow(temp_col, 1);
    lv_obj_set_style_pad_row(temp_col, 2, 0);

    char temp_buf[32];
    if (daily.temperature[0] && strcmp(daily.temperature, "—") != 0) {
        if (strstr(daily.temperature, "°") || strstr(daily.temperature, "℃")) {
            snprintf(temp_buf, sizeof(temp_buf), "%s", daily.temperature);
        } else {
            snprintf(temp_buf, sizeof(temp_buf), "%s°", daily.temperature);
        }
    } else {
        snprintf(temp_buf, sizeof(temp_buf), "--°");
    }
    lv_obj_t* t_big = lv_label_create(temp_col);
    lv_obj_set_style_text_font(t_big, nl_ov_read_font(), 0);
    lv_label_set_text(t_big, temp_buf);
    lv_obj_set_style_text_color(t_big, UI_TEXT, 0);

    /* 最高 / 最低温或简要说明 */
    char sub_temp[160];
    if (daily.day_n > 0 && daily.days[0].high[0]) {
        snprintf(sub_temp, sizeof(sub_temp), "最高 %s · 最低 %s", daily.days[0].high, daily.days[0].low);
    } else {
        snprintf(sub_temp, sizeof(sub_temp), "%s", daily.summary[0] ? daily.summary : "正在同步气象数据");
    }
    lv_obj_t* sub_l = lv_label_create(temp_col);
    nl_ov_set_font(sub_l);
    lv_label_set_text(sub_l, sub_temp);
    lv_obj_set_style_text_color(sub_l, UI_TEXT_DIM, 0);

    /* 右侧图标底座 */
    lv_obj_t* icon_pod = lv_obj_create(main_row);
    lv_obj_remove_style_all(icon_pod);
    lv_obj_set_size(icon_pod, 60, 60);
    lv_obj_set_style_radius(icon_pod, UI_RADIUS_MD, 0);
    lv_obj_set_style_bg_color(icon_pod, UI_SURFACE_PRESSED, 0);
    lv_obj_set_style_bg_opa(icon_pod, LV_OPA_COVER, 0);
    lv_obj_t* wic = daily_weather_icon(icon_pod, today_icon, 48);
    lv_obj_center(wic);

    /* 底部提示小横条（如有告警/未联网刷新提示） */
    if (daily.message[0]) {
        lv_obj_t* tip_box = lv_obj_create(hero);
        lv_obj_remove_style_all(tip_box);
        lv_obj_set_width(tip_box, lv_pct(100));
        lv_obj_set_style_pad_left(tip_box, 10, 0);
        lv_obj_set_style_pad_right(tip_box, 10, 0);
        lv_obj_set_style_pad_top(tip_box, 6, 0);
        lv_obj_set_style_pad_bottom(tip_box, 6, 0);
        lv_obj_set_style_radius(tip_box, UI_RADIUS_SM, 0);
        lv_obj_set_style_bg_color(tip_box, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_opa(tip_box, LV_OPA_COVER, 0);
        lv_obj_t* tip_l = lv_label_create(tip_box);
        nl_ov_set_font(tip_l);
        lv_label_set_text(tip_l, daily.message);
        lv_obj_set_style_text_color(tip_l, UI_WARN, 0);
        lv_obj_center(tip_l);
    }

    /* 2. 空气质量卡片 (Air Quality) */
    lv_obj_t* air = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_all(air, 14, 0);
    lv_obj_set_style_pad_row(air, 8, 0);

    /* 标题 + 更新时间 */
    lv_obj_t* air_top = daily_box(air, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(air_top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* air_tit = lv_label_create(air_top);
    nl_ov_set_font(air_tit);
    lv_label_set_text(air_tit, "空气质量");
    lv_obj_set_style_text_color(air_tit, UI_TEXT_DIM, 0);

    if (daily.air_updated[0]) {
        char time_clean[32];
        format_time_clean(daily.air_updated, time_clean, sizeof(time_clean));
        char up_str[48];
        snprintf(up_str, sizeof(up_str), "%s 更新", time_clean);
        lv_obj_t* up_l = lv_label_create(air_top);
        nl_ov_set_font(up_l);
        lv_label_set_text(up_l, up_str);
        lv_obj_set_style_text_color(up_l, UI_TEXT_MUTED, 0);
    }

    int aqi = -1;
    char level[24];
    const bool has_aqi = daily_split_aqi(daily.air, &aqi, level, sizeof(level));
    const uint32_t aqi_c = daily_aqi_color(aqi < 0 ? 0 : aqi);

    lv_obj_t* air_content = daily_box(air, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(air_content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (has_aqi) {
        lv_obj_t* aqi_left = daily_box(air_content, LV_FLEX_FLOW_ROW, false);
        lv_obj_set_flex_align(aqi_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(aqi_left, 10, 0);

        char num[16];
        snprintf(num, sizeof(num), "%d", aqi);
        lv_obj_t* n = lv_label_create(aqi_left);
        nl_ov_set_font(n);
        lv_obj_set_style_text_font(n, nl_ov_read_font(), 0);
        lv_label_set_text(n, num);
        lv_obj_set_style_text_color(n, lv_color_hex(aqi_c), 0);

        if (level[0]) {
            lv_obj_t* badge = lv_obj_create(aqi_left);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_left(badge, 8, 0);
            lv_obj_set_style_pad_right(badge, 8, 0);
            lv_obj_set_style_pad_top(badge, 3, 0);
            lv_obj_set_style_pad_bottom(badge, 3, 0);
            lv_obj_set_style_radius(badge, UI_RADIUS_XS, 0);
            lv_obj_set_style_bg_color(badge, lv_color_hex(aqi_c), 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
            lv_obj_t* bl = lv_label_create(badge);
            nl_ov_set_font(bl);
            lv_label_set_text(bl, level);
            lv_obj_set_style_text_color(bl, lv_color_hex(aqi_c), 0);
            lv_obj_center(bl);
        }
    } else {
        lv_obj_t* na_l = lv_label_create(air_content);
        nl_ov_set_font(na_l);
        lv_label_set_text(na_l, daily.air[0] ? daily.air : "等待更新");
        lv_obj_set_style_text_color(na_l, UI_TEXT_MUTED, 0);
    }

    const char* pm = daily_pm_detail(daily.air);
    if (pm) {
        lv_obj_t* pm_l = lv_label_create(air_content);
        nl_ov_set_font(pm_l);
        lv_label_set_text(pm_l, pm);
        lv_obj_set_style_text_color(pm_l, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(pm_l, LV_TEXT_ALIGN_RIGHT, 0);
    }

    /* 3. 未来预报列表 (Forecast List) */
    if (daily.day_n > 0) {
        lv_obj_t* fc_hdr = lv_label_create(body);
        nl_ov_set_font(fc_hdr);
        char f_title[32];
        snprintf(f_title, sizeof(f_title), "未来 %d 天天气预报", daily.day_n);
        lv_label_set_text(fc_hdr, f_title);
        lv_obj_set_style_text_color(fc_hdr, UI_TEXT_DIM, 0);

        lv_obj_t* forecast = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
        lv_obj_set_style_pad_all(forecast, 12, 0);
        lv_obj_set_style_pad_row(forecast, 4, 0);

        for (int i = 0; i < daily.day_n; ++i) {
            const auto& day = daily.days[i];
            const bool today_row = i == 0;
            lv_obj_t* row = daily_box(forecast, LV_FLEX_FLOW_ROW, false);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            lv_obj_set_style_pad_column(row, 8, 0);

            daily_weather_icon(row, daily_icon_for(day.condition), 24);

            char date_buf[32];
            snprintf(date_buf, sizeof(date_buf), "%s%s", day.date + 5, today_row ? " (今天)" : "");
            lv_obj_t* d = lv_label_create(row);
            nl_ov_set_font(d);
            lv_label_set_text(d, date_buf);
            lv_obj_set_style_text_color(d, today_row ? UI_ACCENT : UI_TEXT_SUB, 0);
            lv_obj_set_width(d, lv_pct(30));

            lv_obj_t* c = lv_label_create(row);
            nl_ov_set_font(c);
            lv_label_set_text(c, day.condition[0] ? day.condition : "—");
            lv_obj_set_style_text_color(c, UI_TEXT, 0);
            lv_obj_set_flex_grow(c, 1);

            char t_range[32];
            snprintf(t_range, sizeof(t_range), "%s ~ %s", day.low[0] ? day.low : "—",
                     day.high[0] ? day.high : "—");
            lv_obj_t* tr = lv_label_create(row);
            nl_ov_set_font(tr);
            lv_label_set_text(tr, t_range);
            lv_obj_set_style_text_color(tr, UI_TEXT, 0);
            lv_obj_set_width(tr, LV_SIZE_CONTENT);
        }
    }

    /* 4. 底部微弱数据来源 */
    lv_obj_t* foot_l = lv_label_create(body);
    nl_ov_set_font(foot_l);
    lv_label_set_text(foot_l, "右滑查看月历 · Open-Meteo · CAMS");
    lv_obj_set_style_text_color(foot_l, UI_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(foot_l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(foot_l, lv_pct(100));
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

static lv_obj_t* daily_chip(lv_obj_t* parent, const char* text, lv_color_t fg) {
    lv_obj_t* chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(chip, 8, 0);
    lv_obj_set_style_pad_right(chip, 8, 0);
    lv_obj_set_style_pad_top(chip, 3, 0);
    lv_obj_set_style_pad_bottom(chip, 3, 0);
    lv_obj_set_style_radius(chip, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(chip, fg, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
    lv_obj_clear_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t* label = daily_label(chip, text, fg);
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(label, nl_ov_ui_font_compact(), 0);
    return chip;
}

static void daily_pair_row(lv_obj_t* parent, const char* tag, lv_color_t tag_c, const char* text) {
    lv_obj_t* row = daily_box(parent, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_t* mark = daily_label(row, tag, tag_c);
    lv_obj_set_width(mark, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(mark, nl_ov_ui_font_compact(), 0);
    lv_obj_t* value = daily_label(row, text, UI_TEXT);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_width(value, lv_pct(80));
}

/** 点开某一天：黄历在上，历史上的今天在下，整页可滚动。 */
static void daily_day(lv_obj_t* body) {
    const int y = s_daily_focus / 10000;
    const int m = (s_daily_focus / 100) % 100;
    const int d = s_daily_focus % 100;

    lv_obj_t* head = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 2, 0);
    lv_obj_set_style_pad_right(head, 48, 0);
    daily_icon_btn(head, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, 40, UI_TEXT_SUB);

    nl_almanac_t info{};
    if (!nl_almanac_build(y, m, d, &info)) {
        daily_label(head, "这一天暂不能换算黄历", UI_TEXT, true);
        return;
    }

    struct tm when{};
    when.tm_year = y - 1900;
    when.tm_mon = m - 1;
    when.tm_mday = d;
    when.tm_hour = 12;
    when.tm_isdst = -1;
    mktime(&when);
    static const char* week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

    char title[48];
    snprintf(title, sizeof(title), "%d月%d日 %s", m, d, week[when.tm_wday]);
    lv_obj_t* title_l = daily_label(head, title, UI_TEXT, true);
    lv_obj_set_width(title_l, LV_SIZE_CONTENT);

    lv_obj_t* lunar = daily_label(body, info.lunar_line, UI_TEXT_SUB);
    lv_obj_set_style_text_font(lunar, nl_ov_ui_font_compact(), 0);

    if (info.festivals[0] || info.term[0]) {
        lv_obj_t* chips = daily_box(body, LV_FLEX_FLOW_ROW_WRAP, false);
        lv_obj_set_style_pad_column(chips, 6, 0);
        if (info.festivals[0])
            daily_chip(chips, info.festivals, UI_DANGER);
        if (info.term[0] && !strstr(info.festivals, info.term))
            daily_chip(chips, info.term, UI_WARN);
    }

    lv_obj_t* luck = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_row(luck, 6, 0);
    char luck_line[32];
    snprintf(luck_line, sizeof(luck_line), "%s · %s", info.huangdao ? "黄道吉日" : "黑道日", info.tian_shen);
    lv_obj_t* luck_l = daily_label(luck, luck_line, info.huangdao ? UI_SUCCESS : UI_TEXT_SUB, true);
    lv_obj_set_width(luck_l, lv_pct(100));
    char duty[64];
    snprintf(duty, sizeof(duty), "值日 %s · %s", info.zhi_xing, info.chong_sha);
    lv_obj_t* duty_l = daily_label(luck, duty, UI_TEXT_DIM);
    lv_obj_set_style_text_font(duty_l, nl_ov_ui_font_compact(), 0);
    daily_pair_row(luck, "宜", UI_SUCCESS, info.yi);
    daily_pair_row(luck, "忌", UI_DANGER, info.ji);
    lv_obj_t* peng = daily_label(luck, info.pengzu, UI_TEXT_MUTED);
    lv_obj_set_style_text_font(peng, nl_ov_ui_font_compact(), 0);

    /* 提醒：黄历下、历史上的今天上；今天的排服务端顺序（下次触发在前）。 */
    lv_obj_t* rem_title = daily_label(body, "提醒", UI_TEXT_DIM);
    lv_obj_set_style_text_font(rem_title, nl_ov_ui_font_compact(), 0);
    lv_obj_t* rem = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_row(rem, 6, 0);
    if (s_rem_day_key != s_daily_focus) {
        lv_obj_t* loading = daily_label(rem, "加载提醒…", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(loading, nl_ov_ui_font_compact(), 0);
    } else if (s_rem_day_n <= 0) {
        lv_obj_t* empty = daily_label(rem, "这一天没有提醒", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(empty, nl_ov_ui_font_compact(), 0);
    } else {
        for (int i = 0; i < s_rem_day_n; ++i) {
            const daily_rem_item* it = &s_rem_day_items[i];
            lv_obj_t* row = lv_obj_create(rem);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_set_style_pad_ver(row, 4, 0);
            lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

            char left[24];
            if (it->time[0])
                snprintf(left, sizeof(left), "%s", it->time);
            else if (strcmp(it->recur, "daily") == 0)
                snprintf(left, sizeof(left), "每天");
            else if (strcmp(it->recur, "weekly") == 0)
                snprintf(left, sizeof(left), "每周");
            else
                snprintf(left, sizeof(left), "一次");
            lv_obj_t* tm = daily_label(row, left, it->missed ? UI_WARN : UI_ACCENT);
            lv_obj_set_width(tm, LV_SIZE_CONTENT);
            lv_obj_set_style_text_font(tm, nl_ov_ui_font_compact(), 0);

            char line[72];
            if (it->missed)
                snprintf(line, sizeof(line), "%s · 已过点", it->title);
            else
                snprintf(line, sizeof(line), "%s", it->title);
            lv_obj_t* tx = daily_label(row, line, UI_TEXT);
            lv_obj_set_flex_grow(tx, 1);
            lv_obj_set_width(tx, lv_pct(55));

            /* 完成：这一次；删除：整条取消，重复的也不再响。 */
            daily_icon_text_btn(row, "完成", 200000 + it->id, UI_TEXT_MUTED);
            daily_icon_text_btn(row, "删除", 300000 + it->id, UI_DANGER);
        }
    }

    lv_obj_t* hist_title = daily_label(body, "历史上的今天", UI_TEXT_DIM);
    lv_obj_set_style_text_font(hist_title, nl_ov_ui_font_compact(), 0);
    lv_obj_t* hist = daily_box(body, LV_FLEX_FLOW_COLUMN, true);
    lv_obj_set_style_pad_row(hist, 8, 0);
    nl_hist_item_t items[6];
    const int n = nl_history_on_day(m, d, items, 6);
    if (n <= 0) {
        lv_obj_t* empty = daily_label(hist, "这一天还没有收录历史见闻", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(empty, nl_ov_ui_font_compact(), 0);
    } else {
        for (int i = 0; i < n; ++i) {
            lv_obj_t* row = daily_box(hist, LV_FLEX_FLOW_ROW, false);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 10, 0);
            char year[8];
            snprintf(year, sizeof(year), "%u", items[i].year);
            lv_obj_t* yl = daily_label(row, year, UI_ACCENT);
            lv_obj_set_width(yl, LV_SIZE_CONTENT);
            lv_obj_t* tx = daily_label(row, items[i].text, UI_TEXT);
            lv_obj_set_flex_grow(tx, 1);
            lv_obj_set_width(tx, lv_pct(75));
        }
    }

    lv_obj_t* note = daily_label(body, "宜忌按值日归纳，供日常参考", UI_TEXT_MUTED);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(note, nl_ov_ui_font_compact(), 0);
}

void fill_calendar_weather(void) {
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
    const char* sub = "15 天天气 · 右滑看月历";
    if (s_daily_focus && !s_daily_weather)
        sub = "黄历与历史上的今天";
    else if (!s_daily_weather)
        sub = "点日期看黄历 · 左滑看天气";
    lv_label_set_text(s_card_sub, sub);
    lv_obj_t* body = daily_box(s_card_list, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(body, daily_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* 无标签：两页靠左右滑动切换，页脚用圆点与提示说明手势。 */
    const bool detail = s_daily_focus && !s_daily_weather;
    if (detail)
        daily_day(body);
    else if (s_daily_weather)
        daily_weather(body);
    else
        daily_calendar(body);
    if (!detail)
        daily_pager_footer(body);
    lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);
    if (detail)
        daily_reminders_fetch_day(s_daily_focus);
    else if (!s_daily_weather)
        daily_reminders_fetch_month();
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
    s_daily_weather = true;
    s_daily_focus = 0;
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
