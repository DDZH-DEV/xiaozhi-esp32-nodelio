#include "skill_ui_priv.h"

lv_obj_t *s_now_bar = nullptr;
lv_obj_t *s_now_eq_wrap = nullptr;
lv_obj_t *s_now_eq[SKILL_EQ_N] = {nullptr};
lv_obj_t *s_now_lbl = nullptr;
lv_obj_t *s_now_sub = nullptr;
lv_obj_t *s_now_btns = nullptr;
lv_obj_t *s_now_pause_btn = nullptr;
lv_obj_t *s_now_prev_btn = nullptr;
lv_obj_t *s_now_next_btn = nullptr;
lv_obj_t *s_mini_fab = nullptr;
lv_obj_t *s_mini_ic = nullptr;
lv_anim_t s_now_border_anim;
bool s_now_border_anim_on = false;
bool s_mini_pulse_on = false;

/** docked：框内凹槽。否则独立一条，同样不用绿框。 */
void style_now_bar(bool docked)
{
    if (!s_now_bar) {
        return;
    }
    const bool wide = nodelio_display_profile()->nav_width != 0;
    if (!wide) {
        lv_obj_set_style_shadow_width(s_now_bar, 0, 0);
        lv_obj_set_style_shadow_opa(s_now_bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_now_bar, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(s_now_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_now_bar, 0, 0);
        lv_obj_set_style_border_width(s_now_bar, 1, 0);
        lv_obj_set_style_border_side(s_now_bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(s_now_bar, UI_BORDER, 0);
        lv_obj_set_style_border_opa(s_now_bar, LV_OPA_60, 0);
        lv_obj_set_style_pad_hor(s_now_bar, 14, 0);
        return;
    }
    if (docked) {
        lv_obj_set_style_shadow_width(s_now_bar, 0, 0);
        lv_obj_set_style_shadow_opa(s_now_bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s_now_bar, UI_PANEL, 0);
        lv_obj_set_style_bg_opa(s_now_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_now_bar, UI_RADIUS_SM, 0);
        lv_obj_set_style_border_width(s_now_bar, 0, 0);
        return;
    }
    lv_obj_set_style_bg_color(s_now_bar, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_now_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_now_bar, 16, 0);
    ui_style_card(s_now_bar);
    lv_obj_set_style_border_width(s_now_bar, ui_palette_is_dark() ? 1 : 0, 0);
    lv_obj_set_style_border_side(s_now_bar, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_color(s_now_bar, UI_BORDER, 0);
    lv_obj_set_style_border_opa(s_now_bar, LV_OPA_80, 0);
}

bool nodelio_skill_mini_fab_visible(void)
{
    return s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
}

/** 按 media_kind 刷新 now_playing 副文案。上一首/下一首对曲库、电台、播客都保留。 */
void apply_now_media_chrome(bool playing, bool paused, bool list_open)
{
    if (s_now_prev_btn) {
        lv_obj_clear_flag(s_now_prev_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_next_btn) {
        lv_obj_clear_flag(s_now_next_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_now_sub) {
        return;
    }
    if (paused) {
        lv_label_set_text(s_now_sub, "已暂停 · 点播放继续");
        return;
    }
    const char *kind = env_media_kind();
    if (strcmp(kind, "live") == 0) {
        lv_label_set_text(s_now_sub, list_open ? "直播 · 收听中" : "直播 · 点此打开列表");
    } else if (strcmp(kind, "episode") == 0) {
        lv_label_set_text(s_now_sub, list_open ? (playing ? "播客 · 播放中" : "播客")
                                                : "播客 · 点此打开列表");
    } else if (list_open) {
        lv_label_set_text(s_now_sub, playing ? "正在播放" : "点此打开列表");
    } else {
        lv_label_set_text(s_now_sub, "点此打开列表");
    }
}

void on_minimize(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    minimize_to_fab();
}

void on_mini_fab(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    expand_from_fab();
}

/* 播放器关闭时恢复列表序号，避免耳机图标一直占着当前行 */
void clear_play_mark(void)
{
    if (s_play_idx < 0 || s_play_idx >= 64) {
        s_play_idx = -1;
        return;
    }
    if (s_row_idx_labels[s_play_idx]) {
        if (s_env && strcmp(s_env->card_type, "music_list") == 0) {
            paint_music_mark(s_row_idx_labels[s_play_idx], false, s_play_idx);
        } else {
            char idx_buf[16];
            snprintf(idx_buf, sizeof(idx_buf), "%d", s_play_idx + 1);
            nl_ov_set_font(s_row_idx_labels[s_play_idx]);
            lv_label_set_text(s_row_idx_labels[s_play_idx], idx_buf);
            lv_obj_set_style_text_color(s_row_idx_labels[s_play_idx], UI_ACCENT_SOFT, 0);
        }
    }
    if (s_row_btns[s_play_idx]) {
        lv_obj_t *lbl = lv_obj_get_child(s_row_btns[s_play_idx], 0);
        if (lbl) {
            lv_label_set_text(lbl, MATERIAL_SYMBOLS_PLAY_ARROW);
        }
    }
    s_play_idx = -1;
}

/* 只更新指定行的图标，避免重建整个列表 */
void update_row_icon(int idx)
{
    if (idx < 0 || idx >= 64) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    /* 更新序号胶囊：当前播放显示耳机 */
    if (s_row_idx_labels[idx]) {
        if (s_env && strcmp(s_env->card_type, "music_list") == 0) {
            paint_music_mark(s_row_idx_labels[idx], true, idx);
        } else {
            lv_obj_set_style_text_font(s_row_idx_labels[idx], &font_material_symbols_20_4, 0);
            lv_label_set_text(s_row_idx_labels[idx], MATERIAL_SYMBOLS_HEADPHONES);
            lv_obj_set_style_text_color(s_row_idx_labels[idx], UI_ACCENT, 0);
        }
    }
    /* 更新播放按钮：改为暂停图标 */
    if (s_row_btns[idx]) {
        lv_obj_t *lbl = lv_obj_get_child(s_row_btns[idx], 0);
        if (lbl) {
            lv_label_set_text(lbl, MATERIAL_SYMBOLS_PAUSE);
        }
    }
}

void play_at(int idx)
{
    if (!nodelio_caps_has_media()) {
        nl_ov_toast("本机暂不支持播放");
        return;
    }
    if (idx < 0 || idx >= s_env->track_n || !s_env->tracks[idx].playback_url[0]) {
        return;
    }
    /* 恢复上一首的图标 */
    if (s_play_idx >= 0 && s_play_idx < 64) {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            if (s_row_idx_labels[s_play_idx]) {
                if (strcmp(s_env->card_type, "music_list") == 0) {
                    paint_music_mark(s_row_idx_labels[s_play_idx], false, s_play_idx);
                } else {
                    char idx_buf[16];
                    snprintf(idx_buf, sizeof(idx_buf), "%d", s_play_idx + 1);
                    nl_ov_set_font(s_row_idx_labels[s_play_idx]);
                    lv_label_set_text(s_row_idx_labels[s_play_idx], idx_buf);
                    lv_obj_set_style_text_color(s_row_idx_labels[s_play_idx], UI_ACCENT_SOFT, 0);
                }
            }
            if (s_row_btns[s_play_idx]) {
                lv_obj_t *lbl = lv_obj_get_child(s_row_btns[s_play_idx], 0);
                if (lbl) {
                    lv_label_set_text(lbl, MATERIAL_SYMBOLS_PLAY_ARROW);
                }
            }
        }
    }
    s_play_idx = idx;
    (void)nodelio_media_play_track(&s_skill_cfg, &s_env->tracks[idx]);
    /* 只更新当前行的图标，避免重建整个列表 */
    update_row_icon(idx);
}

int neighbor_playable(int from, int dir)
{
    if (s_env->track_n <= 0 || (dir != 1 && dir != -1)) {
        return -1;
    }
    /* from < 0：下一首从 0 起，上一首从末尾起（换一批后当前曲可能不在新列表里） */
    int start = from;
    if (start < 0 || start >= s_env->track_n) {
        start = (dir > 0) ? -1 : 0;
    }
    for (int step = 1; step <= s_env->track_n; step++) {
        int idx = start + dir * step;
        idx %= s_env->track_n;
        if (idx < 0) {
            idx += s_env->track_n;
        }
        if (s_env->tracks[idx].playback_url[0]) {
            return idx;
        }
    }
    return -1;
}

void on_stop_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    nodelio_media_stop();
    /* 播放条和曲库一起退下，动画结束后释放对象 */
    dismiss_library_and_player();
}

void on_pause_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (nodelio_media_is_paused()) {
        nodelio_media_resume();
    } else {
        nodelio_media_pause();
    }
}

/** 播歌时内部 DRAM 常开不起唤醒：点麦克风 = 暂停并进对话 */
void on_voice_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
    nl_ov_toast("请说…");
    Application::GetInstance().Schedule([]() {
        auto &app = Application::GetInstance();
        if (nodelio_media_is_playing()) {
            nodelio_media_pause_for_voice();
            nodelio_offline_cmd_yield_for_cloud_audio();
            app.GetAudioService().EnableVoiceProcessing(false);
        } else if (nodelio_media_is_paused()) {
            /* 已暂停：清掉 pause_close，避免对话中途关播放器 */
            nodelio_media_pause_for_voice(); /* no-op if not playing; need disarm close */
        }
        if (app.GetDeviceState() == kDeviceStateIdle ||
            app.GetDeviceState() == kDeviceStateNotifying) {
            app.ToggleChatState();
        }
    });
}

void on_prev_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!nodelio_skill_media_prev()) {
        nl_ov_toast("没有上一首");
    }
}

void on_next_now(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!nodelio_skill_media_next()) {
        nl_ov_toast("没有下一首");
    }
}

void set_pause_btn_icon(bool playing)
{
    if (!s_now_pause_btn) {
        return;
    }
    lv_obj_t *ic = lv_obj_get_child(s_now_pause_btn, 0);
    if (ic) {
        lv_label_set_text(ic, playing ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW);
    }
}

bool nodelio_skill_media_prev(void)
{
    int idx = neighbor_playable(s_play_idx, -1);
    if (idx < 0) {
        return false;
    }
    play_at(idx);
    return true;
}

bool nodelio_skill_media_next(void)
{
    int idx = neighbor_playable(s_play_idx, +1);
    if (idx < 0) {
        return false;
    }
    play_at(idx);
    return true;
}

bool nodelio_skill_media_play_any(char *title_out, size_t title_out_len)
{
    if (title_out && title_out_len > 0) {
        title_out[0] = '\0';
    }
    if (!s_have_env || s_env->track_n <= 0) {
        return false;
    }
    int idxs[NL_ENV_TRACK_MAX];
    int n = 0;
    for (int i = 0; i < s_env->track_n && n < NL_ENV_TRACK_MAX; i++) {
        if (s_env->tracks[i].playback_url[0]) {
            idxs[n++] = i;
        }
    }
    if (n <= 0) {
        return false;
    }
    int slot = (int)(esp_random() % (uint32_t)n);
    if (n > 1 && idxs[slot] == s_play_idx) {
        slot = (slot + 1) % n;
    }
    const int pick = idxs[slot];
    play_at(pick);
    if (title_out && title_out_len > 0 && pick >= 0 && pick < s_env->track_n) {
        snprintf(title_out, title_out_len, "%s", s_env->tracks[pick].title);
    }
    return true;
}

/** 1-based 序号 → 当前列表下标。跳过没有 URL 的条目，与播报序号保持一致。 */
static int playable_index_at(int ordinal)
{
    if (!s_have_env || !s_env || ordinal <= 0) {
        return -1;
    }
    int seen = 0;
    for (int i = 0; i < s_env->track_n; i++) {
        if (!s_env->tracks[i].playback_url[0]) {
            continue;
        }
        seen++;
        if (seen == ordinal) {
            return i;
        }
    }
    return -1;
}

bool nodelio_skill_media_play_index(int ordinal, char *title_out, size_t title_out_len)
{
    if (title_out && title_out_len > 0) {
        title_out[0] = '\0';
    }
    const int pick = playable_index_at(ordinal);
    if (pick < 0) {
        return false;
    }
    play_at(pick);
    if (title_out && title_out_len > 0) {
        snprintf(title_out, title_out_len, "%s", s_env->tracks[pick].title);
    }
    return true;
}

int nodelio_skill_media_list_titles(char *out, size_t out_len, int max)
{
    if (!out || out_len == 0 || max <= 0) {
        return 0;
    }
    out[0] = '\0';
    if (!s_have_env || !s_env || s_env->track_n <= 0) {
        return 0;
    }
    size_t off = 0;
    int n = 0;
    int seen = 0;
    for (int i = 0; i < s_env->track_n && n < max; i++) {
        if (!s_env->tracks[i].playback_url[0]) {
            continue;
        }
        seen++;
        const int w = snprintf(out + off, out_len - off, "%s%d. %s", off ? "\n" : "", seen,
                               s_env->tracks[i].title);
        if (w <= 0 || (size_t)w >= out_len - off) {
            break;
        }
        off += (size_t)w;
        n++;
    }
    return n;
}

bool nodelio_skill_media_is_sd(void)
{
    if (!s_have_env || !s_env || s_env->track_n <= 0) {
        return false;
    }
    return strncmp(s_env->tracks[0].playback_url, "sd://", 5) == 0;
}

static void anim_mini_spin(void *var, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)var, v, 0);
}

void stop_mini_pulse(void)
{
    if (s_mini_ic) {
        lv_anim_delete(s_mini_ic, anim_mini_spin);
        lv_obj_set_style_transform_rotation(s_mini_ic, 0, 0);
    }
    s_mini_pulse_on = false;
}

void start_mini_pulse(void)
{
    if (!s_mini_ic) {
        return;
    }
    stop_mini_pulse();
    lv_obj_set_style_transform_pivot_x(s_mini_ic, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_mini_ic, LV_PCT(50), 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mini_ic);
    lv_anim_set_values(&a, 0, 3600);
    lv_anim_set_time(&a, 2400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, anim_mini_spin);
    lv_anim_start(&a);
    s_mini_pulse_on = true;
}

void hide_mini_fab(void)
{
    stop_mini_pulse();
    if (s_mini_fab) {
        reset_obj_anim_style(s_mini_fab);
        lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    }
    nl_ov_apps_layout_chrome();
}

void show_mini_fab(bool playing)
{
    if (!s_mini_fab) {
        return;
    }
    lv_obj_clear_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    if (s_mini_ic) {
        lv_label_set_text(s_mini_ic, playing ? MATERIAL_SYMBOLS_MUSIC_NOTE : MATERIAL_SYMBOLS_HEADPHONES);
    }
    anim_show_obj(s_mini_fab, false);
    if (playing) {
        start_mini_pulse();
    } else {
        stop_mini_pulse();
    }
    layout_skill_layers();
    raise_skill_layers();
}

void minimize_to_fab(void)
{
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    s_minimized = true;
    reset_obj_anim_style(s_scrim);
    reset_obj_anim_style(s_card);
    reset_obj_anim_style(s_now_bar);
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_bar) {
        stop_now_anims();
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    }
    set_drawer_for_skill(false);
    show_mini_fab(nodelio_media_is_playing());
    nl_ov_apps_notify_skill_open(false);
    ensure_chrome_order();
}

void expand_from_fab(void)
{
    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }
    s_minimized = false;
    hide_mini_fab();

    const bool playing = nodelio_media_is_playing();
    const bool paused = nodelio_media_is_paused();
    const bool have_list = s_have_env && s_env->track_n > 0 && s_card;

    if (have_list) {
        set_drawer_for_skill(true);
        if (s_scrim) {
            lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
            anim_show_obj(s_scrim, false);
        }
        if (s_card_title) {
            const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
            lv_label_set_text(s_card_title, title);
        }
        fill_list();
        lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        anim_show_obj(s_card, true);
    } else {
        set_drawer_for_skill(false);
    }

    if (s_now_bar && (playing || paused)) {
        apply_now_media_chrome(playing, paused, have_list);
        set_pause_btn_icon(playing);
        lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        if (playing) {
            start_now_anims();
        } else {
            stop_now_anims();
        }
        anim_show_obj(s_now_bar, true);
    }

    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
    if (have_list) {
        nl_ov_apps_notify_skill_open(true);
    }
}

void stop_now_anims(void)
{
    if (s_now_border_anim_on && s_now_bar) {
        lv_anim_delete(s_now_bar, anim_border_opa);
        s_now_border_anim_on = false;
        lv_obj_set_style_border_opa(s_now_bar, SKILL_BORDER_OPA, 0);
    }
    for (int i = 0; i < SKILL_EQ_N; i++) {
        if (s_now_eq[i]) {
            lv_anim_delete(s_now_eq[i], anim_eq_height);
            lv_obj_set_height(s_now_eq[i], 8 + (i % 3) * 4);
        }
    }
}

void start_now_anims(void)
{
    if (!s_now_bar) {
        return;
    }
    stop_now_anims();

    lv_anim_init(&s_now_border_anim);
    lv_anim_set_var(&s_now_border_anim, s_now_bar);
    lv_anim_set_values(&s_now_border_anim, 110, 240);
    lv_anim_set_time(&s_now_border_anim, 900);
    lv_anim_set_playback_time(&s_now_border_anim, 900);
    lv_anim_set_repeat_count(&s_now_border_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_now_border_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&s_now_border_anim, anim_border_opa);
    lv_anim_start(&s_now_border_anim);
    s_now_border_anim_on = true;

    static const int32_t k_lo[SKILL_EQ_N] = {6, 10, 8, 12};
    static const int32_t k_hi[SKILL_EQ_N] = {18, 24, 20, 22};
    static const uint32_t k_ms[SKILL_EQ_N] = {380, 460, 420, 500};
    for (int i = 0; i < SKILL_EQ_N; i++) {
        if (!s_now_eq[i]) {
            continue;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_now_eq[i]);
        lv_anim_set_values(&a, k_lo[i], k_hi[i]);
        lv_anim_set_time(&a, k_ms[i]);
        lv_anim_set_playback_time(&a, k_ms[i]);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, anim_eq_height);
        lv_anim_set_delay(&a, (uint32_t)(i * 70));
        lv_anim_start(&a);
    }
}

void nodelio_panel_ui_media_now_playing(const char *title, bool playing)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    if (s_ui_closing && (!title || !title[0])) {
        return;
    }
    if (s_ui_closing) {
        s_close_gen += 1;
        s_ui_closing = false;
        reset_obj_anim_style(s_scrim);
        reset_obj_anim_style(s_card);
        reset_obj_anim_style(s_now_bar);
    }
    if (!nodelio_caps_has_surface("now_playing") || !s_now_bar) {
        return;
    }
    /* 无标题 → 关闭播放器 */
    if (!title || !title[0]) {
        stop_now_anims();
        set_pause_btn_icon(true);
        clear_play_mark();
        const bool bar_was_up = s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_minimized) {
            if (s_have_env && s_env->track_n > 0) {
                show_mini_fab(false);
            } else {
                s_minimized = false;
                hide_mini_fab();
            }
        }
        sync_player_block_scrim();
        layout_skill_layers();
        if (bar_was_up) {
            reflow_music_list_if_open();
        }
        raise_skill_layers();
        ensure_chrome_order();
        return;
    }

    lv_label_set_text(s_now_lbl, title);
    set_pause_btn_icon(playing);

    if (s_minimized) {
        show_mini_fab(playing);
        layout_skill_layers();
        ensure_chrome_order();
        return;
    }

    {
        const bool list_open = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        apply_now_media_chrome(playing, !playing, list_open);
    }
    const bool was_hidden = lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    if (playing) {
        start_now_anims();
    } else {
        stop_now_anims();
    }
    if (was_hidden) {
        anim_show_obj(s_now_bar, true);
    }
    sync_player_block_scrim();
    layout_skill_layers();
    if (was_hidden) {
        reflow_music_list_if_open();
    }
    raise_skill_layers();
    ensure_chrome_order();
}
