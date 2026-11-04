#include "skill_ui_priv.h"
#include <string>

lv_obj_t *s_dsh_status = nullptr;      /* 续聊/执行中状态条：EQ + 文案 */
lv_obj_t *s_dsh_status_lbl = nullptr;
lv_obj_t *s_dsh_eq[3] = {nullptr};
lv_obj_t *s_dsh_confirm_bar = nullptr; /* 续聊待确认：发送 / 取消 */
lv_obj_t *s_dsh_detail_bar = nullptr;  /* 详情底栏：续聊悬浮，不随正文滚走 */
char s_dsh_pending_preview[96] = {0};
lv_timer_t *s_dsh_detail_poll = nullptr;
char s_dsh_detail_ws[200] = {0};
char s_dsh_detail_sess[80] = {0};
std::atomic<bool> s_dsh_detail_busy{false};
bool s_dsh_eq_anim_on = false;
bool s_dsh_row_pulse_on = false;
lv_obj_t *s_dsh_focus_row = nullptr; /* 续聊焦点行，rebuild 时清空 */
lv_obj_t *s_dsh_pulse_rows[SKILL_LIST_ROWS] = {nullptr}; /* 边框呼吸：焦点 + 执行中 */
int s_dsh_pulse_n = 0;
dsh_ui_phase_t s_dsh_phase = DSH_UI_IDLE;
char s_dsh_focus_sess[80] = {0};
char s_dsh_focus_ws[200] = {0};
char s_dsh_last_instr[96] = {0};
int64_t s_dsh_dispatched_us = 0; /* DISPATCHED 起算，约 25s 内保留乐观态 */

void stop_dsh_detail_poll(void)
{
    if (s_dsh_detail_poll) {
        lv_timer_del(s_dsh_detail_poll);
        s_dsh_detail_poll = nullptr;
    }
}

bool dsh_is_detail_view(void)
{
    return s_have_env && s_env && s_env->detail_body && s_env->detail_body[0] &&
           strstr(s_env->app_id, "dsh") != nullptr && s_card &&
           !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_minimized;
}

/** 从 track 的 arm 动作取出 workspace/session */
void dsh_extract_arm(const nl_env_track_t *tr, char *ws, size_t ws_len, char *sess,
                     size_t sess_len)
{
    if (ws && ws_len) {
        ws[0] = '\0';
    }
    if (sess && sess_len) {
        sess[0] = '\0';
    }
    if (!tr) {
        return;
    }
    for (int ai = 0; ai < tr->invoke_n; ai++) {
        const char *pj = tr->invokes[ai].params_json;
        if (!pj[0] || strstr(pj, "\"arm\"") == nullptr) {
            continue;
        }
        cJSON *root = cJSON_Parse(pj);
        if (!root) {
            continue;
        }
        const cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "workspace");
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "session");
        if (ws && ws_len && cJSON_IsString(jw) && jw->valuestring) {
            snprintf(ws, ws_len, "%s", jw->valuestring);
        }
        if (sess && sess_len && cJSON_IsString(js) && js->valuestring) {
            snprintf(sess, sess_len, "%s", js->valuestring);
        }
        cJSON_Delete(root);
        if (sess && sess[0]) {
            break;
        }
    }
    if (sess && sess_len && !sess[0] && tr->id[0]) {
        snprintf(sess, sess_len, "%s", tr->id);
    }
}

static void remember_dsh_detail_keys(void)
{
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    if (!s_have_env || !s_env || s_env->track_n <= 0) {
        return;
    }
    dsh_extract_arm(&s_env->tracks[0], s_dsh_detail_ws, sizeof(s_dsh_detail_ws), s_dsh_detail_sess,
                    sizeof(s_dsh_detail_sess));
}

bool dsh_track_is_focus(int track_idx)
{
    if (s_dsh_phase == DSH_UI_IDLE || !s_dsh_focus_sess[0] || !s_env) {
        return false;
    }
    if (track_idx < 0 || track_idx >= s_env->track_n) {
        return false;
    }
    char ws[200] = {0};
    char sess[80] = {0};
    dsh_extract_arm(&s_env->tracks[track_idx], ws, sizeof(ws), sess, sizeof(sess));
    return sess[0] && strcmp(sess, s_dsh_focus_sess) == 0;
}

void stop_dsh_row_pulses(void)
{
    if (!s_dsh_row_pulse_on) {
        s_dsh_pulse_n = 0;
        return;
    }
    for (int i = 0; i < s_dsh_pulse_n; i++) {
        if (s_dsh_pulse_rows[i]) {
            lv_anim_delete(s_dsh_pulse_rows[i], anim_border_opa);
        }
        s_dsh_pulse_rows[i] = nullptr;
    }
    s_dsh_pulse_n = 0;
    s_dsh_row_pulse_on = false;
}

void stop_dsh_eq_anims(void)
{
    if (!s_dsh_eq_anim_on) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (s_dsh_eq[i]) {
            lv_anim_delete(s_dsh_eq[i], anim_eq_height);
            lv_obj_set_height(s_dsh_eq[i], 6 + i * 3);
        }
    }
    s_dsh_eq_anim_on = false;
}

void stop_dsh_status_anims(void)
{
    stop_dsh_eq_anims();
    stop_dsh_row_pulses();
}

static void dsh_register_pulse_row(lv_obj_t *row)
{
    if (!row || s_dsh_pulse_n >= SKILL_LIST_ROWS) {
        return;
    }
    for (int i = 0; i < s_dsh_pulse_n; i++) {
        if (s_dsh_pulse_rows[i] == row) {
            return;
        }
    }
    s_dsh_pulse_rows[s_dsh_pulse_n++] = row;
}

void start_dsh_row_pulses(void)
{
    if (s_dsh_pulse_n <= 0) {
        return;
    }
    if (s_dsh_row_pulse_on) {
        for (int i = 0; i < s_dsh_pulse_n; i++) {
            if (s_dsh_pulse_rows[i]) {
                lv_anim_delete(s_dsh_pulse_rows[i], anim_border_opa);
            }
        }
        s_dsh_row_pulse_on = false;
    }
    for (int i = 0; i < s_dsh_pulse_n; i++) {
        lv_obj_t *row = s_dsh_pulse_rows[i];
        if (!row) {
            continue;
        }
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, row);
        lv_anim_set_values(&b, 90, 220);
        lv_anim_set_time(&b, 700);
        lv_anim_set_playback_time(&b, 700);
        lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&b, anim_border_opa);
        lv_anim_set_delay(&b, (uint32_t)(i * 80));
        lv_anim_start(&b);
    }
    s_dsh_row_pulse_on = true;
}

void start_dsh_status_anims(void)
{
    if (!s_dsh_status || lv_obj_has_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN)) {
        start_dsh_row_pulses();
        return;
    }
    stop_dsh_eq_anims();
    static const int32_t k_lo[3] = {5, 8, 6};
    static const int32_t k_hi[3] = {14, 18, 16};
    static const uint32_t k_ms[3] = {360, 440, 400};
    for (int i = 0; i < 3; i++) {
        if (!s_dsh_eq[i]) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dsh_eq[i]);
        lv_anim_set_values(&a, k_lo[i], k_hi[i]);
        lv_anim_set_time(&a, k_ms[i]);
        lv_anim_set_playback_time(&a, k_ms[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_eq_height);
        lv_anim_set_delay(&a, (uint32_t)(i * 60));
        lv_anim_start(&a);
    }
    s_dsh_eq_anim_on = true;
    if (s_dsh_focus_row) {
        dsh_register_pulse_row(s_dsh_focus_row);
    }
    start_dsh_row_pulses();
}

void dsh_hide_confirm_bar(void)
{
    if (s_dsh_confirm_bar) {
        lv_obj_add_flag(s_dsh_confirm_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_confirm_bar, 0);
    }
}

void dsh_clear_ui_phase(void)
{
    stop_dsh_status_anims();
    s_dsh_phase = DSH_UI_IDLE;
    s_dsh_focus_sess[0] = '\0';
    s_dsh_focus_ws[0] = '\0';
    s_dsh_last_instr[0] = '\0';
    s_dsh_pending_preview[0] = '\0';
    s_dsh_focus_row = nullptr;
    s_dsh_dispatched_us = 0;
    if (s_dsh_status) {
        lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_status, 0);
    }
    dsh_hide_confirm_bar();
}

void dsh_paint_status_strip(void)
{
    if (!s_dsh_status || !s_dsh_status_lbl) {
        return;
    }
    if (s_dsh_phase == DSH_UI_IDLE) {
        /* 仅收顶条 EQ；列表「正在用」行的青色呼吸由 fill_list 保留 */
        stop_dsh_eq_anims();
        lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_status, 0);
        dsh_hide_confirm_bar();
        start_dsh_row_pulses();
        return;
    }
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_dsh_status, 32);
    uint32_t pill = UI_C_TILE_SWITCH;
    uint32_t bar = UI_C_ACCENT_SOFT;
    const char *txt = "续聊中 · 请直接说指令";
    const bool pending = s_dsh_pending_preview[0] != '\0' && s_dsh_phase == DSH_UI_ARMED;
    if (s_dsh_phase == DSH_UI_DISPATCHED) {
        pill = UI_C_TILE_PWM;
        bar = UI_C_INFO;
        if (s_dsh_last_instr[0]) {
            static char buf[120];
            snprintf(buf, sizeof(buf), "已投递 · %s", s_dsh_last_instr);
            txt = buf;
        } else {
            txt = "已投递 · 执行中";
        }
        dsh_hide_confirm_bar();
    } else if (pending) {
        pill = UI_C_TILE_GPIO;
        bar = UI_C_WARN;
        static char pbuf[120];
        snprintf(pbuf, sizeof(pbuf), "待确认 · %s", s_dsh_pending_preview);
        txt = pbuf;
        if (s_dsh_confirm_bar) {
            lv_obj_clear_flag(s_dsh_confirm_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_confirm_bar, 44);
        }
    } else {
        dsh_hide_confirm_bar();
    }
    lv_obj_set_style_bg_color(s_dsh_status, lv_color_hex(pill), 0);
    for (int i = 0; i < 3; i++) {
        if (s_dsh_eq[i]) {
            lv_obj_set_style_bg_color(s_dsh_eq[i], lv_color_hex(bar), 0);
        }
    }
    lv_label_set_text(s_dsh_status_lbl, txt);
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    if (s_card_sub) {
        if (pending) {
            lv_label_set_text(s_card_sub, "待确认 · 说「是」发送 · 「不用」取消");
        } else {
            lv_label_set_text(s_card_sub,
                              s_dsh_phase == DSH_UI_ARMED ? "续聊中 · 直接说 · 点「取消」退出"
                                                         : "已投递 · 等回复");
        }
        lv_obj_set_style_text_color(s_card_sub, lv_color_hex(bar), 0);
    }
    start_dsh_status_anims();
}

/** 续聊焦点=绿；执行中=青；等你确认=琥珀 */
void dsh_style_list_row(lv_obj_t *row, int track_idx, lv_obj_t *idx_pod, lv_obj_t *artist_lbl,
                        const char *artist)
{
    if (!row) {
        return;
    }
    const bool focus = dsh_track_is_focus(track_idx);
    const bool running = dsh_subtitle_is_running(artist);
    const bool needs_you = dsh_subtitle_needs_you(artist);
    if (!focus && !running && !needs_you) {
        return;
    }
    const bool dispatched = focus && s_dsh_phase == DSH_UI_DISPATCHED;
    const bool armed = focus && s_dsh_phase == DSH_UI_ARMED;
    /* 优先级：武装绿 > 待确认琥珀 > 投递/执行中青 */
    const bool work_amber = !armed && needs_you;
    const bool work_cyan = !armed && !work_amber && (dispatched || running);
    if (focus) {
        s_dsh_focus_row = row;
    }
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_border_color(row, work_amber ? UI_WARN : (work_cyan ? UI_INFO : UI_ACCENT), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, work_amber ? UI_TILE_GPIO : (work_cyan ? UI_TILE_PWM : UI_TILE_SWITCH),
                              0);
    if (idx_pod) {
        lv_obj_set_style_bg_color(idx_pod,
                                  work_amber ? UI_CAPSULE_GPIO
                                             : (work_cyan ? UI_CAPSULE_PWM : UI_CAPSULE_ON),
                                  0);
        lv_obj_t *ic = lv_obj_get_child(idx_pod, 0);
        if (ic) {
            lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
            lv_label_set_text(ic, armed ? MATERIAL_SYMBOLS_MIC : MATERIAL_SYMBOLS_PROGRESS_ACTIVITY);
            lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
        }
    }
    if (artist_lbl && armed) {
        if (s_dsh_pending_preview[0]) {
            char line[128];
            snprintf(line, sizeof(line), "待确认 · %s", s_dsh_pending_preview);
            lv_label_set_text(artist_lbl, line);
            lv_obj_set_style_text_color(artist_lbl, UI_WARN, 0);
        } else {
            lv_label_set_text(artist_lbl, "续聊中 · 开麦等待你的指令");
            lv_obj_set_style_text_color(artist_lbl, UI_ACCENT_SOFT, 0);
        }
    } else if (artist_lbl && dispatched) {
        if (s_dsh_last_instr[0]) {
            char line[128];
            snprintf(line, sizeof(line), "已投递 · %s", s_dsh_last_instr);
            lv_label_set_text(artist_lbl, line);
        } else {
            lv_label_set_text(artist_lbl, "已投递 · 正在执行…");
        }
        lv_obj_set_style_text_color(artist_lbl, UI_INFO, 0);
    } else if (artist_lbl && needs_you) {
        lv_obj_set_style_text_color(artist_lbl, UI_WARN, 0);
    } else if (artist_lbl && running) {
        lv_obj_set_style_text_color(artist_lbl, UI_INFO, 0);
    }
    dsh_register_pulse_row(row);
}

void dsh_enter_armed(const char *ws, const char *sess)
{
    s_dsh_phase = DSH_UI_ARMED;
    s_dsh_focus_ws[0] = '\0';
    s_dsh_focus_sess[0] = '\0';
    s_dsh_last_instr[0] = '\0';
    s_dsh_focus_row = nullptr;
    if (ws && ws[0]) {
        snprintf(s_dsh_focus_ws, sizeof(s_dsh_focus_ws), "%s", ws);
    }
    if (sess && sess[0]) {
        snprintf(s_dsh_focus_sess, sizeof(s_dsh_focus_sess), "%s", sess);
    }
    /* 勿同步 fill_list：语音回调在 main 上，整表重建 + scroll_to_view 会撑爆 IDLE0 WDT */
    dsh_paint_status_strip();
    if (s_card_sub && s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr) {
        lv_label_set_text(s_card_sub, "续聊中 · 直接说指令");
    }
}

void start_dsh_detail_poll(void)
{
    remember_dsh_detail_keys();
    if (!s_dsh_detail_sess[0]) {
        stop_dsh_detail_poll();
        return;
    }
    if (!s_dsh_detail_poll) {
        s_dsh_detail_poll = lv_timer_create(on_dsh_detail_poll, DSH_DETAIL_POLL_MS, nullptr);
    } else {
        lv_timer_reset(s_dsh_detail_poll);
    }
}

void on_dsh_detail_poll(lv_timer_t *t)
{
    (void)t;
    if (!dsh_is_detail_view() || s_dsh_detail_busy || !s_dsh_detail_sess[0]) {
        return;
    }
    if (!nodelio_panel_ui_screen_on()) {
        return;
    }
    dsh_fetch_detail(s_dsh_detail_ws, s_dsh_detail_sess, true);
}

void dsh_refresh_list(void)
{
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("未配置服务器");
        return;
    }
    char ws[200] = {0};
    if (s_have_env && s_env) {
        for (int i = 0; i < s_env->filter_n; i++) {
            if (s_env->filters[i].selected && s_env->filters[i].workspace[0]) {
                snprintf(ws, sizeof(ws), "%s", s_env->filters[i].workspace);
                break;
            }
        }
        if (s_env->detail_body) {
            heap_caps_free(s_env->detail_body);
            s_env->detail_body = nullptr;
        }
    }
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    struct refresh_arg {
        nodelio_nvs_config_t cfg;
        char ws[200];
    };
    auto *arg = static_cast<refresh_arg *>(malloc(sizeof(refresh_arg)));
    if (!arg) {
        nl_ov_toast("内存不足");
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->ws, sizeof(arg->ws), "%s", ws);
    nl_ov_toast("刷新…");
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive,
            [](void* p) {
                auto *a = static_cast<refresh_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/dsh.status/invoke", base,
                         a->cfg.device_id);
                char ws_esc[400];
                json_escape_str(ws_esc, sizeof(ws_esc), a->ws);
                char body[440];
                if (ws_esc[0]) {
                    snprintf(body, sizeof(body), "{\"workspace\":\"%s\"}", ws_esc);
                } else {
                    snprintf(body, sizeof(body), "{}");
                }
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp = static_cast<char *>(
                    heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                if (!resp) {
                    nodelio_panel_ui_toast_defer("内存不足");
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err = nodelio_http_post_json_ms(url, a->cfg.device_key, body, &status, resp,
                                                              cap - 1, DSH_HTTP_IO_MS);
                    if (err == ESP_OK && resp[0] && (status == 200 || status == 201)) {
                        nodelio_skill_deliver_json(&a->cfg, resp, false, nullptr, 0);
                    } else {
                        nodelio_panel_ui_toast_defer("刷新失败");
                    }
                    free(resp);
                }
                free(a);
            },
            arg, [](void* p) { free(p); }, nodelio_panel_begin_skill_view())) {
        free(arg);
        nl_ov_toast("刷新失败");
    }
}

void on_dsh_back_list(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    dsh_refresh_list();
}

/** 拉 dsh.status 会话详情（含 body）。silent：定时刷新，无 toast、失败不打扰。 */
void dsh_fetch_detail(const char *ws, const char *sess, bool silent)
{
    if (!sess || !sess[0] || !s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        if (!silent) {
            nl_ov_toast("无法打开详情");
        }
        return;
    }
    if (s_dsh_detail_busy) {
        return;
    }
    struct detail_arg {
        nodelio_nvs_config_t cfg;
        char ws[200];
        char sess[80];
        bool silent;
    };
    auto *arg = static_cast<detail_arg *>(malloc(sizeof(detail_arg)));
    if (!arg) {
        if (!silent) {
            nl_ov_toast("内存不足");
        }
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->ws, sizeof(arg->ws), "%s", ws ? ws : "");
    snprintf(arg->sess, sizeof(arg->sess), "%s", sess);
    arg->silent = silent;
    snprintf(s_dsh_detail_ws, sizeof(s_dsh_detail_ws), "%s", arg->ws);
    snprintf(s_dsh_detail_sess, sizeof(s_dsh_detail_sess), "%s", arg->sess);
    s_dsh_detail_busy = true;
    if (!silent) {
        nl_ov_toast("加载回复…");
    }
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive,
            [](void* p) {
                auto *a = static_cast<detail_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[480];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/dsh.status/invoke", base,
                         a->cfg.device_id);
                /* ws≤199 转义≤398；sess≤79 转义≤158；外壳约 34 → body≥591 */
                char ws_esc[400];
                char sess_esc[160];
                json_escape_str(ws_esc, sizeof(ws_esc), a->ws);
                json_escape_str(sess_esc, sizeof(sess_esc), a->sess);
                /* silent：定时刷新，桥端跳过 TTS；detail_tts 由 Host 从技能配置注入 */
                char body[680];
                if (ws_esc[0]) {
                    if (a->silent) {
                        snprintf(body, sizeof(body),
                                 "{\"workspace\":\"%s\",\"session\":\"%s\",\"silent\":true}", ws_esc,
                                 sess_esc);
                    } else {
                        snprintf(body, sizeof(body), "{\"workspace\":\"%s\",\"session\":\"%s\"}",
                                 ws_esc, sess_esc);
                    }
                } else if (a->silent) {
                    snprintf(body, sizeof(body), "{\"session\":\"%s\",\"silent\":true}", sess_esc);
                } else {
                    snprintf(body, sizeof(body), "{\"session\":\"%s\"}", sess_esc);
                }
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp = static_cast<char *>(
                    heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                if (!resp) {
                    if (!a->silent) {
                        nodelio_panel_ui_toast_defer("内存不足");
                    }
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    esp_err_t err = nodelio_http_post_json_ms(url, a->cfg.device_key, body, &status, resp,
                                                              cap - 1, DSH_HTTP_IO_MS);
                    if (err == ESP_OK && resp[0] && (status == 200 || status == 201)) {
                        /* 静默轮询：用户已回列表/收起则丢弃，避免把详情又顶回来 */
                        DisplayLockGuard view_lock(nl_ov_disp());
                        const bool still =
                            view_lock &&
                            nodelio_panel_skill_view_current(nodelio_panel_job_result_epoch()) &&
                            (!a->silent ||
                             (s_dsh_detail_sess[0] && strcmp(s_dsh_detail_sess, a->sess) == 0 &&
                              !s_minimized && s_have_env && s_env && s_env->detail_body &&
                              s_env->detail_body[0]));
                        if (still) {
                            char spoken[NL_ENV_SPOKEN];
                            spoken[0] = '\0';
                            nodelio_skill_deliver_json(&a->cfg, resp, false, spoken, sizeof(spoken));
                            /* 首次打开：有桥 TTS 则播；续聊武装也播（点开详情就是要听回复）。
                             * 勿仅靠 spoken 文本：服务端可能只带 spoken.audio.urls。 */
                            if (!a->silent) {
                                struct UrlPack {
                                    char urls[NL_ENV_SPOKEN_AUDIO_MAX][NL_ENV_URL];
                                    int n;
                                };
                                auto *pack = static_cast<UrlPack *>(calloc(1, sizeof(UrlPack)));
                                const char *spoken_src =
                                    spoken[0] ? spoken
                                              : (s_env && s_env->display_title[0] ? s_env->display_title
                                                                                 : "详情");
                                char *sp = strdup(spoken_src);
                                if (sp) {
                                    /* 详情回退 display_title 常带 #N；去掉以免 toast/摘要念「井号」 */
                                    for (char *q = sp; *q;) {
                                        if (*q == '#') {
                                            memmove(q, q + 1, strlen(q + 1) + 1);
                                            continue;
                                        }
                                        if ((unsigned char)q[0] == 0xEF && (unsigned char)q[1] == 0xBC &&
                                            (unsigned char)q[2] == 0x83) {
                                            memmove(q, q + 3, strlen(q + 3) + 1);
                                            continue;
                                        }
                                        q++;
                                    }
                                }
                                if (sp && pack) {
                                    pack->n = nodelio_skill_last_spoken_audio_urls(
                                        pack->urls, NL_ENV_SPOKEN_AUDIO_MAX);
                                    ESP_LOGI(TAG, "detail TTS schedule n=%d spoken=%d armed=%d",
                                             pack->n, spoken[0] ? 1 : 0,
                                             nodelio_panel_ui_dsh_is_armed() ? 1 : 0);
                                    const auto reply_epoch = nodelio_panel_job_result_epoch();
                                    Application::GetInstance().Schedule([sp, pack, reply_epoch]() {
                                        if (!nodelio_panel_skill_view_current(reply_epoch)) {
                                            free(sp);
                                            free(pack);
                                            return;
                                        }
                                        if (pack->n > 0) {
                                            nodelio_offline_cmd_yield_for_cloud_audio();
                                            const size_t dram = heap_caps_get_largest_free_block(
                                                MALLOC_CAP_INTERNAL);
                                            if (dram >= 24 * 1024) {
                                                ESP_LOGI(TAG, "detail TTS playlist n=%d dram=%u",
                                                         pack->n, (unsigned)dram);
                                                nodelio_panel_ui_announce_spoken_playlist(
                                                    sp, pack->urls, pack->n);
                                            } else {
                                                ESP_LOGW(TAG, "detail TTS skip dram=%u → toast",
                                                         (unsigned)dram);
                                                nodelio_panel_ui_toast_defer(sp);
                                            }
                                        } else if (sp[0]) {
                                            ESP_LOGW(TAG, "detail TTS no wav → toast only");
                                            nodelio_panel_ui_announce_spoken(sp);
                                        }
                                        free(sp);
                                        free(pack);
                                    });
                                } else {
                                    free(sp);
                                    free(pack);
                                }
                            }
                        }
                    } else if (!a->silent) {
                        if (err != ESP_OK || status == 0 || !resp[0]) {
                            nl_ov_toast_invoke_unreachable("dsh.status");
                        } else {
                            nodelio_panel_ui_toast_defer("加载失败");
                        }
                    }
                    free(resp);
                }
                s_dsh_detail_busy = false;
                free(a);
            },
            arg,
            [](void* p) {
                s_dsh_detail_busy = false;
                free(p);
            },
            silent ? nodelio_panel_skill_view_epoch() : nodelio_panel_begin_skill_view())) {
        s_dsh_detail_busy = false;
        free(arg);
        if (!silent) {
            nl_ov_toast("无法打开详情");
        }
    }
}

/** 从列表行打开详情。 */
void dsh_fetch_session_detail(int track_idx)
{
    if (!s_have_env || !s_env || track_idx < 0 || track_idx >= s_env->track_n) {
        return;
    }
    char ws[200] = {0};
    char sess[80] = {0};
    dsh_extract_arm(&s_env->tracks[track_idx], ws, sizeof(ws), sess, sizeof(sess));
    Board::GetInstance().NoteUserActivity();
    restart_skill_ttl();
    dsh_fetch_detail(ws, sess, false);
}

bool dsh_subtitle_is_running(const char *sub)
{
    if (!sub || !sub[0]) {
        return false;
    }
    return strstr(sub, "正在用") != nullptr || strstr(sub, "正在思考") != nullptr ||
           strstr(sub, "执行中") != nullptr;
}

bool dsh_subtitle_needs_you(const char *sub)
{
    if (!sub || !sub[0]) {
        return false;
    }
    return strstr(sub, "等你确认") != nullptr;
}

/** 焦点会话在新列表中的副标题；无焦点返回 nullptr */
const char *dsh_focus_track_artist(void)
{
    if (!s_env || !s_dsh_focus_sess[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_env->track_n; i++) {
        char ws[200] = {0};
        char sess[80] = {0};
        dsh_extract_arm(&s_env->tracks[i], ws, sizeof(ws), sess, sizeof(sess));
        if (sess[0] && strcmp(sess, s_dsh_focus_sess) == 0) {
            return s_env->tracks[i].artist;
        }
    }
    return nullptr;
}

/** 详情页：执行中/待确认 → 顶状态条 EQ；空闲则按续聊 phase 画 */
void dsh_detail_paint_running(const char *subtitle)
{
    /* 与列表同一套：待确认必须出「发送/取消」。详情原先只画续聊态，确认条被吃掉 */
    const bool pending = s_dsh_pending_preview[0] != '\0' && s_dsh_phase == DSH_UI_ARMED;
    if (pending) {
        dsh_paint_status_strip();
        /* 底栏与确认条叠两套取消，待确认时只留发送/取消 */
        if (s_dsh_detail_bar) {
            lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_detail_bar, 0);
        }
        return;
    }
    dsh_hide_confirm_bar();
    const bool needs_you = dsh_subtitle_needs_you(subtitle);
    const bool running = s_dsh_phase == DSH_UI_DISPATCHED || s_dsh_phase == DSH_UI_ARMED ||
                         dsh_subtitle_is_running(subtitle) || needs_you;
    if (!running) {
        if (s_dsh_phase == DSH_UI_IDLE) {
            stop_dsh_status_anims();
            if (s_dsh_status) {
                lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(s_dsh_status, 0);
            }
        } else {
            dsh_paint_status_strip();
        }
        return;
    }
    if (!s_dsh_status || !s_dsh_status_lbl) {
        return;
    }
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_dsh_status, 32);
    uint32_t pill = UI_C_TILE_PWM;
    uint32_t bar = UI_C_INFO;
    const char *txt = "执行中…";
    if (s_dsh_phase == DSH_UI_ARMED) {
        pill = UI_C_TILE_SWITCH;
        bar = UI_C_ACCENT_SOFT;
        txt = "续聊中 · 请直接说指令";
    } else if (needs_you) {
        pill = UI_C_TILE_GPIO;
        bar = UI_C_WARN;
        txt = subtitle && subtitle[0] ? subtitle : "等你确认";
    } else if (s_dsh_phase == DSH_UI_DISPATCHED && s_dsh_last_instr[0]) {
        static char buf[120];
        snprintf(buf, sizeof(buf), "已投递 · %s", s_dsh_last_instr);
        txt = buf;
    } else if (subtitle && subtitle[0] && dsh_subtitle_is_running(subtitle)) {
        txt = subtitle;
    }
    lv_obj_set_style_bg_color(s_dsh_status, lv_color_hex(pill), 0);
    for (int i = 0; i < 3; i++) {
        if (s_dsh_eq[i]) {
            lv_obj_set_style_bg_color(s_dsh_eq[i], lv_color_hex(bar), 0);
        }
    }
    lv_label_set_text(s_dsh_status_lbl, txt);
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    if (s_card_sub) {
        if (s_dsh_phase == DSH_UI_ARMED) {
            lv_label_set_text(s_card_sub, "续聊中 · 直接说 · 底栏可取消");
        } else if (needs_you) {
            lv_label_set_text(s_card_sub, "待你确认 · 底栏可返回列表");
        } else {
            lv_label_set_text(s_card_sub, "执行中 · 底栏可返回列表");
        }
        lv_obj_set_style_text_color(s_card_sub, lv_color_hex(bar), 0);
    }
    start_dsh_status_anims();
}

/** DSH 会话详情：滚动纯文本；续聊固定底栏；执行中顶条 EQ */
void fill_dsh_detail(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    /* 定时刷新重建时保留阅读位置，避免跳回顶 */
    const lv_coord_t keep_y =
        (s_news_body_lbl && s_card_list) ? lv_obj_get_scroll_y(s_card_list) : 0;
    lv_obj_clean(s_card_list);
    s_news_body_lbl = nullptr;
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(s_card_list, 8, 0);

    const char *artist =
        (s_env->track_n > 0 && s_env->tracks[0].artist[0]) ? s_env->tracks[0].artist : "";
    const bool needs_you = dsh_subtitle_needs_you(artist);
    const bool running =
        dsh_subtitle_is_running(artist) || needs_you || s_dsh_phase != DSH_UI_IDLE;

    if (s_card_sub) {
        if (needs_you) {
            lv_label_set_text(s_card_sub, "待你确认 · 底栏可返回列表");
            lv_obj_set_style_text_color(s_card_sub, UI_WARN, 0);
        } else if (running) {
            lv_label_set_text(s_card_sub, "执行中 · 底栏可返回列表");
            lv_obj_set_style_text_color(s_card_sub, UI_INFO, 0);
        } else {
            lv_label_set_text(s_card_sub, "上滑阅读 · 底栏返回列表 / 续聊");
            lv_obj_set_style_text_color(s_card_sub, UI_TEXT_MUTED, 0);
        }
    }

    if (s_env->track_n > 0 && s_env->tracks[0].title[0]) {
        lv_obj_t *ttl = lv_label_create(s_card_list);
        nl_ov_set_font(ttl);
        lv_label_set_text(ttl, s_env->tracks[0].title);
        lv_obj_set_width(ttl, lv_pct(100));
        lv_label_set_long_mode(ttl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(ttl, UI_TEXT, 0);
    }
    if (artist[0] && !dsh_subtitle_is_running(artist) && !dsh_subtitle_needs_you(artist)) {
        /* 空闲字幕仍放正文上；执行中/待确认改走顶状态条，避免重复 */
        lv_obj_t *sub = lv_label_create(s_card_list);
        nl_ov_set_font(sub);
        lv_label_set_text(sub, artist);
        lv_obj_set_width(sub, lv_pct(100));
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
    }

    s_news_body_lbl = lv_label_create(s_card_list);
    nl_ov_set_font(s_news_body_lbl);
    const char *body =
        (s_env->detail_body && s_env->detail_body[0]) ? s_env->detail_body : "（无正文）";
    lv_label_set_text(s_news_body_lbl, body);
    lv_obj_set_width(s_news_body_lbl, lv_pct(100));
    lv_label_set_long_mode(s_news_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_news_body_lbl, UI_TEXT, 0);
    lv_obj_set_style_pad_top(s_news_body_lbl, 8, 0);

    /* 底栏：返回列表 + 续聊（固定不滚） */
    if (s_dsh_detail_bar) {
        lv_obj_clean(s_dsh_detail_bar);
        lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t *back = make_text_action_btn(s_dsh_detail_bar, "返回列表", on_dsh_back_list, nullptr);
        if (back) {
            lv_obj_set_style_bg_color(back, UI_CAPSULE_OFF, 0);
            lv_obj_t *bl = lv_obj_get_child(back, 0);
            if (bl) {
                lv_obj_set_style_text_color(bl, UI_TEXT_SUB, 0);
            }
            lv_obj_set_size(back, LV_SIZE_CONTENT, 40);
            lv_obj_set_style_pad_hor(back, 14, 0);
        }
        if (s_env->track_n > 0) {
            for (int ai = 0; ai < s_env->tracks[0].invoke_n && ai < NL_ENV_ACTION_MAX; ai++) {
                const intptr_t packed = ((intptr_t)0 << 8) | (intptr_t)ai;
                const char *btn_title = s_env->tracks[0].invokes[ai].title;
                lv_color_t btn_bg = UI_ACCENT;
                if (strstr(btn_title, "续聊") != nullptr) {
                    if (s_dsh_phase == DSH_UI_ARMED) {
                        btn_title = "取消";
                        btn_bg = UI_WARN;
                    } else if (s_dsh_phase == DSH_UI_DISPATCHED || dsh_subtitle_is_running(artist) ||
                               dsh_subtitle_needs_you(artist)) {
                        btn_title = dsh_subtitle_needs_you(artist) ? "待确认" : "执行中";
                        btn_bg = dsh_subtitle_needs_you(artist) ? UI_WARN : UI_INFO;
                    }
                }
                lv_obj_t *ab =
                    make_text_action_btn(s_dsh_detail_bar, btn_title, on_invoke_action, (void *)packed);
                if (ab) {
                    lv_obj_set_style_bg_color(ab, btn_bg, 0);
                    lv_obj_set_size(ab, LV_SIZE_CONTENT, 40);
                    lv_obj_set_style_pad_hor(ab, 16, 0);
                }
            }
        }
        lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 48);
    }

    dsh_detail_paint_running(artist);

    if (keep_y > 0) {
        lv_obj_update_layout(s_card_list);
        lv_obj_scroll_to_y(s_card_list, keep_y, LV_ANIM_OFF);
    } else {
        lv_obj_scroll_to_y(s_card_list, 0, LV_ANIM_OFF);
    }
    apply_card_chrome();
    start_dsh_detail_poll();
}

void nodelio_skill_dsh_on_confirm_pending(const char *instruction)
{
    s_dsh_pending_preview[0] = '\0';
    if (instruction && instruction[0]) {
        snprintf(s_dsh_pending_preview, sizeof(s_dsh_pending_preview), "%s", instruction);
        const size_t n = strlen(s_dsh_pending_preview);
        if (n > 40) {
            s_dsh_pending_preview[37] = '.';
            s_dsh_pending_preview[38] = '.';
            s_dsh_pending_preview[39] = '.';
            s_dsh_pending_preview[40] = '\0';
        }
        if (s_dsh_phase == DSH_UI_IDLE) {
            s_dsh_phase = DSH_UI_ARMED;
        }
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    /* 续聊时常收成 FAB；确认时展开，才能看见「发送/取消」 */
    if (s_dsh_pending_preview[0] && s_minimized) {
        expand_from_fab();
    }
    const bool card_open =
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && s_skill_ui_active;
    if (card_open && s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr && s_card_list) {
        fill_list();
    } else {
        dsh_paint_status_strip();
    }
}

/** 续聊详情在新结果回来前，清掉上一轮正文和报错副标题。 */
static void dsh_hold_detail_for_new_turn(const char *instruction)
{
    if (!s_have_env || !s_env || !s_env->detail_body || !s_env->detail_body[0] ||
        strstr(s_env->app_id, "dsh") == nullptr || s_env->track_n <= 0) {
        return;
    }
    char sess[80] = {0};
    char ws[200] = {0};
    dsh_extract_arm(&s_env->tracks[0], ws, sizeof(ws), sess, sizeof(sess));
    if (s_dsh_focus_sess[0] && sess[0] && strcmp(sess, s_dsh_focus_sess) != 0) {
        return;
    }
    static const char kWait[] = "本轮已发送，完成后显示结果。";
    char *dup = static_cast<char *>(heap_caps_malloc(sizeof(kWait), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!dup) {
        dup = static_cast<char *>(malloc(sizeof(kWait)));
    }
    if (!dup) {
        return;
    }
    memcpy(dup, kWait, sizeof(kWait));
    heap_caps_free(s_env->detail_body);
    s_env->detail_body = dup;
    snprintf(s_env->tracks[0].artist, sizeof(s_env->tracks[0].artist), "执行中");
    if (instruction && instruction[0]) {
        snprintf(s_env->tracks[0].title, sizeof(s_env->tracks[0].title), "%s", instruction);
    }
}

void nodelio_skill_dsh_on_dispatched(const char *instruction)
{
    if (s_dsh_phase == DSH_UI_IDLE && !s_dsh_focus_sess[0]) {
        char ws[200] = {0};
        char sess[80] = {0};
        nodelio_panel_ui_get_dsh_arm(ws, sizeof(ws), sess, sizeof(sess));
        if (sess[0]) {
            snprintf(s_dsh_focus_sess, sizeof(s_dsh_focus_sess), "%s", sess);
        }
        if (ws[0]) {
            snprintf(s_dsh_focus_ws, sizeof(s_dsh_focus_ws), "%s", ws);
        }
    }
    /* 无续聊焦点时仍 toast，避免「说了没反应」 */
    s_dsh_phase = DSH_UI_DISPATCHED;
    s_dsh_dispatched_us = esp_timer_get_time();
    s_dsh_last_instr[0] = '\0';
    if (instruction && instruction[0]) {
        snprintf(s_dsh_last_instr, sizeof(s_dsh_last_instr), "%s", instruction);
        const size_t n = strlen(s_dsh_last_instr);
        if (n > 36) {
            s_dsh_last_instr[33] = '.';
            s_dsh_last_instr[34] = '.';
            s_dsh_last_instr[35] = '.';
            s_dsh_last_instr[36] = '\0';
        }
    }
    dsh_hold_detail_for_new_turn(instruction);
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_toast("正在发送，请等待结果…");
    const bool card_open =
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && s_skill_ui_active;
    if (card_open && s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr && s_card_list) {
        fill_list(); /* 列表或详情都会重画底栏/状态条 */
    } else if (card_open) {
        dsh_paint_status_strip();
    }
}

void nodelio_skill_dsh_on_arm_cleared(void)
{
    if (s_dsh_phase != DSH_UI_ARMED && s_dsh_phase != DSH_UI_DISPATCHED) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        dsh_clear_ui_phase();
        return;
    }
    dsh_clear_ui_phase();
    if (s_have_env && s_env && strstr(s_env->app_id, "dsh") != nullptr && s_card_list &&
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        fill_list();
    }
}

static char s_dsh_arm_ws[200];
static char s_dsh_arm_sess[80];
static char s_dsh_pending_instr[200];
static esp_timer_handle_t s_dsh_arm_timer;
static int64_t s_dsh_arm_deadline_us;

static void dsh_pending_clear(void)
{
    s_dsh_pending_instr[0] = '\0';
    /* 触摸回调可能已持有显示锁；主任务收确认条，并保留随后新建的确认。 */
    Application::GetInstance().Schedule([]() {
        if (!s_dsh_pending_instr[0]) {
            nodelio_skill_dsh_on_confirm_pending(nullptr);
        }
    });
}

static void dsh_arm_timer_stop(void)
{
    if (s_dsh_arm_timer) {
        esp_timer_stop(s_dsh_arm_timer);
    }
}

/** 剥标点后的短肯定/否定（与打开 DSH 确认同口径）。 */
static void dsh_norm_stt(const char *text, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!text) {
        return;
    }
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < buf_len; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '.' || c == '?' || c == '!' || c == ',' || c == ' ' || c == '\t') {
            continue;
        }
        if ((c == 0xE3 && (unsigned char)p[1] == 0x80 &&
             ((unsigned char)p[2] == 0x82 || (unsigned char)p[2] == 0x81)) ||
            (c == 0xEF && (unsigned char)p[1] == 0xBC &&
             ((unsigned char)p[2] == 0x9F || (unsigned char)p[2] == 0x81 ||
              (unsigned char)p[2] == 0x8C))) {
            p += 2;
            continue;
        }
        buf[n++] = *p;
    }
    buf[n] = '\0';
}

static bool dsh_stt_is_yes(const char *norm)
{
    if (!norm || !norm[0]) {
        return false;
    }
    return strcmp(norm, "是的") == 0 || strcmp(norm, "是") == 0 || strcmp(norm, "好的") == 0 ||
           strcmp(norm, "好") == 0 || strcmp(norm, "嗯") == 0 || strcmp(norm, "行") == 0 ||
           strcmp(norm, "可以") == 0 || strcmp(norm, "对") == 0 || strcmp(norm, "要") == 0 ||
           strcmp(norm, "发送") == 0 || strcmp(norm, "确认") == 0 || strcmp(norm, "发吧") == 0 ||
           strcmp(norm, "发") == 0 || strcmp(norm, "发送吧") == 0;
}

static bool dsh_stt_is_no(const char *norm)
{
    if (!norm || !norm[0]) {
        return false;
    }
    return strcmp(norm, "不") == 0 || strcmp(norm, "不用") == 0 || strcmp(norm, "不要") == 0 ||
           strcmp(norm, "算了") == 0 || strcmp(norm, "取消") == 0 || strcmp(norm, "重说") == 0 ||
           strcmp(norm, "不用了") == 0 || strcmp(norm, "重新说") == 0 ||
           strstr(norm, "不要发") != nullptr || strstr(norm, "别发") != nullptr ||
           strstr(norm, "不用发") != nullptr || strcmp(norm, "取消发送") == 0;
}

static void dsh_arm_timeout_cb(void *arg)
{
    (void)arg;
    Application::GetInstance().Schedule([]() {
        if (!nodelio_panel_ui_dsh_is_armed() || esp_timer_get_time() < s_dsh_arm_deadline_us) {
            return;
        }
        nodelio_panel_ui_clear_dsh_arm();
        nodelio_skill_dsh_on_arm_cleared();
        nl_ov_toast("续聊已超时取消");
    });
}

static void dsh_arm_timer_restart(void)
{
    s_dsh_arm_deadline_us = esp_timer_get_time() + 60 * 1000000LL;
    if (!s_dsh_arm_timer) {
        const esp_timer_create_args_t args = {
            .callback = &dsh_arm_timeout_cb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dsh_arm",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_dsh_arm_timer) != ESP_OK) {
            s_dsh_arm_timer = nullptr;
            return;
        }
    }
    esp_timer_stop(s_dsh_arm_timer);
    esp_timer_start_once(s_dsh_arm_timer, 60 * 1000000ULL);
}

extern "C" void nodelio_panel_ui_arm_dsh(const char *workspace, const char *session)
{
    dsh_pending_clear();
    s_dsh_arm_ws[0] = '\0';
    s_dsh_arm_sess[0] = '\0';
    if ((workspace && strlen(workspace) >= sizeof(s_dsh_arm_ws)) ||
        (session && strlen(session) >= sizeof(s_dsh_arm_sess))) {
        dsh_arm_timer_stop();
        nodelio_panel_ui_toast_defer("工作区或会话标识过长，无法续聊");
        return;
    }
    if (workspace && workspace[0]) {
        snprintf(s_dsh_arm_ws, sizeof(s_dsh_arm_ws), "%s", workspace);
    }
    if (session && session[0]) {
        snprintf(s_dsh_arm_sess, sizeof(s_dsh_arm_sess), "%s", session);
    }
    ESP_LOGI(TAG, "arm dsh ws='%s' sess='%s'", s_dsh_arm_ws, s_dsh_arm_sess);
    dsh_arm_timer_restart();
}

extern "C" void nodelio_panel_ui_clear_dsh_arm(void)
{
    dsh_arm_timer_stop();
    s_dsh_arm_ws[0] = '\0';
    s_dsh_arm_sess[0] = '\0';
    dsh_pending_clear();
}

extern "C" bool nodelio_panel_ui_dsh_is_armed(void)
{
    return s_dsh_arm_ws[0] != '\0' || s_dsh_arm_sess[0] != '\0';
}

extern "C" void nodelio_panel_ui_get_dsh_arm(char *workspace, size_t ws_len, char *session,
                                            size_t sess_len)
{
    if (workspace && ws_len) {
        snprintf(workspace, ws_len, "%s", s_dsh_arm_ws);
    }
    if (session && sess_len) {
        snprintf(session, sess_len, "%s", s_dsh_arm_sess);
    }
}

extern "C" bool nodelio_panel_ui_dsh_has_pending_send(void)
{
    return s_dsh_pending_instr[0] != '\0';
}

extern "C" void nodelio_panel_ui_dsh_get_pending_send(char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", s_dsh_pending_instr);
}

extern "C" bool nodelio_panel_ui_dsh_task(const char *instruction)
{
    Board::GetInstance().NoteUserActivity();
    if (instruction && strlen(instruction) >= sizeof(s_dsh_pending_instr)) {
        nodelio_panel_ui_toast_defer("指令过长，未发送，请缩短后重说");
        return true; /* 已处理，勿落回云端再次派活 */
    }
    char *copy = nullptr;
    if (instruction && instruction[0]) {
        copy = strdup(instruction);
        if (!copy) {
            nodelio_panel_ui_toast_defer("内存不足，未发送，请重试");
            return true;
        }
    }
    const std::string workspace(s_dsh_arm_ws);
    const std::string session(s_dsh_arm_sess);
    if (copy) {
        nodelio_panel_ui_clear_dsh_arm();
    }
    Application::GetInstance().Schedule([copy, workspace, session]() {
        if (!nodelio_overlay_visible()) {
            nodelio_overlay_show(false);
        }
        (void)nl_ov_apps_invoke_dsh_task(copy, workspace.c_str(), session.c_str());
        free(copy);
    });
    return true;
}

extern "C" bool nodelio_panel_ui_dsh_confirm_send(const char *instruction)
{
    if (!instruction || !instruction[0]) {
        return false;
    }
    if (strlen(instruction) >= sizeof(s_dsh_pending_instr)) {
        dsh_pending_clear();
        nodelio_panel_ui_toast_defer("指令过长，未发送，请缩短后重说");
        return true;
    }
    if (!nodelio_panel_ui_dsh_is_armed()) {
        /* 无武装则直接派活（「让它…」路径） */
        return nodelio_panel_ui_dsh_task(instruction);
    }
    snprintf(s_dsh_pending_instr, sizeof(s_dsh_pending_instr), "%s", instruction);
    dsh_arm_timer_restart();
    Board::GetInstance().NoteUserActivity();
    /* toast 只预览前缀，完整句在状态条 / pending（勿用 %s 拷满长串，躲 -Wformat-truncation） */
    char toast[128];
    static const char kPrefix[] = "确认发送？";
    const size_t pre_len = sizeof(kPrefix) - 1;
    memcpy(toast, kPrefix, pre_len);
    const size_t room = sizeof(toast) - pre_len - 1;
    const size_t n = strnlen(s_dsh_pending_instr, room);
    memcpy(toast + pre_len, s_dsh_pending_instr, n);
    toast[pre_len + n] = '\0';
    Application::GetInstance().Schedule(
        [toast_copy = std::string(toast), instr = std::string(s_dsh_pending_instr)]() {
            if (instr != s_dsh_pending_instr) {
                return;
            }
            nl_ov_toast(toast_copy.c_str());
            nodelio_skill_dsh_on_confirm_pending(instr.c_str());
        });
    ESP_LOGI(TAG, "dsh confirm pending '%s'", s_dsh_pending_instr);
    return true;
}

extern "C" void nodelio_panel_ui_dsh_confirm_accept(void)
{
    if (!s_dsh_pending_instr[0]) {
        return;
    }
    char instr[200];
    snprintf(instr, sizeof(instr), "%s", s_dsh_pending_instr);
    ESP_LOGI(TAG, "dsh confirm accept '%s'", instr);
    (void)nodelio_panel_ui_dsh_task(instr);
}

extern "C" void nodelio_panel_ui_dsh_confirm_reject(void)
{
    if (!s_dsh_pending_instr[0]) {
        return;
    }
    ESP_LOGI(TAG, "dsh confirm reject '%s'", s_dsh_pending_instr);
    dsh_pending_clear();
    dsh_arm_timer_restart();
    Board::GetInstance().NoteUserActivity();
    Application::GetInstance().Schedule([]() {
        nl_ov_toast("已取消，请重新说");
    });
}

extern "C" int nodelio_panel_ui_dsh_try_confirm_send(const char *stt)
{
    if (!s_dsh_pending_instr[0] || !stt || !stt[0]) {
        return 0;
    }
    char norm[64];
    dsh_norm_stt(stt, norm, sizeof(norm));
    if (dsh_stt_is_no(norm)) {
        nodelio_panel_ui_dsh_confirm_reject();
        return 2;
    }
    if (dsh_stt_is_yes(norm)) {
        nodelio_panel_ui_dsh_confirm_accept();
        return 1;
    }
    /* 其它话当作改口：整句替换待确认内容 */
    if (strlen(norm) >= 2) {
        ESP_LOGI(TAG, "dsh confirm replace → '%s'", stt);
        (void)nodelio_panel_ui_dsh_confirm_send(stt);
        return 2;
    }
    return 0;
}

