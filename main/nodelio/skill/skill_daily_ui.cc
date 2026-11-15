#include "skill_ui_priv.h"
#include "almanac.h"
#include <esp_attr.h>

/* Built-in daily view. No polling task or calendar dependency. */
static uint32_t s_daily_generation;
static bool s_daily_weather;
static bool s_daily_all_rem; /* 全部提醒列表（按日期分组） */
static int s_daily_year;
static int s_daily_month;
static int s_daily_date_key;
static int s_daily_focus; /* 年月日，0 表示停在月历 */

/** 当日天气 RAM 缓存：打开时先上屏，再后台刷新。 */
static bool s_wx_cache_ok;
static int s_wx_cache_day; /* YYYYMMDD */
EXT_RAM_BSS_ATTR static nl_env_daily_t s_wx_cache_daily;
EXT_RAM_BSS_ATTR static char s_wx_cache_spoken[NL_ENV_SPOKEN];

/** 月历圆点：当前查看年月的有提醒日期（bit d = 日 d）。 */
static uint32_t s_rem_day_bits;
static int s_rem_bits_year;
static int s_rem_bits_month;
static bool s_rem_fetching;
static uint32_t s_rem_fetch_gen; /* invalidate 递增；过期响应丢弃并补拉 */

enum { kDailyRemMax = 12 };
/** 0=普通 1=30 分钟内需要办 2=已过点 */
enum { kRemToneNormal = 0, kRemToneDueSoon = 1, kRemToneMissed = 2 };
struct daily_rem_item {
    int id;
    char title[48];
    char time[8];
    char recur[12];
    uint8_t tone;
};
EXT_RAM_BSS_ATTR static daily_rem_item s_rem_day_items[kDailyRemMax];
static int s_rem_day_n;
static int s_rem_day_key; /* 已加载的 focus 日 YYYYMMDD */

enum { kDailyAllRemMax = 40 };
struct daily_all_rem_item {
    int id;
    int ymd; /* 分组键 YYYYMMDD，来自 nextFireAt / 今天 */
    char title[48];
    char time[8];
    char recur[12];
    uint8_t tone;
};
EXT_RAM_BSS_ATTR static daily_all_rem_item s_rem_all_items[kDailyAllRemMax];
static int s_rem_all_n;
static bool s_rem_all_ok;
static bool s_rem_all_fetching;

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
static void daily_reminders_fetch_all(void);

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
                        s_rem_all_ok = false;
                        s_rem_bits_year = 0;
                        if (s_daily_all_rem)
                            daily_reminders_fetch_all();
                        else {
                            daily_reminders_fetch_month();
                            if (s_daily_focus)
                                daily_reminders_fetch_day(s_daily_focus);
                        }
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
    if (action == 50) {
        /* 月历 → 全部提醒 */
        s_daily_all_rem = true;
        s_daily_focus = 0;
        s_daily_weather = false;
        s_rem_all_ok = false;
    } else if (action == 40) {
        if (s_daily_all_rem) {
            s_daily_all_rem = false;
        } else {
            s_daily_focus = 0;
        }
    } else if (action >= 1 && action <= 31) {
        if (s_daily_year) {
            s_daily_all_rem = false;
            s_daily_focus = s_daily_year * 10000 + s_daily_month * 100 + action;
        }
    } else if (action == 10) {
        s_daily_all_rem = false;
        s_daily_focus = 0;
        daily_reset_month();
    } else if (s_daily_year && (action == 11 || action == 12)) {
        s_daily_all_rem = false;
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
    if (s_daily_all_rem)
        daily_reminders_fetch_all();
    else if (s_daily_focus)
        daily_reminders_fetch_day(s_daily_focus);
    else if (!s_daily_weather)
        daily_reminders_fetch_month();
}

/* 两页之间左右滑动切换。日历在左、天气在右：向左滑进天气，向右滑回日历。
 * 点开某一天 / 全部提醒时先看详情，滑动留给正文。 */
static void daily_gesture(lv_event_t*) {
    if (s_daily_focus || s_daily_all_rem)
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

/** 胶囊操作按钮：带底色与轻圆角，触控明确，用于完成/删除等次级操作。 */
static lv_obj_t* daily_action_btn(lv_obj_t* parent, const char* text, int action,
                                  lv_color_t fg, lv_color_t bg) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_pad_left(btn, 10, 0);
    lv_obj_set_style_pad_right(btn, 10, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, daily_action, LV_EVENT_CLICKED, (void*)(intptr_t)action);
    lv_obj_t* label = lv_label_create(btn);
    nl_ov_set_font(label);
    lv_obj_set_style_text_font(label, nl_ov_ui_font_compact(), 0);
    lv_label_set_text(label, text);
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
    if (!s_daily_year)
        return;
    if (s_rem_fetching)
        return; /* 在飞请求结束时若 gen 已变会自动补拉 */
    if (s_rem_bits_year == s_daily_year && s_rem_bits_month == s_daily_month)
        return;
    nodelio_nvs_config_t cfg{};
    nl_ov_cfg_for_http(&cfg);
    if (!cfg.server_url[0] || !cfg.device_id[0] || !cfg.device_key[0])
        return;
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip())
        return;
    s_rem_fetching = true;
    const uint32_t gen = s_rem_fetch_gen;
    struct job_t {
        int year;
        int month;
        uint32_t gen;
        nodelio_nvs_config_t cfg;
    };
    auto* job = static_cast<job_t*>(malloc(sizeof(job_t)));
    if (!job) {
        s_rem_fetching = false;
        return;
    }
    job->year = s_daily_year;
    job->month = s_daily_month;
    job->gen = gen;
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
                const bool ok = (err == ESP_OK && status == 200);
                if (ok) {
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
                const uint32_t gen = j->gen;
                free(j);
                Application::GetInstance().Schedule([bits, ok, y, m, gen]() {
                    s_rem_fetching = false;
                    if (gen != s_rem_fetch_gen) {
                        /* 期间已 invalidate：丢弃旧结果并补拉 */
                        if (daily_is_open() && !s_daily_weather && !s_daily_focus)
                            daily_reminders_fetch_month();
                        return;
                    }
                    if (ok) {
                        s_rem_day_bits = bits;
                        s_rem_bits_year = y;
                        s_rem_bits_month = m;
                        if (daily_is_open() && !s_daily_weather && !s_daily_focus &&
                            s_daily_year == y && s_daily_month == m)
                            daily_redraw();
                    }
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
                            const cJSON* due_soon =
                                cJSON_GetObjectItemCaseSensitive(it, "dueSoon");
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
                            if (cJSON_IsTrue(missed))
                                items[n].tone = kRemToneMissed;
                            else if (cJSON_IsTrue(due_soon))
                                items[n].tone = kRemToneDueSoon;
                            else
                                items[n].tone = kRemToneNormal;
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

static void daily_reminders_fetch_all(void) {
    if (s_rem_all_ok || s_rem_all_fetching)
        return;
    nodelio_nvs_config_t cfg{};
    nl_ov_cfg_for_http(&cfg);
    if (!cfg.server_url[0] || !cfg.device_id[0] || !cfg.device_key[0])
        return;
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip())
        return;
    auto* cfgp = static_cast<nodelio_nvs_config_t*>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp)
        return;
    *cfgp = cfg;
    s_rem_all_fetching = true;
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto* c = static_cast<nodelio_nvs_config_t*>(p);
                char base[160];
                nl_url_strip_base(c->server_url, base, sizeof(base));
                char url[288];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/reminders", base, c->device_id);
                char buf[6144];
                buf[0] = '\0';
                int status = 0;
                (void)nodelio_http_get_ms(url, c->device_key, nullptr, &status, buf, sizeof(buf) - 1,
                                          6000);
                free(c);
                daily_all_rem_item items[kDailyAllRemMax];
                memset(items, 0, sizeof(items));
                int n = 0;
                struct tm today{};
                int today_ymd = 0;
                if (daily_today(&today))
                    today_ymd = (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 +
                                today.tm_mday;
                if (status == 200) {
                    cJSON* root = cJSON_Parse(buf);
                    const cJSON* arr =
                        root ? cJSON_GetObjectItemCaseSensitive(root, "items") : nullptr;
                    if (cJSON_IsArray(arr)) {
                        const cJSON* it = nullptr;
                        cJSON_ArrayForEach(it, arr) {
                            if (n >= kDailyAllRemMax)
                                break;
                            const cJSON* id = cJSON_GetObjectItemCaseSensitive(it, "id");
                            const cJSON* title = cJSON_GetObjectItemCaseSensitive(it, "title");
                            const cJSON* tm = cJSON_GetObjectItemCaseSensitive(it, "time");
                            const cJSON* recur = cJSON_GetObjectItemCaseSensitive(it, "recur");
                            const cJSON* missed = cJSON_GetObjectItemCaseSensitive(it, "missed");
                            const cJSON* due_soon =
                                cJSON_GetObjectItemCaseSensitive(it, "dueSoon");
                            const cJSON* next = cJSON_GetObjectItemCaseSensitive(it, "nextFireAt");
                            const cJSON* st = cJSON_GetObjectItemCaseSensitive(it, "status");
                            if (cJSON_IsString(st) && st->valuestring &&
                                strcmp(st->valuestring, "active") != 0)
                                continue;
                            if (!cJSON_IsNumber(id) || !cJSON_IsString(title) || !title->valuestring)
                                continue;
                            int ymd = today_ymd;
                            if (cJSON_IsString(next) && next->valuestring &&
                                strlen(next->valuestring) >= 10) {
                                int yy = 0, mm = 0, dd = 0;
                                if (sscanf(next->valuestring, "%d-%d-%d", &yy, &mm, &dd) == 3 &&
                                    yy >= 1970 && mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31)
                                    ymd = yy * 10000 + mm * 100 + dd;
                            }
                            items[n].id = (int)id->valuedouble;
                            items[n].ymd = ymd;
                            snprintf(items[n].title, sizeof(items[n].title), "%s",
                                     title->valuestring);
                            if (cJSON_IsString(tm) && tm->valuestring)
                                snprintf(items[n].time, sizeof(items[n].time), "%s",
                                         tm->valuestring);
                            if (cJSON_IsString(recur) && recur->valuestring)
                                snprintf(items[n].recur, sizeof(items[n].recur), "%s",
                                         recur->valuestring);
                            if (cJSON_IsTrue(missed))
                                items[n].tone = kRemToneMissed;
                            else if (cJSON_IsTrue(due_soon))
                                items[n].tone = kRemToneDueSoon;
                            else
                                items[n].tone = kRemToneNormal;
                            n++;
                        }
                    }
                    if (root)
                        cJSON_Delete(root);
                }
                /* 按日期再按时间排序，便于分组渲染 */
                for (int i = 0; i + 1 < n; ++i) {
                    for (int j = i + 1; j < n; ++j) {
                        const bool swap = items[j].ymd < items[i].ymd ||
                                          (items[j].ymd == items[i].ymd &&
                                           strcmp(items[j].time, items[i].time) < 0);
                        if (swap) {
                            daily_all_rem_item tmp = items[i];
                            items[i] = items[j];
                            items[j] = tmp;
                        }
                    }
                }
                auto* pack = static_cast<daily_all_rem_item*>(
                    malloc(sizeof(daily_all_rem_item) * (size_t)kDailyAllRemMax + sizeof(int)));
                if (!pack) {
                    Application::GetInstance().Schedule([]() { s_rem_all_fetching = false; });
                    return;
                }
                memcpy(pack, items, sizeof(daily_all_rem_item) * (size_t)n);
                *reinterpret_cast<int*>(pack + kDailyAllRemMax) = n;
                Application::GetInstance().Schedule([pack]() {
                    const int n = *reinterpret_cast<const int*>(pack + kDailyAllRemMax);
                    memcpy(s_rem_all_items, pack, sizeof(daily_all_rem_item) * (size_t)n);
                    s_rem_all_n = n;
                    s_rem_all_ok = true;
                    s_rem_all_fetching = false;
                    free(pack);
                    if (daily_is_open() && s_daily_all_rem)
                        daily_redraw();
                });
            },
            cfgp, [](void* p) { free(p); })) {
        free(cfgp);
        s_rem_all_fetching = false;
    }
}

void nodelio_skill_daily_reminders_invalidate(void) {
    s_rem_bits_year = 0;
    s_rem_bits_month = 0;
    s_rem_day_key = 0;
    s_rem_all_ok = false;
    s_rem_day_bits = 0;
    ++s_rem_fetch_gen; /* 作废在飞月历圆点响应 */
    Application::GetInstance().Schedule([]() {
        if (!daily_is_open())
            return;
        if (s_daily_all_rem)
            daily_reminders_fetch_all();
        else if (s_daily_focus)
            daily_reminders_fetch_day(s_daily_focus);
        else if (!s_daily_weather)
            daily_reminders_fetch_month();
    });
}

/* ---- 日历 -------------------------------------------------------------- */

/** 日期行之间的留白（相邻两行「15 / 22」之间）。 */
static const int kCalRowGap = 10;
/** 公历与农历行距（负值：农历上移靠拢）。 */
static const int kCalNumSubLineSpace = -3;
/** 当日绿底内边距；角标走左右 gutter，不靠 FLOATING/translate。 */
static const int kCalTodayPadY = 10;
static const int kCalTodayPadX = 10;
static const int kCalRemBadge = 8;
/** 无底部提示后的固定开销（月份条+表头+页脚圆点）。 */
static const int kCalFixed = 84;

/** 日期格高按结果卡可用高度倒推；须能装下今日绿底 pad + 两行字。 */
static int daily_cell_h(int rows) {
    const auto* p = nodelio_display_profile();
    const int card_h = p ? (int)p->result_max_height + NL_UI_STATUS_H : 460;
    if (rows < 1)
        rows = 1;
    /* 今日：padY*2 + 公历≈22 + 农历≈16 ≈ 62；格高下限高于此。 */
    const int want = kCalTodayPadY * 2 + 40;
    for (int cell = 84; cell >= want; --cell) {
        if (kCalFixed + rows * (cell + kCalRowGap) <= card_h - 6)
            return cell;
    }
    return want;
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
    daily_icon_text_btn(monthbar, "提醒", 50, UI_TEXT_SUB);

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

    /* 不再画底部「点日期…」提示，高度全给行距与格子。 */
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
        const bool has_rem = valid && daily_rem_has(day);
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

        nl_almanac_t info{};
        const bool have_sub =
            nl_almanac_build(s_daily_year, s_daily_month, day, &info) && info.label[0];

        /*
         * 绿底 COLUMN + 中层 ROW：左 gutter | 文字 | 右 gutter（角标置顶）。
         * 左右等宽保证「15」光学居中；角标全程在 flex 流里，不 FLOATING/translate。
         */
        const int pad_x = current ? kCalTodayPadX : (has_rem ? 6 : 0);
        const int pad_y = current ? kCalTodayPadY : (has_rem ? 4 : 0);
        const int gutter = has_rem ? kCalRemBadge : 0;

        lv_obj_t* pill = lv_obj_create(cell);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(pill, 0, 0);
        lv_obj_set_style_pad_hor(pill, pad_x, 0);
        lv_obj_set_style_pad_ver(pill, pad_y, 0);
        lv_obj_clear_flag(pill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_set_style_clip_corner(pill, false, 0);
        if (current) {
            lv_obj_set_style_bg_color(pill, UI_SELECTION, 0);
            lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(pill, UI_RADIUS_SM, 0);
        }
        lv_obj_center(pill);

        lv_obj_t* mid = lv_obj_create(pill);
        lv_obj_remove_style_all(mid);
        lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(mid, 0, 0);
        lv_obj_clear_flag(mid, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        if (gutter > 0) {
            lv_obj_t* left = lv_obj_create(mid);
            lv_obj_remove_style_all(left);
            lv_obj_set_size(left, gutter, 1);
            lv_obj_clear_flag(left, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        }

        lv_obj_t* text_col = lv_obj_create(mid);
        lv_obj_remove_style_all(text_col);
        lv_obj_set_size(text_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(text_col, 0, 0);
        lv_obj_clear_flag(text_col, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

        char number[12];
        snprintf(number, sizeof(number), "%d", day);
        lv_obj_t* num = lv_label_create(text_col);
        nl_ov_set_font(num);
        lv_label_set_text(num, number);
        lv_obj_set_width(num, LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(num, 0, 0);
        lv_obj_set_style_text_color(num, current ? UI_ACCENT : UI_TEXT, 0);

        if (have_sub) {
            lv_color_t sub_c = UI_TEXT_MUTED;
            if (info.label_kind == NL_ALMANAC_FESTIVAL)
                sub_c = UI_DANGER;
            else if (info.label_kind == NL_ALMANAC_TERM)
                sub_c = UI_WARN;
            else if (current)
                sub_c = UI_ACCENT;
            lv_obj_t* sub = lv_label_create(text_col);
            nl_ov_set_font(sub);
            lv_obj_set_style_text_font(sub, nl_ov_ui_font_compact(), 0);
            lv_label_set_text(sub, info.label);
            lv_obj_set_width(sub, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(sub, 0, 0);
            lv_obj_set_style_translate_y(sub, kCalNumSubLineSpace, 0);
            lv_obj_set_style_text_color(sub, sub_c, 0);
        }

        if (gutter > 0) {
            lv_obj_t* right = lv_obj_create(mid);
            lv_obj_remove_style_all(right);
            lv_obj_set_size(right, gutter, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END,
                                  LV_FLEX_ALIGN_START);
            lv_obj_clear_flag(right, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

            lv_obj_t* badge = lv_obj_create(right);
            lv_obj_remove_style_all(badge);
            lv_obj_set_size(badge, kCalRemBadge, kCalRemBadge);
            lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(badge, UI_DANGER, 0);
            lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
            lv_obj_clear_flag(badge,
                              (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
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

/** 曜夜下卡片无阴影，补一条淡描边，两栏才分得清。 */
static void daily_card_edge(lv_obj_t* card) {
    if (!card || !ui_palette_is_dark())
        return;
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UI_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_40, 0);
}

/** 宜/忌块：标签 + 事项竖排（compact），可放进双列以免左卡空洞。 */
static void daily_luck_block(lv_obj_t* parent, const char* tag, lv_color_t tag_c, const char* text) {
    lv_obj_t* block = daily_box(parent, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_set_style_pad_row(block, 3, 0);
    lv_obj_set_style_min_width(block, 0, 0);
    lv_obj_set_flex_grow(block, 1);
    lv_obj_set_flex_align(block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* badge = lv_obj_create(block);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 2, 0);
    lv_obj_set_style_radius(badge, UI_RADIUS_XS, 0);
    lv_obj_set_style_bg_color(badge, tag_c, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_clear_flag(badge, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t* mark = lv_label_create(badge);
    nl_ov_set_font(mark);
    lv_obj_set_style_text_font(mark, nl_ov_ui_font_compact(), 0);
    lv_label_set_text(mark, tag);
    lv_obj_set_style_text_color(mark, tag_c, 0);
    lv_obj_center(mark);

    if (!text || !text[0]) {
        lv_obj_t* empty = daily_label(block, "—", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(empty, nl_ov_ui_font_compact(), 0);
        return;
    }

    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p)
            break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        char item[24];
        size_t n = (size_t)(p - start);
        if (n >= sizeof(item))
            n = sizeof(item) - 1;
        memcpy(item, start, n);
        item[n] = '\0';
        lv_obj_t* lab = daily_label(block, item, UI_TEXT);
        lv_obj_set_width(lab, lv_pct(100));
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_LEFT, 0);
    }
}

/** 宽结果区（P4 侧栏卡等）日详情左右分栏，避免竖条空档。 */
static bool daily_day_wide(void) {
    const auto* p = nodelio_display_profile();
    return p && p->result_width >= 560;
}

/**
 * 提醒磁贴三态：普通白底 / 30 分钟内琥珀「需要办」/ 过点淡红「已过点」。
 * 宽屏不再用 flex_grow 把完成/删除甩到屏幕最右侧。
 */
static void daily_rem_tile(lv_obj_t* parent, int id, const char* when, const char* title, uint8_t tone,
                           const char* extra, bool compact_width) {
    const bool due_soon = tone == kRemToneDueSoon;
    const bool missed = tone == kRemToneMissed;
    lv_obj_t* tile = lv_obj_create(parent);
    lv_obj_remove_style_all(tile);
    if (compact_width)
        lv_obj_set_size(tile, 420, LV_SIZE_CONTENT);
    else
        lv_obj_set_size(tile, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tile, 12, 0);
    lv_obj_set_style_pad_row(tile, 8, 0);
    lv_obj_set_style_radius(tile, UI_RADIUS_SM, 0);
    /* 黄底用暖琥珀磁贴色（tile_gpio），红底用 danger_bg，普通白卡 */
    lv_obj_set_style_bg_color(tile, missed ? UI_DANGER_BG : (due_soon ? UI_TILE_GPIO : UI_SURFACE),
                              0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, missed ? UI_DANGER : (due_soon ? UI_WARN : UI_BORDER), 0);
    lv_obj_set_style_border_opa(tile, missed ? LV_OPA_30 : (due_soon ? LV_OPA_50 : LV_OPA_40), 0);
    if (!ui_palette_is_dark() && !missed && !due_soon)
        ui_style_card(tile);
    lv_obj_clear_flag(tile, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t* meta = daily_box(tile, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(meta, 6, 0);

    const lv_color_t time_fg = missed ? UI_DANGER_FG : (due_soon ? UI_WARN : UI_ACCENT);
    const lv_color_t time_bg = missed ? UI_DANGER : (due_soon ? UI_WARN : UI_ACCENT);
    lv_obj_t* time_badge = lv_obj_create(meta);
    lv_obj_remove_style_all(time_badge);
    lv_obj_set_size(time_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(time_badge, 8, 0);
    lv_obj_set_style_pad_ver(time_badge, 3, 0);
    lv_obj_set_style_radius(time_badge, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(time_badge, time_bg, 0);
    lv_obj_set_style_bg_opa(time_badge, LV_OPA_20, 0);
    lv_obj_clear_flag(time_badge, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t* tm = lv_label_create(time_badge);
    nl_ov_set_font(tm);
    lv_obj_set_style_text_font(tm, nl_ov_ui_font_compact(), 0);
    lv_label_set_text(tm, when && when[0] ? when : "—");
    lv_obj_set_style_text_color(tm, time_fg, 0);
    lv_obj_center(tm);

    if (missed)
        daily_chip(meta, "已过点", UI_DANGER_FG);
    else if (due_soon)
        daily_chip(meta, "需要办", UI_WARN);
    if (extra && extra[0])
        daily_chip(meta, extra, UI_TEXT_MUTED);

    lv_obj_t* act = daily_box(tile, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 8, 0);

    lv_obj_t* tx = daily_label(act, title && title[0] ? title : "提醒", UI_TEXT);
    lv_obj_set_flex_grow(tx, 1);
    lv_obj_set_style_min_width(tx, 0, 0);

    daily_action_btn(act, "完成", 200000 + id, lv_color_white(), UI_ACCENT);
    daily_action_btn(act, "删除", 300000 + id, UI_DANGER_FG, UI_DANGER_BG);
}

static void daily_day_fill_reminders(lv_obj_t* rem) {
    if (s_rem_day_key != s_daily_focus) {
        lv_obj_t* loading = daily_label(rem, "正在同步…", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(loading, nl_ov_ui_font_compact(), 0);
        return;
    }
    if (s_rem_day_n <= 0) {
        lv_obj_t* empty = daily_label(rem, "今日暂无提醒", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(empty, nl_ov_ui_font_compact(), 0);
        return;
    }
    for (int i = 0; i < s_rem_day_n; ++i) {
        const daily_rem_item* it = &s_rem_day_items[i];
        char when[24];
        if (it->time[0])
            snprintf(when, sizeof(when), "%s", it->time);
        else if (strcmp(it->recur, "daily") == 0)
            snprintf(when, sizeof(when), "每天");
        else if (strcmp(it->recur, "weekly") == 0)
            snprintf(when, sizeof(when), "每周");
        else
            snprintf(when, sizeof(when), "一次");
        daily_rem_tile(rem, it->id, when, it->title, it->tone, nullptr, false);
    }
}

/** 点开某一天：宽屏双卡（左黄历竖排 | 右待办），方屏上下叠。
 *  勿对宽屏列设 width=0：父级 SIZE_CONTENT 时会把双卡高度挤没。 */
static void daily_day(lv_obj_t* body) {
    const int y = s_daily_focus / 10000;
    const int m = (s_daily_focus / 100) % 100;
    const int d = s_daily_focus % 100;
    const bool wide = daily_day_wide();
    lv_obj_set_style_pad_row(body, 10, 0);

    nl_almanac_t info{};
    const bool info_ok = nl_almanac_build(y, m, d, &info);

    struct tm when{};
    when.tm_year = y - 1900;
    when.tm_mon = m - 1;
    when.tm_mday = d;
    when.tm_hour = 12;
    when.tm_isdst = -1;
    mktime(&when);
    static const char* week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

    /* 顶栏：返回 + 日期 + 黄道胶囊（农历改入黄历卡内） */
    lv_obj_t* head = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_style_pad_row(head, 0, 0);
    lv_obj_set_style_pad_right(head, 44, 0);
    daily_icon_btn(head, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, 40, UI_TEXT_SUB);

    lv_obj_t* top_line = daily_box(head, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_grow(top_line, 1);
    lv_obj_set_flex_align(top_line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top_line, 6, 0);
    lv_obj_set_style_pad_row(top_line, 0, 0);
    lv_obj_set_style_min_width(top_line, 0, 0);

    char title[48];
    snprintf(title, sizeof(title), "%d月%d日 %s", m, d, week[when.tm_wday]);
    lv_obj_t* title_l = daily_label(top_line, title, UI_TEXT, true);
    lv_obj_set_width(title_l, LV_SIZE_CONTENT);

    if (info_ok) {
        char luck_tag[32];
        snprintf(luck_tag, sizeof(luck_tag), "%s · %s", info.huangdao ? "黄道吉日" : "黑道日",
                 info.tian_shen[0] ? info.tian_shen : "吉神");
        daily_chip(top_line, luck_tag, info.huangdao ? UI_SUCCESS : UI_TEXT_MUTED);
    }

    if (!info_ok) {
        daily_label(body, "这一天暂不能换算黄历", UI_TEXT_MUTED);
        return;
    }

    if (info.festivals[0] || info.term[0]) {
        lv_obj_t* chips = daily_box(body, LV_FLEX_FLOW_ROW_WRAP, false);
        lv_obj_set_style_pad_column(chips, 6, 0);
        lv_obj_set_style_pad_row(chips, 4, 0);
        if (info.festivals[0])
            daily_chip(chips, info.festivals, UI_DANGER);
        if (info.term[0] && !strstr(info.festivals, info.term))
            daily_chip(chips, info.term, UI_WARN);
    }

    /* 宽屏双卡并排；方屏上下叠。列宽用 flex，禁止 width=0。 */
    lv_obj_t* cols = nullptr;
    if (wide) {
        cols = daily_box(body, LV_FLEX_FLOW_ROW, false);
        lv_obj_set_style_pad_column(cols, 12, 0);
        lv_obj_set_style_pad_row(cols, 0, 0);
        /* 整块内容略离左边，不要贴死卡缘 */
        lv_obj_set_style_pad_left(cols, 10, 0);
        lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }
    lv_obj_t* card_host = cols ? cols : body;

    /* 左卡：黄历。宽屏宜|忌双列填宽，事项仍各自竖排，避免单列空洞。 */
    lv_obj_t* luck = daily_box(card_host, LV_FLEX_FLOW_COLUMN, true);
    daily_card_edge(luck);
    if (wide) {
        lv_obj_set_flex_grow(luck, 1);
        lv_obj_set_style_min_width(luck, 0, 0);
        lv_obj_set_width(luck, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_style_pad_left(luck, 10, 0);
    }
    lv_obj_set_style_pad_all(luck, 16, 0);
    lv_obj_set_style_pad_left(luck, 20, 0);
    lv_obj_set_style_pad_row(luck, 10, 0);
    lv_obj_set_flex_align(luck, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    daily_label(luck, "黄历", UI_TEXT_SUB);
    if (info.lunar_line[0]) {
        lv_obj_t* lunar = daily_label(luck, info.lunar_line, UI_TEXT);
        lv_obj_set_width(lunar, lv_pct(100));
    }

    lv_obj_t* meta = daily_box(luck, LV_FLEX_FLOW_ROW_WRAP, false);
    lv_obj_set_style_pad_column(meta, 8, 0);
    lv_obj_set_style_pad_row(meta, 4, 0);
    char duty[40];
    snprintf(duty, sizeof(duty), "值日 · %s", info.zhi_xing[0] ? info.zhi_xing : "平");
    daily_chip(meta, duty, UI_TEXT_SUB);
    char cs[48];
    snprintf(cs, sizeof(cs), "冲煞 · %s", info.chong_sha[0] ? info.chong_sha : "无");
    daily_chip(meta, cs, UI_TEXT_MUTED);

    lv_obj_t* yj = daily_box(luck, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN, false);
    lv_obj_set_style_pad_column(yj, 16, 0);
    lv_obj_set_style_pad_row(yj, 8, 0);
    lv_obj_set_style_min_width(yj, 0, 0);
    daily_luck_block(yj, "宜", UI_SUCCESS, info.yi);
    daily_luck_block(yj, "忌", UI_DANGER, info.ji);

    if (info.pengzu[0]) {
        lv_obj_t* peng = lv_label_create(luck);
        nl_ov_set_font(peng);
        lv_label_set_long_mode(peng, LV_LABEL_LONG_WRAP);
        lv_label_set_text(peng, info.pengzu);
        lv_obj_set_style_text_color(peng, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(peng, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(peng, lv_pct(100));
    }

    /* 与上方彭祖/宜忌拉开，避免贴在一起 */
    lv_obj_t* hist_sec = daily_box(luck, LV_FLEX_FLOW_COLUMN, false);
    lv_obj_set_style_pad_top(hist_sec, 8, 0);
    lv_obj_set_style_pad_row(hist_sec, 6, 0);
    daily_label(hist_sec, "历史上的今天", UI_TEXT_SUB);
    nl_hist_item_t items[6];
    const int n = nl_history_on_day(m, d, items, 6);
    if (n <= 0) {
        daily_label(hist_sec, "暂无收录", UI_TEXT_MUTED);
    } else {
        const int show = wide ? (n > 3 ? 3 : n) : n;
        for (int i = 0; i < show; ++i) {
            lv_obj_t* row = daily_box(hist_sec, LV_FLEX_FLOW_ROW, false);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_set_style_pad_row(row, 0, 0);

            char year[16];
            snprintf(year, sizeof(year), "%u年", items[i].year);
            daily_chip(row, year, UI_ACCENT);

            lv_obj_t* tx = lv_label_create(row);
            nl_ov_set_font(tx);
            lv_label_set_long_mode(tx, LV_LABEL_LONG_DOT);
            lv_label_set_text(tx, items[i].text);
            lv_obj_set_style_text_color(tx, UI_TEXT, 0);
            lv_obj_set_flex_grow(tx, 1);
            lv_obj_set_style_min_width(tx, 0, 0);
        }
    }

    /* 右卡：待办，条目用磁贴，操作跟标题在一起 */
    lv_obj_t* rem = daily_box(card_host, LV_FLEX_FLOW_COLUMN, true);
    daily_card_edge(rem);
    if (wide) {
        lv_obj_set_width(rem, lv_pct(42));
        lv_obj_set_style_min_width(rem, 0, 0);
    }
    lv_obj_set_style_pad_all(rem, 14, 0);
    lv_obj_set_style_pad_row(rem, 10, 0);
    char rem_title_str[32];
    if (s_rem_day_key == s_daily_focus && s_rem_day_n > 0)
        snprintf(rem_title_str, sizeof(rem_title_str), "待办提醒 · %d", s_rem_day_n);
    else
        snprintf(rem_title_str, sizeof(rem_title_str), "待办提醒");
    daily_label(rem, rem_title_str, UI_TEXT_SUB);
    daily_day_fill_reminders(rem);
}

/** 全部提醒：按日分组；宽屏磁贴换行，避免一行拉满整屏。 */
static void daily_all_reminders(lv_obj_t* body) {
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_style_pad_left(body, 12, 0);
    lv_obj_set_style_pad_right(body, 12, 0);
    const bool wide = daily_day_wide();

    lv_obj_t* head = daily_box(body, LV_FLEX_FLOW_ROW, false);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_style_pad_row(head, 0, 0);
    lv_obj_set_style_pad_right(head, 44, 0);
    daily_icon_btn(head, MATERIAL_SYMBOLS_KEYBOARD_ARROW_LEFT, 40, UI_TEXT_SUB);
    lv_obj_t* title = daily_label(head, "全部提醒", UI_TEXT, true);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(title, 1);

    if (!s_rem_all_ok) {
        lv_obj_t* tip = daily_label(body, "正在同步…", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(tip, nl_ov_ui_font_compact(), 0);
        return;
    }
    if (s_rem_all_n <= 0) {
        lv_obj_t* empty = daily_label(body, "还没有提醒", UI_TEXT_MUTED);
        lv_obj_set_style_text_font(empty, nl_ov_ui_font_compact(), 0);
        return;
    }

    static const char* week[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    int last_ymd = -1;
    lv_obj_t* group = nullptr;
    lv_obj_t* list = nullptr;
    for (int i = 0; i < s_rem_all_n; ++i) {
        const daily_all_rem_item* it = &s_rem_all_items[i];
        if (it->ymd != last_ymd) {
            last_ymd = it->ymd;
            const int y = it->ymd / 10000;
            const int m = (it->ymd / 100) % 100;
            const int d = it->ymd % 100;
            struct tm when{};
            when.tm_year = y - 1900;
            when.tm_mon = m - 1;
            when.tm_mday = d;
            when.tm_hour = 12;
            when.tm_isdst = -1;
            mktime(&when);
            char hdr[48];
            snprintf(hdr, sizeof(hdr), "%d月%d日 %s", m, d, week[when.tm_wday]);
            /* 日期作分组标题：正文级字号，不再套一层灰底卡片 */
            group = daily_box(body, LV_FLEX_FLOW_COLUMN, false);
            lv_obj_set_style_pad_row(group, 8, 0);
            if (i > 0)
                lv_obj_set_style_pad_top(group, 6, 0);
            lv_obj_t* hl = daily_label(group, hdr, UI_TEXT, true);
            lv_obj_set_width(hl, LV_SIZE_CONTENT);
            list = daily_box(group, wide ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN, false);
            lv_obj_set_style_pad_column(list, 8, 0);
            lv_obj_set_style_pad_row(list, 8, 0);
        }
        if (!group || !list)
            continue;

        char when[24];
        if (it->time[0])
            snprintf(when, sizeof(when), "%s", it->time);
        else if (strcmp(it->recur, "daily") == 0)
            snprintf(when, sizeof(when), "每天");
        else if (strcmp(it->recur, "weekly") == 0)
            snprintf(when, sizeof(when), "每周");
        else
            snprintf(when, sizeof(when), "一次");

        const char* extra = nullptr;
        if (strcmp(it->recur, "daily") == 0)
            extra = "每天";
        else if (strcmp(it->recur, "weekly") == 0)
            extra = "每周";
        /* when 已是「每天/每周」时勿重复挂 extra */
        if (extra && strcmp(when, extra) == 0)
            extra = nullptr;

        daily_rem_tile(list, it->id, when, it->title, it->tone, extra, wide);
    }
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
    if (s_daily_all_rem)
        sub = "按下次提醒时间分组";
    else if (s_daily_focus && !s_daily_weather)
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
    const bool all_rem = s_daily_all_rem && !s_daily_weather;
    if (all_rem)
        daily_all_reminders(body);
    else if (detail)
        daily_day(body);
    else if (s_daily_weather)
        daily_weather(body);
    else
        daily_calendar(body);
    if (!detail && !all_rem)
        daily_pager_footer(body);
    lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);
    if (all_rem)
        daily_reminders_fetch_all();
    else if (detail)
        daily_reminders_fetch_day(s_daily_focus);
    else if (!s_daily_weather)
        daily_reminders_fetch_month();
}

static uint32_t daily_bump_generation(void) {
    ++s_daily_generation;
    if (!s_daily_generation)
        ++s_daily_generation;
    return s_daily_generation;
}

static int daily_today_key(void) {
    struct tm today{};
    if (!daily_today(&today)) {
        return 0;
    }
    return (today.tm_year + 1900) * 10000 + (today.tm_mon + 1) * 100 + today.tm_mday;
}

static bool daily_temp_ok(const nl_env_daily_t* d) {
    return d && d->temperature[0] && strcmp(d->temperature, "—") != 0 &&
           strcmp(d->temperature, "--") != 0 && strcmp(d->temperature, "--°") != 0;
}

static bool daily_wx_cache_ready(void) {
    const int day = daily_today_key();
    return s_wx_cache_ok && day && s_wx_cache_day == day && daily_temp_ok(&s_wx_cache_daily);
}

bool nodelio_skill_daily_has_wx_cache(void) {
    return daily_wx_cache_ready();
}

static void daily_cache_store(const nl_content_envelope_t* env) {
    if (!env || !daily_temp_ok(&env->daily)) {
        return;
    }
    const int day = daily_today_key();
    if (!day) {
        return;
    }
    s_wx_cache_daily = env->daily;
    snprintf(s_wx_cache_spoken, sizeof(s_wx_cache_spoken), "%s", env->spoken);
    s_wx_cache_day = day;
    s_wx_cache_ok = true;
}

static nl_content_envelope_t* daily_shell_env(void) {
    auto* env = static_cast<nl_content_envelope_t*>(
        heap_caps_calloc(1, sizeof(nl_content_envelope_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!env) {
        nl_ov_toast("内存不足");
        return nullptr;
    }
    snprintf(env->status, sizeof(env->status), "ok");
    snprintf(env->app_id, sizeof(env->app_id), "builtin.daily");
    snprintf(env->card_type, sizeof(env->card_type), "calendar_weather");
    snprintf(env->display_title, sizeof(env->display_title), "日历与天气");
    const int day = daily_today_key();
    if (s_wx_cache_ok && day && s_wx_cache_day == day) {
        env->daily = s_wx_cache_daily;
        if (s_wx_cache_spoken[0]) {
            snprintf(env->spoken, sizeof(env->spoken), "%s", s_wx_cache_spoken);
        }
    } else {
        snprintf(env->daily.message, sizeof(env->daily.message), "尚未获取天气，可联网后点击刷新");
    }
    return env;
}

uint32_t nodelio_skill_daily_open(bool reset_view, bool weather_tab) {
    const uint32_t generation = daily_bump_generation();
    if (!reset_view && daily_is_open()) {
        daily_redraw();
        return generation;
    }
    auto* env = daily_shell_env();
    if (!env)
        return 0;
    s_daily_weather = weather_tab;
    s_daily_all_rem = false;
    s_daily_focus = 0;
    daily_reset_month();
    nodelio_skill_result_show(nullptr, env, false);
    return generation;
}

uint32_t nodelio_skill_daily_open_reminders(void) {
    const uint32_t generation = daily_bump_generation();
    struct tm today{};
    int focus = 0;
    if (daily_today(&today)) {
        s_daily_year = today.tm_year + 1900;
        s_daily_month = today.tm_mon + 1;
        focus = s_daily_year * 10000 + s_daily_month * 100 + today.tm_mday;
    } else {
        daily_reset_month();
    }
    s_daily_weather = false;
    s_daily_all_rem = false;
    s_daily_focus = focus;
    s_rem_day_key = 0;
    if (daily_is_open()) {
        daily_redraw();
        return generation;
    }
    auto* env = daily_shell_env();
    if (!env)
        return 0;
    nodelio_skill_result_show(nullptr, env, false);
    return generation;
}

uint32_t nodelio_skill_daily_open_all_reminders(void) {
    const uint32_t generation = daily_bump_generation();
    s_daily_weather = false;
    s_daily_all_rem = true;
    s_daily_focus = 0;
    s_rem_all_ok = false;
    if (!s_daily_year)
        daily_reset_month();
    if (daily_is_open()) {
        daily_redraw();
        daily_reminders_fetch_all();
        return generation;
    }
    auto* env = daily_shell_env();
    if (!env)
        return 0;
    nodelio_skill_result_show(nullptr, env, false);
    daily_reminders_fetch_all();
    return generation;
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
    /* 新包无有效气温：保留当日缓存画面，最多并入新空气质量，避免「暂不可用」盖掉好数据 */
    if (!daily_temp_ok(&env->daily) && daily_wx_cache_ready()) {
        const bool air_new = env->daily.air[0] != '\0';
        char air[sizeof(env->daily.air)];
        char air_upd[sizeof(env->daily.air_updated)];
        air[0] = '\0';
        air_upd[0] = '\0';
        if (air_new) {
            snprintf(air, sizeof(air), "%s", env->daily.air);
            snprintf(air_upd, sizeof(air_upd), "%s", env->daily.air_updated);
        }
        env->daily = s_wx_cache_daily;
        if (air_new) {
            snprintf(env->daily.air, sizeof(env->daily.air), "%s", air);
            snprintf(env->daily.air_updated, sizeof(env->daily.air_updated), "%s", air_upd);
        }
        if (s_wx_cache_spoken[0]) {
            snprintf(env->spoken, sizeof(env->spoken), "%s", s_wx_cache_spoken);
        }
        env->daily.message[0] = '\0';
    }
    daily_cache_store(env);
    const int scroll_y = s_card_list ? lv_obj_get_scroll_y(s_card_list) : 0;
    nodelio_skill_result_show(cfg, env, false);
    if (s_card_list && scroll_y > 0) {
        lv_obj_update_layout(s_card_list);
        lv_obj_scroll_to_y(s_card_list, scroll_y, LV_ANIM_OFF);
    }
}
