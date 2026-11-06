#include "skill_ui_priv.h"

lv_obj_t *s_scrim = nullptr;
lv_obj_t *s_card = nullptr;
lv_obj_t *s_filter_bar = nullptr;
lv_obj_t *s_src_bar = nullptr;
lv_obj_t *s_card_list = nullptr;
lv_obj_t *s_card_title = nullptr;
lv_obj_t *s_card_sub = nullptr;
lv_timer_t *s_ttl_tmr = nullptr;
bool s_minimized = false;
bool s_ui_closing = false;
uint32_t s_close_gen = 0;
nodelio_nvs_config_t s_skill_cfg;
/** ~54KB：必须 PSRAM。由 deliver 堆分配后交给 show 接管，禁止再拷一份。 */
nl_content_envelope_t *s_env = nullptr;
bool s_have_env = false;
int s_play_idx = -1;
int s_list_page = 0;
lv_obj_t *s_hdr_mid_btn = nullptr;
lv_obj_t *s_hdr_btns = nullptr;
lv_obj_t *s_hdr = nullptr;        /* 头部行；精简模式下整体收起 */
lv_obj_t *s_hdr_txt = nullptr;    /* 标题 + 副标题；精简模式下只留标题 */
lv_obj_t *s_close_btn = nullptr;  /* 关闭钮；精简模式下浮到卡片右上角 */
bool s_hdr_minimal = false;     /* 日历：顶部只留关闭，把高度让给内容 */
lv_obj_t *s_row_idx_labels[64] = {nullptr};  /* 序号胶囊标签（数字/耳机） */
lv_obj_t *s_row_btns[64] = {nullptr};       /* 播放/暂停按钮 */

void free_skill_env(void)
{
    if (!s_env) {
        return;
    }
    if (s_env->detail_body) {
        heap_caps_free(s_env->detail_body);
        s_env->detail_body = nullptr;
    }
    heap_caps_free(s_env);
    s_env = nullptr;
    s_have_env = false;
}

void free_owned_env(nl_content_envelope_t *env)
{
    if (!env) {
        return;
    }
    if (env->detail_body) {
        heap_caps_free(env->detail_body);
        env->detail_body = nullptr;
    }
    heap_caps_free(env);
}

/** 技能层抬完后把状态栏、小智和提示条按固定顺序压回去。 */
void ensure_chrome_order(void)
{
    nl_ov_raise_chrome();
}

/** 宽屏且列表、播放条同时可见：播放条嵌进列表框底，不再单独占一条。 */
bool skill_player_docked(void)
{
    if (!nodelio_display_profile()->nav_width) {
        return false;
    }
    const bool card_up = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    const bool bar_up = s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    return card_up && bar_up;
}

/** 列表卡可用高度。宽屏播放条嵌进框内时，外框铺满右列。 */
int skill_card_h_max(void)
{
    const auto *p = nodelio_display_profile();
    if (skill_player_docked()) {
        const int h = (int)p->height - skill_top_y() - SKILL_GAP;
        return h < SKILL_CARD_H_MIN ? SKILL_CARD_H_MIN : h;
    }
    if (!p->nav_width) {
        int h = (int)p->height;
        if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
            h -= SKILL_NOW_H;
        }
        return h < SKILL_CARD_H_MIN ? SKILL_CARD_H_MIN : h;
    }
    int h = (int)p->result_max_height + NL_UI_STATUS_H;
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        const int bottom = SKILL_NOW_H + 3 * SKILL_GAP;
        h = (int)p->height - skill_top_y() - bottom;
    }
    return h < SKILL_CARD_H_MIN ? SKILL_CARD_H_MIN : h;
}

/** 宽屏：右侧应用宽度。首页给左栏留固定宽，避免启动台被应用列表轨挤扁。 */
int skill_panel_w(void)
{
    const auto *p = nodelio_display_profile();
    if (!p->nav_width) {
        return (int)p->width;
    }
    const int base = (int)p->result_width;
    if (s_active_tab == NL_TAB_HOME) {
        const int home_col = 520;
        int w = (int)p->width - (int)p->nav_width - home_col - SKILL_GAP;
        if (w < base) {
            w = base;
        }
        return w;
    }
    const int rail = nl_ov_apps_split_rail_w();
    if (rail <= 0) {
        return base;
    }
    /* 与设置页相同：左 16 + 卡片 + 右 16，窗口再留屏幕右缘。 */
    const int left = (int)p->nav_width + 16 + rail + 16;
    int w = (int)p->width - left - SKILL_GAP;
    if (w < base) {
        w = base;
    }
    return w;
}

/** 应用外框：石板细边，靠表面提亮，不用品牌绿描边。 */
void style_skill_frame(void)
{
    if (!s_card) {
        return;
    }
    const bool wide = nodelio_display_profile()->nav_width != 0;
    if (!wide) {
        lv_obj_set_style_bg_color(s_card, UI_BG, 0);
        lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_card, 0, 0);
        lv_obj_set_style_border_width(s_card, 0, 0);
        lv_obj_set_style_shadow_width(s_card, 0, 0);
        lv_obj_set_style_pad_all(s_card, 12, 0);
        return;
    }
    lv_obj_set_style_bg_color(s_card, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 20, 0);
    lv_obj_set_style_border_width(s_card, ui_palette_is_dark() ? 1 : 0, 0);
    lv_obj_set_style_border_color(s_card, UI_BORDER, 0);
    lv_obj_set_style_border_opa(s_card, LV_OPA_70, 0);
    ui_style_card(s_card);
}

void layout_skill_layers(void)
{
    const int panel_w = skill_panel_w();
    const bool card_up = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    const bool bar_up = s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    const bool wide = nodelio_display_profile()->nav_width != 0;
    const bool dock = wide && card_up && bar_up;
    /* 宽屏：播放条只占右侧技能列，勿再砍左轨应用列表一行高 */
    const int reserve_bottom = (!wide && bar_up && !card_up) ? (SKILL_NOW_H + 2 * SKILL_GAP) : 0;
    nl_ov_workspace_reserve(card_up ? (wide ? panel_w + SKILL_GAP : panel_w) : 0,
                            reserve_bottom);
    int y = wide ? skill_top_y() : 0;
    if (bar_up && !dock) {
        style_now_bar(false);
        if (wide) {
            const int bar_w = panel_w;
            lv_obj_set_width(s_now_bar, bar_w);
            if (s_now_lbl) {
                lv_obj_set_width(s_now_lbl, bar_w - 240);
            }
            lv_obj_align(s_now_bar, LV_ALIGN_BOTTOM_RIGHT, -SKILL_GAP, -SKILL_GAP);
        } else {
            lv_obj_set_size(s_now_bar, panel_w, SKILL_NOW_H);
            if (s_now_lbl) {
                lv_obj_set_width(s_now_lbl, panel_w - 330);
            }
            lv_obj_align(s_now_bar, LV_ALIGN_TOP_LEFT, 0, y);
            y += SKILL_NOW_H;
        }
    }
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        if (wide) {
            const int scrim_bottom = (bar_up && !card_up) ? (SKILL_NOW_H + 2 * SKILL_GAP) : 0;
            lv_obj_set_size(s_scrim, card_up ? panel_w + SKILL_GAP : 0,
                            nodelio_display_profile()->height - scrim_bottom);
            lv_obj_align(s_scrim, LV_ALIGN_TOP_RIGHT, 0, 0);
        } else {
            lv_obj_set_size(s_scrim, nodelio_display_profile()->width,
                            nodelio_display_profile()->height);
            lv_obj_align(s_scrim, LV_ALIGN_TOP_LEFT, 0, 0);
        }
    }
    if (card_up) {
        lv_obj_set_width(s_card, panel_w);
        if (s_have_env && s_env) {
            apply_card_chrome();
        } else {
            const int h = wide ? skill_card_h_max() : (nodelio_display_profile()->height - y);
            lv_obj_set_height(s_card, h);
            lv_obj_set_style_max_height(s_card, h, 0);
            lv_obj_set_style_pad_bottom(s_card, dock ? (SKILL_NOW_H + 18) : 12, 0);
        }
        style_skill_frame();
        lv_obj_align(s_card, wide ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, wide ? -SKILL_GAP : 0, y);
    }
    if (dock) {
        const int bar_w = panel_w - 20;
        lv_obj_set_width(s_now_bar, bar_w);
        if (s_now_lbl) {
            lv_obj_set_width(s_now_lbl, bar_w - 240);
        }
        style_now_bar(true);
        /* 贴在外框底边内侧，左右各留 10，和列表合成一块 */
        lv_obj_align(s_now_bar, LV_ALIGN_BOTTOM_RIGHT, -(SKILL_GAP + 10), -(SKILL_GAP + 10));
        lv_obj_move_foreground(s_now_bar);
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_align(s_mini_fab, LV_ALIGN_BOTTOM_RIGHT, -14, -(NL_UI_TAB_H + 32));
    }
    nl_ov_apps_layout_chrome();
    nl_ov_settings_sync_columns();
}

void nodelio_skill_result_layout(void) { layout_skill_layers(); }

void nodelio_skill_result_reapply_text_scale(void)
{
    /* 新闻详情字号跟全局档走；卡未打开时也先记下，下次打开用新档。A± 仍可临时改。 */
    news_sync_global_text_scale();
    if (!s_card || lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) || !s_have_env || !s_env) {
        return;
    }
    if (s_card_title) {
        nl_ov_set_font_title(s_card_title);
        lv_obj_set_height(s_card_title, nl_ov_ui_font_line_h());
    }
    if (s_card_sub) {
        nl_ov_set_font(s_card_sub);
        lv_obj_set_height(s_card_sub, nl_ov_ui_font_line_h());
    }
    if (s_now_lbl) {
        nl_ov_set_font(s_now_lbl);
    }
    if (s_now_sub) {
        nl_ov_set_font(s_now_sub);
    }
    fill_list();
    layout_skill_layers();
}

static void paint_round_btn(lv_obj_t *btn, lv_color_t bg, lv_color_t fg)
{
    if (!btn) {
        return;
    }
    lv_obj_set_style_bg_color(btn, bg, 0);
    if (lv_obj_t *ic = lv_obj_get_child(btn, 0)) {
        lv_obj_set_style_text_color(ic, fg, 0);
    }
}

void nodelio_skill_result_reapply_appearance(void)
{
    if (s_scrim) {
        lv_obj_set_style_bg_color(s_scrim, UI_BG, 0);
    }
    style_skill_frame();
    if (s_card_title) {
        lv_obj_set_style_text_color(s_card_title, UI_TEXT, 0);
    }
    if (s_card_sub) {
        lv_obj_set_style_text_color(s_card_sub, UI_TEXT_MUTED, 0);
    }
    if (s_hdr_btns) {
        const uint32_t n = lv_obj_get_child_count(s_hdr_btns);
        bool first_btn = true;
        for (uint32_t i = 0; i < n; i++) {
            lv_obj_t *ch = lv_obj_get_child(s_hdr_btns, i);
            if (ch == s_src_bar || ch == s_news_zoom_bar) {
                continue;
            }
            paint_round_btn(ch, UI_CAPSULE_OFF, first_btn ? UI_ACCENT_SOFT : UI_TEXT_SUB);
            first_btn = false;
        }
    }
    if (s_now_bar) {
        style_now_bar(skill_player_docked());
    }
    if (s_now_eq_wrap) {
        lv_obj_set_style_bg_color(s_now_eq_wrap, UI_TILE_SWITCH, 0);
    }
    for (int i = 0; i < SKILL_EQ_N; i++) {
        if (s_now_eq[i]) {
            lv_obj_set_style_bg_color(s_now_eq[i], UI_ACCENT_SOFT, 0);
        }
    }
    if (s_now_sub) {
        lv_obj_set_style_text_color(s_now_sub, UI_ACCENT_SOFT, 0);
    }
    if (s_now_lbl) {
        lv_obj_set_style_text_color(s_now_lbl, UI_TEXT, 0);
    }
    if (s_now_btns) {
        const uint32_t n = lv_obj_get_child_count(s_now_btns);
        if (n > 0) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 0), UI_TILE_SWITCH, UI_ACCENT_SOFT);
        }
        if (n > 1) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 1), UI_SURFACE, UI_TEXT);
        }
        if (n > 2) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 2), UI_ACCENT, UI_ON_TILE);
        }
        if (n > 3) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 3), UI_SURFACE, UI_TEXT);
        }
        if (n > 4) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 4), UI_SURFACE, UI_TEXT);
        }
        if (n > 5) {
            paint_round_btn(lv_obj_get_child(s_now_btns, 5), UI_CAPSULE_OFF, UI_TEXT_SUB);
        }
    }
    if (s_mini_fab) {
        lv_obj_set_style_bg_color(s_mini_fab, UI_TILE_SWITCH, 0);
        lv_obj_set_style_bg_color(s_mini_fab, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(s_mini_fab, UI_ACCENT, 0);
        lv_obj_set_style_shadow_color(s_mini_fab, UI_ACCENT, 0);
        if (s_mini_ic) {
            lv_obj_set_style_text_color(s_mini_ic, UI_ACCENT_SOFT, 0);
        }
    }
    if (s_have_env && s_env && s_card_list) {
        fill_list();
        layout_skill_layers();
    }
}

void raise_skill_layers(void)
{
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_scrim);
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_card);
    }
    /* 宽屏 now 贴底：条必须在卡之上，否则重叠时点不到暂停；卡高已为条让位 */
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_now_bar);
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(s_mini_fab);
    }
}

void nodelio_skill_result_raise(void)
{
    /* 播放条/曲库在 lv_layer_top 与 s_root 同级；s_root 被置顶后必须再抬一次，
     * 且收起曲库后 s_skill_ui_active=false，不能再靠该旗标判断。 */
    raise_skill_layers();
    ensure_chrome_order();
}

/** 曲库或播放条可见时挡住下层应用页，避免点穿卡片 */
void sync_player_block_scrim(void)
{
    if (!s_scrim) {
        return;
    }
    const bool card_up = s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    const bool bar_up = s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
    const bool wide = nodelio_display_profile()->nav_width != 0;
    if (!s_minimized && (card_up || (bar_up && wide))) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_scrim, wide ? SKILL_SCRIM_OPA : LV_OPA_COVER, 0);
    } else if (!card_up) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
}

void set_drawer_for_skill(bool skill_on)
{
    s_skill_ui_active = skill_on;
    if (skill_on) {
        nl_ov_raise_chrome();
        return;
    }
    nl_ov_restore_ai_drawer_if_session();
}

void clear_ttl(void)
{
    if (s_ttl_tmr) {
        lv_timer_del(s_ttl_tmr);
    }
    s_ttl_tmr = nullptr;
}

static void on_ttl(lv_timer_t *t)
{
    (void)t;
    s_ttl_tmr = nullptr;
    /* 播歌中勿自动关列表：ttl_sec=300 常撞上首曲尾声，表现为「列表突然没了又接着播」 */
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        return;
    }
    if (!s_env) {
        nodelio_skill_result_hide();
        return;
    }
    /* 曲库空闲超时：只收起卡片，保留歌单，点播放条「点此打开列表」可再开 */
    if (strcmp(s_env->card_type, "music_list") == 0 && s_env->track_n > 0) {
        close_list_keep_player();
        return;
    }
    /* DSH/新闻等 list：超时关卡，否则占屏无法息屏 */
    ESP_LOGI(TAG, "ttl expired — close card app=%s", s_env->app_id);
    nodelio_skill_result_hide();
}

void restart_skill_ttl(void)
{
    clear_ttl();
    if (!s_env) {
        return;
    }
    int ttl = s_env->ttl_sec;
    if (ttl <= 0 && nl_workbench_app(s_env->app_id)) {
        ttl = DSH_IDLE_TTL_SEC_DEFAULT;
    }
    if (ttl <= 0) {
        return;
    }
    s_ttl_tmr = lv_timer_create(on_ttl, (uint32_t)ttl * 1000, nullptr);
    if (s_ttl_tmr) {
        lv_timer_set_repeat_count(s_ttl_tmr, 1);
    }
}

static void request_reshuffle(void);

/** 曲库来源：0=在线曲库 1=内存卡（定义在下方来源切换段）。 */
static int s_library_source;

/** track | live | episode；无 env 时按 track */
const char *env_media_kind(void)
{
    if (!s_have_env || !s_env || !s_env->media_kind[0]) {
        return "track";
    }
    return s_env->media_kind;
}

bool env_media_is_live(void)
{
    return strcmp(env_media_kind(), "live") == 0;
}

void close_list_keep_player(void)
{
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    reset_obj_anim_style(s_card);
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        if (s_now_bar) {
            lv_obj_clear_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        }
        apply_now_media_chrome(nodelio_media_is_playing(), nodelio_media_is_paused(), false);
        /* 保留遮罩：仅播放条时也挡住应用列表，避免点穿 */
        sync_player_block_scrim();
        set_drawer_for_skill(false);
        layout_skill_layers();
        raise_skill_layers();
        ensure_chrome_order();
        return;
    }
    nodelio_skill_result_hide();
}

static void on_close_card(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* X：关列表；在播则留播放条，未播则全关 */
    close_list_keep_player();
}

static void on_scrim_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    /* 检查点击是否在顶栏区域，如果是则不处理（让点击穿透到 status_bar） */
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    if (p.y < NL_UI_STATUS_H) {
        return;
    }
    /* 点遮罩：有内容可恢复则收成悬浮球，否则关掉 */
    if (nodelio_media_is_playing() || nodelio_media_is_paused() ||
        (s_have_env && s_env && s_env->track_n > 0)) {
        minimize_to_fab();
    } else {
        nodelio_skill_result_hide();
    }
}

static void on_reopen_list(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    reopen_list();
}

static void on_reshuffle(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (s_have_env && s_env && strcmp(s_env->card_type, "calendar_weather") == 0) {
        nl_ov_apps_refresh_daily();
        return;
    }
    /* DSH：刷新按钮回到/重拉进度列表（详情页也走这里） */
    if (s_have_env && s_env && nl_workbench_app(s_env->app_id)) {
        dsh_refresh_list();
        return;
    }
    /* 内存卡曲库：刷新=重新扫卡（可能刚拷进新歌） */
    if (s_have_env && s_env && strcmp(s_env->card_type, "music_list") == 0 &&
        s_library_source == 1) {
        nodelio_skill_request_sd_library(false);
        return;
    }
    request_reshuffle();
}

/** 曲库双列紧凑；其它 list 默认单列舒适（density=compact 仍双列） */
static bool list_use_comfortable(void)
{
    if (strcmp(s_env->card_type, "music_list") == 0) {
        return false;
    }
    if (strcmp(s_env->density, "compact") == 0) {
        return false;
    }
    /* comfortable / 空 / 其它 → 单列 */
    return true;
}

static int music_end_sz(void);

/** 紧凑行一格给歌名/标题至少留这么宽，否则双列只会截成「走…」。 */
#define SKILL_COMPACT_TEXT_MIN 96

static int list_cols(void)
{
    if (list_use_comfortable()) {
        return SKILL_LIST_COLS;
    }
    /* 双列只在文字还够宽时成立。方屏 480（卡 412）双列每格约 200px，
     * 扣掉序号圆标、播放钮和内边距只剩 70px 出头，歌名被截成两个字。
     * 宽屏（卡 560+）仍保持双列。 */
    const bool music = s_env && strcmp(s_env->card_type, "music_list") == 0;
    const int mark = music ? music_end_sz() : 22;
    const int pad_hor = music ? 10 : 6;
    const int gap = music ? 10 : 6;
    const int play_w = music ? mark : SKILL_CTRL_SZ;
    const int cell_gap = 6; /* 与 fill_list 里 line 的 pad_column 一致 */
    const int per_cell = (skill_panel_w() - cell_gap * (SKILL_PAGE_COLS - 1)) / SKILL_PAGE_COLS;
    const int text_w = per_cell - pad_hor * 2 - gap - mark - (gap + play_w);
    if (text_w < SKILL_COMPACT_TEXT_MIN) {
        return SKILL_LIST_COLS;
    }
    return SKILL_PAGE_COLS;
}

/** 曲库左右端圆标：不小于播放钮，并随字号加大，避免大字档把序号裁进 22px 盒。 */
static int music_end_sz(void)
{
    int sz = nl_ov_ui_font_line_h() + 16;
    if (sz < SKILL_CTRL_SZ) {
        sz = SKILL_CTRL_SZ;
    }
    if (sz > 56) {
        sz = 56;
    }
    return sz;
}

void paint_music_mark(lv_obj_t *lbl, bool playing, int index)
{
    if (!lbl) {
        return;
    }
    lv_obj_t *pod = lv_obj_get_parent(lbl);
    if (playing) {
        lv_obj_set_style_text_font(lbl, &font_material_symbols_30_4, 0);
        lv_label_set_text(lbl, MATERIAL_SYMBOLS_HEADPHONES);
        lv_obj_set_style_text_color(lbl, UI_ON_TILE, 0);
        if (pod) {
            lv_obj_set_style_bg_color(pod, UI_ACCENT, 0);
        }
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", index + 1);
        nl_ov_set_font(lbl);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_color(lbl, UI_TEXT_SUB, 0);
        if (pod) {
            lv_obj_set_style_bg_color(pod, UI_CAPSULE_OFF, 0);
        }
    }
}

static int list_row_h(void)
{
    if (list_use_comfortable()) {
        const bool dsh = s_env && s_env->app_id[0] && nl_workbench_app(s_env->app_id);
        if (!dsh) {
            return SKILL_LIST_ROW_H;
        }
        /* 标题一行 + 副文两行 + 上下 pad，固定 68 会把标题裁进圆角 */
        const int font_h = nl_ov_ui_font_line_h();
        int lsp = nl_ov_ui_line_box() - font_h;
        if (lsp < 0) {
            lsp = 0;
        }
        int h = 16 + font_h + 3 + (font_h * 2 + lsp) + 6;
        if (h < SKILL_LIST_ROW_H) {
            h = SKILL_LIST_ROW_H;
        }
        return h;
    }
    const int font_h = nl_ov_ui_font_line_h();
    int lsp = nl_ov_ui_line_box() - font_h;
    if (lsp < 0) {
        lsp = 0;
    }
    /* 非曲库紧凑行仍按标题 + 两行简介留高 */
    if (!s_env || strcmp(s_env->card_type, "music_list") != 0) {
        int h = font_h + 4 + (font_h * 2 + lsp) + 8;
        if (h < SKILL_MUSIC_ROW_H) {
            h = SKILL_MUSIC_ROW_H;
        }
        return h;
    }
    /* 曲库一行歌名 + 一行艺人。行高取文字块与圆标的较大者，大字档只增高不留空行。 */
    int gap = lsp;
    if (gap < 2) {
        gap = 2;
    }
    if (gap > 6) {
        gap = 6;
    }
    const int text_h = font_h + gap + font_h;
    const int mark = music_end_sz();
    int inner = text_h > mark ? text_h : mark;
    int h = inner + 12;
    if (h < SKILL_MUSIC_ROW_H) {
        h = SKILL_MUSIC_ROW_H;
    }
    return h;
}

/** 曲库按卡内剩余高度算本页行数，避免 now_playing 让位后末行被裁 */
static int list_page_rows(void)
{
    if (list_use_comfortable()) {
        return SKILL_LIST_ROWS;
    }
    const int row_h = list_row_h();
    const int gap = 4;
    const int hdr_budget = nl_ov_ui_font_line_h() * 2 + 12;
    int chrome = SKILL_LIST_CHROME_PAD;
    if (skill_player_docked()) {
        chrome += SKILL_NOW_H + 6;
    }
    const int list_avail =
        skill_card_h_max() - chrome -
        (hdr_budget > SKILL_LIST_HDR_BUDGET ? hdr_budget : SKILL_LIST_HDR_BUDGET);
    int fit = SKILL_PAGE_ROWS;
    if (list_avail > 0 && row_h + gap > 0) {
        fit = (list_avail + gap) / (row_h + gap);
    }
    if (fit < 2) {
        fit = 2;
    }
    if (fit > SKILL_PAGE_ROWS) {
        fit = SKILL_PAGE_ROWS;
    }
    return fit;
}

static int list_page_size(void)
{
    /* 曲库/电台：一次铺全量 + 指滑，避免矮屏分页估行仍裁末行 */
    if (list_use_comfortable() || strcmp(s_env->card_type, "music_list") == 0) {
        return s_env->track_n > 0 ? s_env->track_n : (SKILL_LIST_COLS * SKILL_LIST_ROWS);
    }
    return list_cols() * list_page_rows();
}

/** 播放条显隐改卡高后重排曲库，并跳到当前在播页 */
void reflow_music_list_if_open(void)
{
    if (!s_have_env || !s_env || !s_card || !s_card_list) {
        return;
    }
    if (lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (strcmp(s_env->card_type, "music_list") != 0) {
        return;
    }
    const int ps = list_page_size();
    if (ps > 0 && s_play_idx >= 0) {
        s_list_page = s_play_idx / ps;
    }
    fill_list();
}

static int list_page_count(void)
{
    if (s_env->track_n <= 0) {
        return 1;
    }
    const int ps = list_page_size();
    return (s_env->track_n + ps - 1) / ps;
}

void set_hdr_mid_icon(const char *sym)
{
    if (!s_hdr_mid_btn || !sym) {
        return;
    }
    lv_obj_t *ic = lv_obj_get_child(s_hdr_mid_btn, 0);
    if (ic) {
        lv_label_set_text(ic, sym);
    }
}

/** 列表翻页（曲库等）；新闻收成悬浮球。DSH 详情勿用此钮（易与刷新混淆）。 */
static void on_hdr_mid(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_env) {
        return;
    }
    if (nl_workbench_app(s_env->app_id)) {
        nl_env_invoke_action_t action = {};
        nl_workbench_status_id(s_env->app_id, action.app_id, sizeof(action.app_id));
        snprintf(action.title, sizeof(action.title), "选择电脑");
        snprintf(action.params_json, sizeof(action.params_json), "{\"select_computer\":true}");
        nodelio_panel_ui_clear_dsh_arm();
        dsh_clear_ui_phase();
        stop_dsh_detail_poll();
        run_invoke_action(&action);
        return;
    }
    if (strcmp(s_env->card_type, "news_list") == 0) {
        minimize_to_fab();
        return;
    }
    /* DSH 详情：顶栏中间钮已隐藏；若仍点到则回列表 */
    if (s_env->detail_body && s_env->detail_body[0] && nl_workbench_app(s_env->app_id)) {
        dsh_refresh_list();
        return;
    }
    /* 内存卡多批：中间钮=下一批（翻页对全量滚动的曲库无意义） */
    if (strcmp(s_env->card_type, "music_list") == 0 && s_library_source == 1 &&
        nodelio_sd_batch_count() > 1) {
        if (!nodelio_skill_sd_next_batch()) {
            nl_ov_toast("没有下一批");
            return;
        }
        Board::GetInstance().NoteUserActivity();
        return;
    }
    const int pages = list_page_count();
    if (pages <= 1) {
        return;
    }
    s_list_page = (s_list_page + 1) % pages;
    fill_list();
}

void reopen_list(void)
{
    if (s_env->track_n <= 0 || !s_card) {
        nl_ov_toast("暂无曲目列表");
        return;
    }
    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    s_minimized = false;
    hide_mini_fab();
    s_have_env = true;
    const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
    if (s_env->bridge_name[0])
        lv_label_set_text_fmt(s_card_title, "%s · %s", s_env->bridge_name, title);
    else
        lv_label_set_text(s_card_title, title);
    fill_list();
    set_drawer_for_skill(true);
    if (s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        anim_show_obj(s_scrim, false);
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    anim_show_obj(s_card, true);
    apply_now_media_chrome(true, false, true);
    nl_ov_apps_notify_skill_open(true);
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
}

struct reshuffle_job {
    nodelio_nvs_config_t cfg;
    char skill_id[NL_ENV_STR];
};
static void reshuffle_worker(void* arg) {
    auto* job = static_cast<reshuffle_job*>(arg);
    char base[160];
    nl_url_strip_base(job->cfg.server_url, base, sizeof(base));
    const char* sid = job->skill_id[0] ? job->skill_id : "music.search";
    if (strcmp(sid, "music_search") == 0) {
        sid = "music.search";
    }
    char url[360];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, job->cfg.device_id,
             sid);
    const int cap = nodelio_caps_max_content_bytes() + 512;
    char* resp = (char*)heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = (char*)malloc((size_t)cap);
    }
    if (!resp) {
        nodelio_panel_ui_toast_defer("内存不足");
        free(job);
        return;
    }
    resp[0] = '\0';
    int status = 0;
    esp_err_t err = nodelio_http_post_json(
        url, job->cfg.device_key, "{\"keyword\":\"\",\"shuffle\":true}", &status, resp, cap - 1);
    if (err == ESP_OK && (status == 200 || status == 201) && resp[0] &&
        strlen(resp) < (size_t)(cap - 2)) {
        nodelio_skill_deliver_json(&job->cfg, resp, false, nullptr, 0);
    } else {
        nodelio_panel_ui_toast_defer("换一批失败");
    }
    free(resp);
    free(job);
}

static void request_reshuffle(void) {
    /* 从内存卡来源切回在线曲库时，可能还没展示过任何云端信封 → s_skill_cfg 为空。
     * 此时从 NVS 缓存补一次，否则「在线曲库」chip 会误报「未配置服务器」。 */
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_cfg_for_http(&s_skill_cfg);
    }
    if (!s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("未配置服务器");
        return;
    }
    auto* job = static_cast<reshuffle_job*>(calloc(1, sizeof(reshuffle_job)));
    if (!job) {
        nl_ov_toast("内存不足");
        return;
    }
    job->cfg = s_skill_cfg;
    /* 内存卡曲库的 app_id 是本地 music.sd，服务端没有这个技能：
     * 从「内存卡」切回「在线曲库」时必须发 music.search。 */
    const char *sid = s_env && s_env->app_id[0] ? s_env->app_id : "music.search";
    if (strcmp(sid, "music.sd") == 0) {
        sid = "music.search";
    }
    snprintf(job->skill_id, sizeof(job->skill_id), "%s", sid);
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive, reshuffle_worker, job, [](void* p) { free(p); },
            nodelio_panel_begin_skill_view(), "reshuffle")) {
        free(job);
        nl_ov_toast("换一批失败");
        return;
    }
    nl_ov_toast("换一批…");
}

static void on_play_row(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* DSH 列表：点行看末轮全文；有播放 URL 的仍走点播 */
    if (s_have_env && s_env && nl_workbench_app(s_env->app_id) &&
        strcmp(s_env->card_type, "music_list") != 0) {
        if (idx >= 0 && idx < s_env->track_n && !s_env->tracks[idx].playback_url[0]) {
            if (strcmp(s_env->tracks[idx].id, "__new__") == 0 && s_env->tracks[idx].invoke_n > 0) {
                run_invoke_action(&s_env->tracks[idx].invokes[0]);
            } else {
                dsh_fetch_session_detail(idx);
            }
            return;
        }
    }
    play_at(idx);
}

/** JSON 字符串转义（路径反斜杠）；out 须大于 in*2。 */
void json_escape_str(char *out, size_t out_len, const char *in)
{
    if (!out || out_len == 0) {
        return;
    }
    size_t o = 0;
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (const char *p = in; *p && o + 2 < out_len; ++p) {
        if (*p == '\\' || *p == '"') {
            out[o++] = '\\';
        }
        if ((unsigned char)*p < 0x20) {
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/** user_data = (track_idx << 8) | action_idx；走 Host invoke，禁止直连桥。 */
void run_invoke_action(const nl_env_invoke_action_t *ia)
{
    if (!ia) {
        return;
    }
    if (ia->params_json[0] && strstr(ia->params_json, "\"dismiss\"")) {
        nodelio_skill_result_hide();
        return;
    }
    /* DSH 派活/续聊：本地武装，不 HTTP（等语音「让它…」） */
    if (ia->params_json[0] && strstr(ia->params_json, "\"arm\"")) {
        char ws_buf[200] = {0};
        char sess_buf[80] = {0};
        char bridge_buf[64] = {}, provider_buf[16] = {};
        cJSON *root = cJSON_Parse(ia->params_json);
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "arm"))) {
            cJSON_Delete(root);
            nl_ov_toast("无效的续聊操作");
            return;
        }
        if (root) {
            const cJSON *jw = cJSON_GetObjectItemCaseSensitive(root, "workspace");
            const cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "session");
            const cJSON* jb = cJSON_GetObjectItemCaseSensitive(root, "bridge_id");
            const cJSON* jp = cJSON_GetObjectItemCaseSensitive(root, "provider");
            if ((cJSON_IsString(jb) && strlen(jb->valuestring) >= sizeof(bridge_buf)) ||
                (cJSON_IsString(jp) && strlen(jp->valuestring) >= sizeof(provider_buf))) {
                cJSON_Delete(root);
                nl_ov_toast("电脑标识过长，未发送");
                return;
            }
            snprintf(bridge_buf, sizeof(bridge_buf), "%s",
                     cJSON_IsString(jb) ? jb->valuestring : "");
            snprintf(
                provider_buf, sizeof(provider_buf), "%s",
                cJSON_IsString(jp) ? jp->valuestring : (s_env ? s_env->workbench_provider : ""));

            if ((cJSON_IsString(jw) && strlen(jw->valuestring) >= sizeof(ws_buf)) ||
                (cJSON_IsString(js) && strlen(js->valuestring) >= sizeof(sess_buf))) {
                cJSON_Delete(root);
                nl_ov_toast("工作区或会话标识过长，无法续聊");
                return;
            }
            if (cJSON_IsString(jw) && jw->valuestring) {
                snprintf(ws_buf, sizeof(ws_buf), "%s", jw->valuestring);
            }
            if (cJSON_IsString(js) && js->valuestring) {
                snprintf(sess_buf, sizeof(sess_buf), "%s", js->valuestring);
            }
            cJSON_Delete(root);
        }
        /* 已续聊同一会话：再点＝取消 */
        if (s_dsh_phase == DSH_UI_ARMED && sess_buf[0] && s_dsh_focus_sess[0] &&
            strcmp(sess_buf, s_dsh_focus_sess) == 0) {
            nodelio_panel_ui_clear_dsh_arm();
            dsh_clear_ui_phase();
            nl_ov_toast("已取消续聊");
            dsh_paint_status_strip();
            if (s_card_sub && s_have_env && s_env && nl_workbench_app(s_env->app_id) &&
                s_env->track_n > 0) {
                char sub[72];
                snprintf(sub, sizeof(sub), "共 %d 条 · 点卡片看回复 · 「续聊」开麦", s_env->track_n);
                lv_label_set_text(s_card_sub, sub);
            }
            return;
        }
        nodelio_panel_ui_arm_workbench_target(
            ia->instance_id[0] ? ia->instance_id : ia->app_id, ws_buf[0] ? ws_buf : nullptr,
            sess_buf[0] ? sess_buf : nullptr, bridge_buf, provider_buf);
        if (!nodelio_panel_ui_dsh_is_armed()) {
            nl_ov_toast("缺少工作区或会话，无法续聊");
            return;
        }
        Board::GetInstance().NoteUserActivity();
        restart_skill_ttl();
        /* 续聊提示只显示文字，保持麦克风等待正文。 */
        const bool card_open =
            s_skill_ui_active && s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (sess_buf[0]) {
            nl_ov_toast(card_open ? "请说指令，发送前会确认" : "已续聊，请说指令（发送前确认）");
            dsh_enter_armed(ws_buf, sess_buf);
        } else if (ws_buf[0]) {
            nl_ov_toast("已选工作区，请说指令（发送前确认）");
            dsh_enter_armed(ws_buf, nullptr);
        } else {
            nl_ov_toast("请说指令，发送前会确认");
            dsh_enter_armed(nullptr, nullptr);
        }
        /* 续聊下发任务 = 自由说 → 必须云端 ASR（STT 再走本地 armed→dsh.task）。
         * 本地 MultiNet 只有固定口令，听不懂「帮我提交版本」这类指令。
         * 列表/详情已打开时保持卡片：收成悬浮球会被当成「没点到续聊、整卡缩小」。
         * 仅无卡时收起，给云端 ASR 腾一点内部堆。 */
        if (!card_open) {
            minimize_to_fab();
        } else {
            const size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (dram_largest < 32 * 1024) {
                ESP_LOGW(TAG, "dsh arm keep card open, low dram_largest=%u",
                         (unsigned)dram_largest);
            }
        }
        Application::GetInstance().Schedule([]() {
            nodelio_offline_cmd_yield_for_cloud_audio();
            nodelio_overlay_show_chat();
        });
        return;
    }
    const char *skill = ia->instance_id[0] ? ia->instance_id : ia->app_id;
    if (!skill[0] || !s_skill_cfg.server_url[0] || !s_skill_cfg.device_id[0]) {
        nl_ov_toast("无法执行");
        return;
    }
    struct invoke_arg {
        nodelio_nvs_config_t cfg;
        char skill_id[NL_ENV_STR];
        char body[NL_ENV_PARAMS];
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        nl_ov_toast("内存不足");
        return;
    }
    arg->cfg = s_skill_cfg;
    snprintf(arg->skill_id, sizeof(arg->skill_id), "%s", skill);
    snprintf(arg->body, sizeof(arg->body), "%s", ia->params_json[0] ? ia->params_json : "{}");
    nl_ov_toast(ia->title[0] ? ia->title : "执行中…");
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive,
            [](void* p) {
                auto *a = static_cast<invoke_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, a->cfg.device_id,
                         a->skill_id);
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
                    const int io_ms = nl_workbench_app(a->skill_id) ? DSH_HTTP_IO_MS : 10000;
                    esp_err_t err =
                        nodelio_http_post_json_ms(url, a->cfg.device_key, a->body, &status, resp, cap - 1, io_ms);
                    if (err == ESP_OK && resp[0] &&
                        (status == 200 || status == 201 ||
                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                        char spoken[NL_ENV_SPOKEN];
                        spoken[0] = '\0';
                        nodelio_skill_deliver_json(&a->cfg, resp, false, spoken, sizeof(spoken));
                        if (nl_workbench_app(a->skill_id) && spoken[0]) {
                            nodelio_panel_ui_announce_spoken(spoken);
                        }
                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                        if (nl_workbench_mutation(a->skill_id, a->body)) {
                            nodelio_panel_ui_toast_defer("结果未确认，请先查看任务，勿重复发送");
                        } else {
                            nl_ov_toast_invoke_unreachable(a->skill_id);
                        }
                    } else {
                        nodelio_panel_ui_toast_defer("执行失败");
                    }
                    free(resp);
                }
                free(a);
            },
            arg, [](void* p) { free(p); }, nodelio_panel_begin_skill_view(), nullptr, true)) {
        free(arg);
        nl_ov_toast("无法执行");
    }
}

void on_invoke_action(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const intptr_t packed = (intptr_t)lv_event_get_user_data(e);
    const int track_idx = (int)(packed >> 8);
    const int act_idx = (int)(packed & 0xff);
    if (track_idx < 0 || track_idx >= s_env->track_n || act_idx < 0 ||
        act_idx >= s_env->tracks[track_idx].invoke_n) {
        return;
    }
    run_invoke_action(&s_env->tracks[track_idx].invokes[act_idx]);
}

/** 「第一个 / 第2个 / 选三」→ 1-based；失败返回 0 */
int parse_ordinal_zh(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return 0;
    }
    static const char *const kCn[] = {"一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十"};
    static const int kVal[] = {1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const char *p = strstr(phrase, "第");
    if (p) {
        p += 3; /* UTF-8 第 */
        if (p[0] >= '1' && p[0] <= '9') {
            int n = p[0] - '0';
            if (p[1] >= '0' && p[1] <= '9') {
                n = n * 10 + (p[1] - '0');
            }
            return n;
        }
        for (size_t i = 0; i < sizeof(kCn) / sizeof(kCn[0]); i++) {
            if (strncmp(p, kCn[i], strlen(kCn[i])) == 0) {
                return kVal[i];
            }
        }
    }
    /* 选一 / 要第二个 */
    for (size_t i = 0; i < sizeof(kCn) / sizeof(kCn[0]); i++) {
        if (strstr(phrase, kCn[i]) &&
            (strstr(phrase, "选") || strstr(phrase, "要") || strstr(phrase, "打开") ||
             strstr(phrase, "进"))) {
            return kVal[i];
        }
    }
    return 0;
}

void invoke_filter_workspace(const nl_env_filter_t *f)
{
    if (!f || !f->workspace[0] || !s_env) {
        return;
    }
    nl_env_invoke_action_t ia;
    memset(&ia, 0, sizeof(ia));
    snprintf(ia.id, sizeof(ia.id), "filter");
    snprintf(ia.title, sizeof(ia.title), "%s", f->title[0] ? f->title : "工作区");
    snprintf(ia.app_id, sizeof(ia.app_id), "%s",
             s_env->app_id[0] ? s_env->app_id : "dsh.status");
    cJSON* params = cJSON_CreateObject();
    const bool valid =
        params && cJSON_AddStringToObject(params, "workspace", f->workspace) &&
        cJSON_AddStringToObject(params, "bridge_id", s_env->bridge_id) &&
        cJSON_AddStringToObject(params, "provider", s_env->workbench_provider) &&
        cJSON_PrintPreallocated(params, ia.params_json, sizeof(ia.params_json), false);
    cJSON_Delete(params);
    if (!valid) {
        nl_ov_toast("工作区参数无法发送，请检查路径长度");
        return;
    }
    nodelio_panel_ui_arm_dsh(f->workspace, nullptr);
    dsh_clear_ui_phase();
    run_invoke_action(&ia);
}

static void on_filter_chip(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (!s_env) {
        return;
    }
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_env->filter_n) {
        return;
    }
    const nl_env_filter_t *f = &s_env->filters[idx];
    if (f->selected) {
        return;
    }
    /* 换工作区：取消续聊武装，避免派错会话 */
    if (nodelio_panel_ui_dsh_is_armed() || s_dsh_phase != DSH_UI_IDLE) {
        nodelio_panel_ui_clear_dsh_arm();
        dsh_clear_ui_phase();
    }
    invoke_filter_workspace(f);
}

/* ---------------- 曲库来源：在线曲库 / 内存卡 ---------------- */

static bool s_sd_busy = false;

int nodelio_skill_library_source(void)
{
    return s_library_source;
}

/** 卡内曲目 → music_list 信封。调用方在 Job 线程里跑（会碰 FATFS，禁止在 LVGL 回调里做）。 */
static nl_content_envelope_t *build_sd_envelope(bool auto_play)
{
    nl_content_envelope_t *env = static_cast<nl_content_envelope_t *>(
        heap_caps_calloc(1, sizeof(nl_content_envelope_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!env) {
        env = static_cast<nl_content_envelope_t *>(calloc(1, sizeof(nl_content_envelope_t)));
    }
    if (!env) {
        return nullptr;
    }
    snprintf(env->status, sizeof(env->status), "ok");
    snprintf(env->app_id, sizeof(env->app_id), "music.sd");
    snprintf(env->card_type, sizeof(env->card_type), "music_list");
    snprintf(env->display_title, sizeof(env->display_title), "内存卡曲库");
    snprintf(env->media_kind, sizeof(env->media_kind), "track");
    env->auto_play = auto_play;
    int total = 0;
    const int n = nodelio_sd_fill_envelope(env, &total);
    if (n <= 0) {
        snprintf(env->spoken, sizeof(env->spoken),
                 total > 0 ? "内存卡曲目读取失败" : "内存卡里没有找到歌曲，请拷贝 MP3/WAV/M4A 到卡的任意文件夹");
    } else if (total > n) {
        /* 只报本批，避免模型以为整张卡只有这些 */
        snprintf(env->spoken, sizeof(env->spoken), "内存卡共%d首，本批%d首，可说播放第几首", total, n);
    } else {
        snprintf(env->spoken, sizeof(env->spoken), "内存卡共%d首，可说播放第几首", n);
    }
    return env;
}

static void sd_open_worker(void *p)
{
    const bool auto_play = (reinterpret_cast<intptr_t>(p) != 0);
    nodelio_sd_mount();
    if (nodelio_sd_is_mounted()) {
        (void)nodelio_sd_rescan();
    }
    if (!nodelio_sd_is_mounted()) {
        s_sd_busy = false;
        nodelio_panel_ui_toast_defer("未检测到内存卡");
        return;
    }
    nl_content_envelope_t *env = build_sd_envelope(auto_play);
    s_sd_busy = false;
    if (!env) {
        nodelio_panel_ui_toast_defer("内存不足");
        return;
    }
    /* 来源以信封信源为准（show() 会按 app_id 回填 s_library_source） */
    nodelio_skill_result_show(nullptr, env, auto_play);
}

void nodelio_skill_request_sd_library(bool auto_play)
{
    if (!nodelio_sd_available()) {
        nl_ov_toast("本机没有内存卡");
        return;
    }
    if (s_sd_busy) {
        nl_ov_toast("正在读取内存卡…");
        return;
    }
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        /* 换来源=换歌单：先停播，避免播放条指向上一批 */
        nodelio_media_stop();
    }
    s_sd_busy = true;
    nl_ov_toast("正在读取内存卡…");
    /* discard：任务被更新的技能视图顶掉时也要解锁，否则「正在读取内存卡…」会一直卡住 */
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive, sd_open_worker,
            reinterpret_cast<void *>(auto_play ? 1 : 0),
            [](void *) { s_sd_busy = false; }, nodelio_panel_begin_skill_view(), "sd_library")) {
        s_sd_busy = false;
        nl_ov_toast("内存卡读取失败");
    }
}

bool nodelio_skill_sd_next_batch(void)
{
    if (s_library_source != 1 || nodelio_sd_batch_count() <= 1) {
        return false;
    }
    nodelio_sd_next_batch();
    nl_content_envelope_t *env = build_sd_envelope(false);
    if (!env) {
        return false;
    }
    nodelio_skill_result_show(nullptr, env, false);
    return true;
}

static void on_src_chip(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const int want = (int)(intptr_t)lv_event_get_user_data(e);
    if (want == s_library_source) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
    restart_skill_ttl();
    if (want == 1) {
        nodelio_skill_request_sd_library(false);
        return;
    }
    /* 回在线曲库：重新 invoke 云端 music.search（与「打开曲库」同一路径）。
     * 来源标记等回包到达后由 show() 按 app_id 更新，失败时仍停在内存卡，不会错标。 */
    request_reshuffle();
}

/** 曲库卡顶部的来源切换：在线曲库 / 内存卡。仅 music_list 显示。 */
static void fill_source_bar(void)
{
    if (!s_src_bar) {
        return;
    }
    lv_obj_clean(s_src_bar);
    const bool music = s_env && strcmp(s_env->card_type, "music_list") == 0;
    if (!music || !nodelio_sd_available()) {
        lv_obj_add_flag(s_src_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_src_bar, 0);
        return;
    }
    lv_obj_clear_flag(s_src_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(s_src_bar, LV_SIZE_CONTENT);
    lv_obj_set_height(s_src_bar, 40);
    lv_obj_move_to_index(s_src_bar, 0);
    static const char *const kNames[2] = {"在线曲库", "内存卡"};
    for (int i = 0; i < 2; i++) {
        const bool selected = (i == s_library_source);
        lv_obj_t *chip = lv_obj_create(s_src_bar);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, 34);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 16, 0);
        lv_obj_set_style_radius(chip, 17, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        if (selected) {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_ON, 0);
            lv_obj_set_style_border_color(chip, UI_ACCENT_SOFT, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_80, 0);
        } else {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_border_color(chip, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_60, 0);
        }
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, 6);
        lv_obj_add_event_cb(chip, on_src_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(chip);
        nl_ov_set_font(lbl);
        lv_label_set_text(lbl, kNames[i]);
        lv_obj_set_style_text_color(lbl, selected ? UI_ON_TILE : UI_TEXT_SUB, 0);
    }
}

static void fill_filters(void)
{
    if (!s_filter_bar) {
        return;
    }
    lv_obj_clean(s_filter_bar);
    if (!s_env || s_env->filter_n <= 0) {
        lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_filter_bar, 0);
        return;
    }
    lv_obj_clear_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(s_filter_bar, 40);
    for (int i = 0; i < s_env->filter_n; i++) {
        const nl_env_filter_t *f = &s_env->filters[i];
        lv_obj_t *chip = lv_obj_create(s_filter_bar);
        lv_obj_remove_style_all(chip);
        lv_obj_set_height(chip, 36);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 14, 0);
        lv_obj_set_style_radius(chip, 18, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (f->selected) {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_ON, 0);
            lv_obj_set_style_border_color(chip, UI_ACCENT_SOFT, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_80, 0);
        } else {
            lv_obj_set_style_bg_color(chip, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_border_color(chip, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(chip, LV_OPA_60, 0);
        }
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, 8);
        lv_obj_add_event_cb(chip, on_filter_chip, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(chip);
        nl_ov_set_font(lbl);
        lv_label_set_text(lbl, f->title[0] ? f->title : "?");
        lv_obj_set_style_text_color(lbl, f->selected ? UI_ON_TILE : UI_TEXT_SUB, 0);
    }
    /* 不在此处 scroll_to_view：会强制整树 layout，语音/主循环路径易触发 task_wdt */
}

/**
 * 曲库卡前台：「播放第一首 / 放第三首 / 来第2首」→ 在当前列表里点播。
 * 必须本机处理：交云端会重新 invoke music.search，返回的是另一批结果，
 * 「第一首」就不再是用户看到的那一首（用户报告的「又请求一次」问题）。
 */
bool nodelio_skill_try_music_pick_phrase(const char *text)
{
    if (!text || !text[0] || !s_have_env || !s_env) {
        return false;
    }
    if (strcmp(s_env->card_type, "music_list") != 0) {
        return false;
    }
    /* 点歌说法里必须有「首/曲」，否则「第一个」可能是在选应用/工作区 */
    if (strstr(text, "首") == nullptr && strstr(text, "曲") == nullptr &&
        strstr(text, "歌") == nullptr) {
        return false;
    }
    int ord = parse_ordinal_zh(text);
    if (ord <= 0) {
        if (strstr(text, "第一首") || strstr(text, "头一首") || strstr(text, "首个")) {
            ord = 1;
        } else if (strstr(text, "最后一首") || strstr(text, "最后那首")) {
            ord = s_env->track_n;
        }
    }
    if (ord <= 0) {
        return false;
    }
    char title[NL_ENV_TITLE];
    title[0] = '\0';
    {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock || !nodelio_skill_media_play_index(ord, title, sizeof(title))) {
            char tip[48];
            snprintf(tip, sizeof(tip), "列表只有 %d 首", s_env ? s_env->track_n : 0);
            nodelio_panel_ui_toast_defer(tip);
            nodelio_panel_ui_set_voice_hint("没有这个序号");
            return true; /* 已处理：不要让云端再搜一次歌 */
        }
    }
    ESP_LOGI(TAG, "music pick ord=%d title=%s", ord, title);
    {
        char tip[NL_ENV_TITLE + 16];
        snprintf(tip, sizeof(tip), "%s", title[0] ? title : "已点播");
        nodelio_panel_ui_toast_defer(tip);
        nodelio_panel_ui_set_voice_hint(title[0] ? title : "已点播");
    }
    return true;
}

bool nodelio_skill_try_pick_phrase(const char *text)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock || !text || !text[0] || !skill_ui_is_up() || !s_env) {
        return false;
    }
    if (!nl_workbench_app(s_env->app_id)) {
        return false;
    }
    bool resume = false;
    int ord = nodelio_offline_dsh_selection(text, &resume);
    if (ord == 0) {
        return false;
    }
    if (ord < 0) {
        if (!dsh_is_detail_view()) {
            nodelio_panel_ui_announce_spoken("请说续聊第几个，或先打开会话详情。");
            return true;
        }
        ord = 1;
    }
    /* 「第 N 个工作区」→ 顶栏 filters */
    if (s_env->filter_n > 0 && strstr(text, "工作区") != nullptr) {
        if (ord > s_env->filter_n) {
            nl_ov_toast("超出范围");
            nodelio_panel_ui_announce_spoken("没有这个工作区序号，请再说。");
            return true;
        }
        const nl_env_filter_t *f = &s_env->filters[ord - 1];
        ESP_LOGI(TAG, "dsh pick workspace ord=%d title=%s", ord, f->title);
        invoke_filter_workspace(f);
        return true;
    }
    /* 会话列表按播报序号 */
    int selectable[24];
    int n = 0;
    for (int i = 0; i < s_env->track_n && n < 24; i++) {
        if (strcmp(s_env->tracks[i].id, "__workspaces__") == 0 ||
            strcmp(s_env->tracks[i].id, "__new__") == 0) {
            continue;
        }
        selectable[n++] = i;
    }
    if (n <= 0) {
        return false;
    }
    if (ord > n) {
        nl_ov_toast("超出范围");
        nodelio_panel_ui_announce_spoken("没有这个序号，请再说第几个。");
        return true;
    }
    const int ti = selectable[ord - 1];
    if (s_env->workbench_picker) {
        dsh_fetch_session_detail(ti);
        return true;
    }
    if (resume) {
        const auto& track = s_env->tracks[ti];
        for (int ai = 0; ai < track.invoke_n; ++ai) {
            cJSON* params = cJSON_Parse(track.invokes[ai].params_json);
            const bool arm = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "arm"));
            cJSON_Delete(params);
            if (arm) {
                run_invoke_action(&track.invokes[ai]);
                return true;
            }
        }
        nodelio_panel_ui_announce_spoken("这个会话暂不支持续聊。");
        return true;
    }
    /* 「第 N 个」→ 打开详情看回复；续聊请说「续聊」或点按钮（避免误武装 + 主线程重建卡死） */
    ESP_LOGI(TAG, "dsh pick ord=%d track=%d → detail", ord, ti);
    nl_ov_toast("打开详情");
    dsh_fetch_session_detail(ti);
    return true;
}

lv_obj_t *make_text_action_btn(lv_obj_t *parent, const char *title, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, UI_ACCENT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, UI_ACCENT_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 10);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    nl_ov_set_font(lbl);
    lv_label_set_text(lbl, title && title[0] ? title : "执行");
    lv_obj_set_style_text_color(lbl, UI_ON_TILE, 0);
    lv_obj_center(lbl);
    return btn;
}

/** 新结果仅 1 首且可在旧列表中找到 → 保留旧列表只切播，避免「随便」再搜把歌单冲成 1 首。 */
static int find_track_in_env(const nl_content_envelope_t *hay, const nl_env_track_t *needle)
{
    if (!hay || !needle || hay->track_n <= 0) {
        return -1;
    }
    if (needle->playback_url[0]) {
        for (int i = 0; i < hay->track_n; i++) {
            if (hay->tracks[i].playback_url[0] &&
                strcmp(hay->tracks[i].playback_url, needle->playback_url) == 0) {
                return i;
            }
        }
    }
    if (needle->title[0]) {
        for (int i = 0; i < hay->track_n; i++) {
            if (strcmp(hay->tracks[i].title, needle->title) != 0) {
                continue;
            }
            if (needle->artist[0] && hay->tracks[i].artist[0] &&
                strcmp(hay->tracks[i].artist, needle->artist) != 0) {
                continue;
            }
            return i;
        }
    }
    return -1;
}

/** Material Symbols 图标钮（勿用 nl_ov_set_font：普惠字库无符号字形） */
lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym,
                        lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, SKILL_CTRL_SZ, SKILL_CTRL_SZ);
    lv_obj_set_style_radius(btn, SKILL_CTRL_SZ / 2, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &font_material_symbols_30_4, 0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, fg, 0);
    lv_obj_center(ic);
    return btn;
}

/** 播放控制专用大图标钮（提高点击命中率） */
lv_obj_t *make_play_ctrl_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, SKILL_PLAY_CTRL_SZ, SKILL_PLAY_CTRL_SZ);
    lv_obj_set_style_radius(btn, SKILL_PLAY_CTRL_SZ / 2, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 12);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &font_material_symbols_30_4, 0);
    lv_label_set_text(ic, sym);
    lv_obj_set_style_text_color(ic, fg, 0);
    lv_obj_center(ic);
    return btn;
}

void anim_eq_height(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, (lv_coord_t)v);
}

void anim_border_opa(void *var, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_obj_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_obj_ty(void *var, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void anim_obj_tx(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static bool skill_slide_from_right(void)
{
    return nodelio_display_profile()->nav_width != 0;
}

/** from_below=true：宽屏自右侧滑入，方屏自下方滑入（禁止负 translate 盖住顶栏）。 */
void anim_show_obj(lv_obj_t *obj, bool from_below)
{
    if (!obj) {
        return;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_anim_delete(obj, anim_obj_tx);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_x(obj, 0, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
    const bool slide_x = from_below && skill_slide_from_right();
    if (slide_x) {
        lv_obj_set_style_translate_x(obj, SKILL_ANIM_SLIDE_X, 0);
    } else if (from_below) {
        lv_obj_set_style_translate_y(obj, SKILL_ANIM_SLIDE, 0);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_obj_opa);
    lv_anim_start(&a);

    if (from_below) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, obj);
        lv_anim_set_values(&b, slide_x ? SKILL_ANIM_SLIDE_X : SKILL_ANIM_SLIDE, 0);
        lv_anim_set_time(&b, SKILL_ANIM_MS);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, slide_x ? anim_obj_tx : anim_obj_ty);
        lv_anim_start(&b);
    }
}

static void null_skill_widget_ptrs(void)
{
    s_scrim = nullptr;
    s_card = nullptr;
    s_filter_bar = nullptr;
    s_dsh_status = nullptr;
    s_dsh_status_lbl = nullptr;
    for (int i = 0; i < 3; i++) {
        s_dsh_eq[i] = nullptr;
    }
    s_dsh_confirm_bar = nullptr;
    s_dsh_detail_bar = nullptr;
    s_card_list = nullptr;
    s_card_title = nullptr;
    s_card_sub = nullptr;
    s_now_bar = nullptr;
    s_now_eq_wrap = nullptr;
    for (int i = 0; i < SKILL_EQ_N; i++) {
        s_now_eq[i] = nullptr;
    }
    s_now_lbl = nullptr;
    s_now_sub = nullptr;
    s_now_btns = nullptr;
    s_now_pause_btn = nullptr;
    s_now_prev_btn = nullptr;
    s_now_next_btn = nullptr;
    s_mini_fab = nullptr;
    s_mini_ic = nullptr;
    s_hdr = nullptr;
    s_hdr_txt = nullptr;
    s_close_btn = nullptr;
    s_hdr_btns = nullptr;
    s_hdr_mid_btn = nullptr;
    s_news_zoom_bar = nullptr;
    s_news_body_lbl = nullptr;
    s_dsh_focus_row = nullptr;
    s_dsh_pulse_n = 0;
    for (int i = 0; i < SKILL_LIST_ROWS; i++) {
        s_dsh_pulse_rows[i] = nullptr;
    }
    s_dsh_eq_anim_on = false;
    s_dsh_row_pulse_on = false;
    memset(s_row_idx_labels, 0, sizeof(s_row_idx_labels));
    memset(s_row_btns, 0, sizeof(s_row_btns));
}

static void free_skill_widgets(void)
{
    stop_dsh_status_anims();
    stop_now_anims();
    stop_mini_pulse();
    lv_obj_t *scrim = s_scrim;
    lv_obj_t *card = s_card;
    lv_obj_t *bar = s_now_bar;
    lv_obj_t *fab = s_mini_fab;
    if (scrim) {
        lv_anim_delete(scrim, anim_obj_opa);
        lv_anim_delete(scrim, anim_obj_ty);
    }
    if (card) {
        lv_anim_delete(card, anim_obj_opa);
        lv_anim_delete(card, anim_obj_ty);
    }
    if (bar) {
        lv_anim_delete(bar, anim_obj_opa);
        lv_anim_delete(bar, anim_obj_ty);
        lv_anim_delete(bar, anim_border_opa);
    }
    if (fab) {
        lv_anim_delete(fab, anim_obj_opa);
        lv_anim_delete(fab, anim_border_opa);
    }
    null_skill_widget_ptrs();
    s_ui_closing = false;
    free_skill_env();
    s_play_idx = -1;
    s_minimized = false;
    s_news_detail = -1;
    s_news_gen += 1;
    if (s_news_body_heap) {
        heap_caps_free(s_news_body_heap);
        s_news_body_heap = nullptr;
    }
    if (scrim) {
        lv_obj_delete(scrim);
    }
    if (card) {
        lv_obj_delete(card);
    }
    if (bar) {
        lv_obj_delete(bar);
    }
    if (fab) {
        lv_obj_delete(fab);
    }
    set_drawer_for_skill(false);
    layout_skill_layers();
    ensure_chrome_order();
}

static void release_async(void *p)
{
    uint32_t gen = (uint32_t)(uintptr_t)p;
    if (gen != s_close_gen || !s_ui_closing) {
        return;
    }
    free_skill_widgets();
}

static void on_dismiss_done(lv_anim_t *a)
{
    if (!a) {
        return;
    }
    /* 完成回调里不能删正在做动画的对象，丢到下一拍 */
    lv_async_call(release_async, lv_anim_get_user_data(a));
}

static bool slide_out_obj(lv_obj_t *obj, bool with_done)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_anim_delete(obj, anim_obj_tx);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, obj);
    lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&fade, anim_obj_opa);
    lv_anim_start(&fade);

    const bool slide_x = skill_slide_from_right();
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, obj);
    lv_anim_set_values(&slide, 0, slide_x ? SKILL_ANIM_SLIDE_X : SKILL_ANIM_SLIDE);
    lv_anim_set_time(&slide, SKILL_ANIM_MS);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide, slide_x ? anim_obj_tx : anim_obj_ty);
    if (with_done) {
        lv_anim_set_completed_cb(&slide, on_dismiss_done);
        lv_anim_set_user_data(&slide, (void *)(uintptr_t)s_close_gen);
    }
    lv_anim_start(&slide);
    return true;
}

void dismiss_library_and_player(void)
{
    nodelio_panel_begin_skill_view();
    s_close_gen += 1;
    s_ui_closing = true;
    free_skill_env();
    s_play_idx = -1;
    s_minimized = false;
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    stop_now_anims();
    hide_mini_fab();
    set_drawer_for_skill(false);
    nl_ov_apps_notify_skill_open(false);

    lv_obj_t *done_on = nullptr;
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_card;
    } else if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_now_bar;
    } else if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        done_on = s_scrim;
    }
    bool started = false;
    started = slide_out_obj(s_scrim, s_scrim && s_scrim == done_on) || started;
    started = slide_out_obj(s_now_bar, s_now_bar && s_now_bar == done_on) || started;
    started = slide_out_obj(s_card, s_card && s_card == done_on) || started;
    if (!started) {
        free_skill_widgets();
    }
}

void reset_obj_anim_style(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_anim_delete(obj, anim_obj_opa);
    lv_anim_delete(obj, anim_obj_ty);
    lv_anim_delete(obj, anim_obj_tx);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_x(obj, 0, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
}

static void ensure_card(void)
{
    if (s_card) {
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    if (!parent) {
        return;
    }

    const bool wide = nodelio_display_profile()->nav_width != 0;
    s_scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_scrim);
    lv_obj_set_size(s_scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    if (wide) {
        lv_obj_set_width(s_scrim, SKILL_CARD_W + 2 * SKILL_GAP);
        lv_obj_align(s_scrim, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_bg_opa(s_scrim, SKILL_SCRIM_OPA, 0);
    } else {
        lv_obj_set_style_bg_opa(s_scrim, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(s_scrim, UI_BG, 0);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scrim, on_scrim_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);

    s_card = lv_obj_create(parent);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, wide ? SKILL_CARD_W : nodelio_display_profile()->width,
                    wide ? SKILL_CARD_H_MAX : nodelio_display_profile()->height);
    lv_obj_align(s_card, wide ? LV_ALIGN_TOP_MID : LV_ALIGN_TOP_LEFT, 0, wide ? skill_top_y() : 0);
    style_skill_frame();
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_set_style_pad_row(s_card, 8, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hdr = lv_obj_create(s_card);
    s_hdr = hdr;
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hdr_txt = lv_obj_create(hdr);
    s_hdr_txt = hdr_txt;
    lv_obj_remove_style_all(hdr_txt);
    lv_obj_set_flex_grow(hdr_txt, 1);
    lv_obj_set_height(hdr_txt, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(hdr_txt, 0, 0);
    lv_obj_set_style_pad_right(hdr_txt, 12, 0);
    lv_obj_set_flex_flow(hdr_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr_txt, 2, 0);

    s_card_title = lv_label_create(hdr_txt);
    nl_ov_set_font(s_card_title);
    lv_label_set_text(s_card_title, "曲库");
    lv_label_set_long_mode(s_card_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_card_title, lv_pct(100));
    lv_obj_set_height(s_card_title, nl_ov_ui_font_line_h());
    lv_obj_set_style_text_color(s_card_title, UI_TEXT, 0);

    s_card_sub = lv_label_create(hdr_txt);
    nl_ov_set_font(s_card_sub);
    lv_label_set_text(s_card_sub, "");
    lv_label_set_long_mode(s_card_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_card_sub, lv_pct(100));
    lv_obj_set_height(s_card_sub, nl_ov_ui_font_line_h());
    lv_obj_set_style_text_color(s_card_sub, UI_TEXT_MUTED, 0);

    s_hdr_btns = lv_obj_create(hdr);
    lv_obj_remove_style_all(s_hdr_btns);
    lv_obj_set_size(s_hdr_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_hdr_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_hdr_btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_hdr_btns, 6, 0);
    lv_obj_clear_flag(s_hdr_btns, LV_OBJ_FLAG_CLICKABLE);

    /* 来源芯片插在刷新左侧；非曲库时隐藏，不占位 */
    s_src_bar = lv_obj_create(s_hdr_btns);
    lv_obj_remove_style_all(s_src_bar);
    lv_obj_set_width(s_src_bar, LV_SIZE_CONTENT);
    lv_obj_set_height(s_src_bar, 0);
    lv_obj_set_flex_flow(s_src_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_src_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_src_bar, 8, 0);
    lv_obj_clear_flag(s_src_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_src_bar, LV_OBJ_FLAG_HIDDEN);

    make_play_ctrl_btn(s_hdr_btns, UI_CAPSULE_OFF, UI_ACCENT_SOFT, MATERIAL_SYMBOLS_REFRESH, on_reshuffle);
    /* 曲库为下一页；新闻在 fill_news 里改回下箭头收起 */
    s_hdr_mid_btn = make_play_ctrl_btn(s_hdr_btns, UI_CAPSULE_OFF, UI_TEXT_SUB, MATERIAL_SYMBOLS_SKIP_NEXT,
                                       on_hdr_mid);

    lv_obj_t *xbtn = lv_obj_create(s_hdr_btns);
    lv_obj_remove_style_all(xbtn);
    lv_obj_set_size(xbtn, 44, 44);
    lv_obj_set_style_radius(xbtn, wide ? 22 : 0, 0);
    lv_obj_set_style_bg_color(xbtn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(xbtn, LV_OPA_COVER, 0);
    lv_obj_add_flag(xbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(xbtn, 12);
    lv_obj_add_event_cb(xbtn, on_close_card, LV_EVENT_CLICKED, nullptr);
    s_close_btn = xbtn;
    lv_obj_t *xl = lv_label_create(xbtn);
    lv_obj_set_style_text_font(xl, &font_material_symbols_20_4, 0);
    lv_label_set_text(xl, MATERIAL_SYMBOLS_CLOSE);
    lv_obj_set_style_text_color(xl, UI_TEXT_SUB, 0);
    lv_obj_center(xl);

    /* display.filters：横滑 chip（DSH 工作区等） */
    s_filter_bar = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_filter_bar);
    lv_obj_set_width(s_filter_bar, lv_pct(100));
    lv_obj_set_height(s_filter_bar, 0);
    lv_obj_set_flex_flow(s_filter_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_filter_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_filter_bar, 8, 0);
    lv_obj_set_style_pad_hor(s_filter_bar, 2, 0);
    lv_obj_set_scroll_dir(s_filter_bar, LV_DIR_HOR);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_filter_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(s_filter_bar, LV_OBJ_FLAG_HIDDEN);

    /* 续聊状态条：开麦/投递时展开，带 EQ 动画 */
    s_dsh_status = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_dsh_status);
    lv_obj_set_width(s_dsh_status, lv_pct(100));
    lv_obj_set_height(s_dsh_status, 0);
    lv_obj_set_style_radius(s_dsh_status, 10, 0);
    lv_obj_set_style_bg_color(s_dsh_status, UI_TILE_SWITCH, 0);
    lv_obj_set_style_bg_opa(s_dsh_status, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_dsh_status, 10, 0);
    lv_obj_set_style_pad_column(s_dsh_status, 8, 0);
    lv_obj_set_flex_flow(s_dsh_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dsh_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dsh_status, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *eq_wrap = lv_obj_create(s_dsh_status);
    lv_obj_remove_style_all(eq_wrap);
    lv_obj_set_size(eq_wrap, 22, 22);
    lv_obj_set_flex_flow(eq_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eq_wrap, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(eq_wrap, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) {
        s_dsh_eq[i] = lv_obj_create(eq_wrap);
        lv_obj_remove_style_all(s_dsh_eq[i]);
        lv_obj_set_size(s_dsh_eq[i], 3, 6 + i * 3);
        lv_obj_set_style_radius(s_dsh_eq[i], 2, 0);
        lv_obj_set_style_bg_color(s_dsh_eq[i], UI_ACCENT_SOFT, 0);
        lv_obj_set_style_bg_opa(s_dsh_eq[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_dsh_eq[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_dsh_status_lbl = lv_label_create(s_dsh_status);
    nl_ov_set_font(s_dsh_status_lbl);
    lv_label_set_text(s_dsh_status_lbl, "");
    lv_obj_set_style_text_color(s_dsh_status_lbl, UI_TEXT, 0);
    lv_obj_set_flex_grow(s_dsh_status_lbl, 1);
    lv_label_set_long_mode(s_dsh_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_dsh_status_lbl, lv_pct(100));

    s_dsh_confirm_bar = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_dsh_confirm_bar);
    lv_obj_set_width(s_dsh_confirm_bar, lv_pct(100));
    lv_obj_set_height(s_dsh_confirm_bar, 0);
    lv_obj_set_style_pad_column(s_dsh_confirm_bar, 10, 0);
    lv_obj_set_style_pad_ver(s_dsh_confirm_bar, 4, 0);
    lv_obj_set_flex_flow(s_dsh_confirm_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dsh_confirm_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_dsh_confirm_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dsh_confirm_bar, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *send_btn =
            make_text_action_btn(s_dsh_confirm_bar, "发送",
                                 [](lv_event_t *e) {
                                     if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                                         nodelio_panel_ui_dsh_confirm_accept();
                                     }
                                 },
                                 nullptr);
        if (send_btn) {
            lv_obj_set_style_bg_color(send_btn, UI_ACCENT, 0);
            lv_obj_set_flex_grow(send_btn, 1);
            lv_obj_set_height(send_btn, 36);
        }
        lv_obj_t *cancel_btn =
            make_text_action_btn(s_dsh_confirm_bar, "取消",
                                 [](lv_event_t *e) {
                                     if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                                         nodelio_panel_ui_dsh_confirm_reject();
                                     }
                                 },
                                 nullptr);
        if (cancel_btn) {
            lv_obj_set_style_bg_color(cancel_btn, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_color(cancel_btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_t *cl = lv_obj_get_child(cancel_btn, 0);
            if (cl) {
                lv_obj_set_style_text_color(cl, UI_TEXT_SUB, 0);
            }
            lv_obj_set_flex_grow(cancel_btn, 1);
            lv_obj_set_height(cancel_btn, 36);
        }
    }

    s_card_list = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_card_list);
    lv_obj_set_width(s_card_list, lv_pct(100));
    lv_obj_set_flex_grow(s_card_list, 1);
    lv_obj_set_flex_flow(s_card_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_card_list, 4, 0);
    lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);

    /* 详情底栏：续聊固定在窗口底部，不进滚动区 */
    s_dsh_detail_bar = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_dsh_detail_bar);
    lv_obj_set_width(s_dsh_detail_bar, lv_pct(100));
    lv_obj_set_height(s_dsh_detail_bar, 0);
    lv_obj_set_style_pad_top(s_dsh_detail_bar, 6, 0);
    lv_obj_set_style_pad_bottom(s_dsh_detail_bar, 2, 0);
    lv_obj_set_flex_flow(s_dsh_detail_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dsh_detail_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_dsh_detail_bar, 8, 0);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dsh_detail_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);

    if (nodelio_caps_has_surface("now_playing")) {
        s_now_bar = lv_obj_create(parent);
        lv_obj_remove_style_all(s_now_bar);
        lv_obj_set_size(s_now_bar, SKILL_CARD_W, SKILL_NOW_H);
        lv_obj_align(s_now_bar, LV_ALIGN_TOP_MID, 0, skill_top_y());
        lv_obj_set_style_bg_color(s_now_bar, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(s_now_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_now_bar, 16, 0);
        lv_obj_set_style_border_width(s_now_bar, ui_palette_is_dark() ? 1 : 0, 0);
        lv_obj_set_style_border_color(s_now_bar, UI_BORDER, 0);
        lv_obj_set_style_border_opa(s_now_bar, LV_OPA_80, 0);
        ui_style_card(s_now_bar);
        lv_obj_set_style_pad_hor(s_now_bar, 10, 0);
        lv_obj_set_style_pad_ver(s_now_bar, 8, 0);
        lv_obj_set_style_pad_column(s_now_bar, 8, 0);
        lv_obj_set_flex_flow(s_now_bar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        /* 整条可点中（吞掉空隙点击，勿穿透到下层应用卡） */
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);

        s_now_eq_wrap = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(s_now_eq_wrap);
        lv_obj_set_size(s_now_eq_wrap, 28, 28);
        lv_obj_set_style_radius(s_now_eq_wrap, 8, 0);
        lv_obj_set_style_bg_color(s_now_eq_wrap, UI_TILE_SWITCH, 0);
        lv_obj_set_style_bg_opa(s_now_eq_wrap, LV_OPA_COVER, 0);
        lv_obj_set_flex_flow(s_now_eq_wrap, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_eq_wrap, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(s_now_eq_wrap, 4, 0);
        lv_obj_set_style_pad_bottom(s_now_eq_wrap, 4, 0);
        lv_obj_add_flag(s_now_eq_wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_eq_wrap, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        for (int i = 0; i < SKILL_EQ_N; i++) {
            s_now_eq[i] = lv_obj_create(s_now_eq_wrap);
            lv_obj_remove_style_all(s_now_eq[i]);
            lv_obj_set_size(s_now_eq[i], 3, 8 + (i % 3) * 4);
            lv_obj_set_style_radius(s_now_eq[i], 2, 0);
            lv_obj_set_style_bg_color(s_now_eq[i], UI_ACCENT_SOFT, 0);
            lv_obj_set_style_bg_opa(s_now_eq[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(s_now_eq[i], LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *meta = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(meta);
        lv_obj_set_flex_grow(meta, 1);
        lv_obj_set_height(meta, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(meta, 1, 0);
        lv_obj_add_flag(meta, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(meta, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_sub = lv_label_create(meta);
        nl_ov_set_font(s_now_sub);
        lv_label_set_text(s_now_sub, "点此打开列表");
        lv_obj_set_style_text_color(s_now_sub, UI_ACCENT_SOFT, 0);
        lv_obj_add_flag(s_now_sub, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_sub, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_lbl = lv_label_create(meta);
        nl_ov_set_font(s_now_lbl);
        lv_label_set_long_mode(s_now_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(s_now_lbl, SKILL_CARD_W - (wide ? 200 : 330));
        lv_obj_set_style_text_color(s_now_lbl, UI_TEXT, 0);
        lv_obj_add_flag(s_now_lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_now_lbl, on_reopen_list, LV_EVENT_CLICKED, nullptr);

        s_now_btns = lv_obj_create(s_now_bar);
        lv_obj_remove_style_all(s_now_btns);
        lv_obj_set_size(s_now_btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_now_btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_now_btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(s_now_btns, 4, 0);
        lv_obj_clear_flag(s_now_btns, LV_OBJ_FLAG_CLICKABLE);

        /* 麦克风：播中 DRAM 常开不起唤醒，点此暂停并进对话 */
        lv_obj_t *now_mic = make_play_ctrl_btn(s_now_btns, UI_TILE_SWITCH, UI_ACCENT_SOFT, MATERIAL_SYMBOLS_MIC, on_voice_now);
        s_now_prev_btn =
            make_play_ctrl_btn(s_now_btns, UI_SURFACE, UI_TEXT, MATERIAL_SYMBOLS_SKIP_PREVIOUS, on_prev_now);
        s_now_pause_btn =
            make_play_ctrl_btn(s_now_btns, UI_ACCENT, UI_ON_TILE, MATERIAL_SYMBOLS_PAUSE, on_pause_now);
        s_now_next_btn =
            make_play_ctrl_btn(s_now_btns, UI_SURFACE, UI_TEXT, MATERIAL_SYMBOLS_SKIP_NEXT, on_next_now);
        make_play_ctrl_btn(s_now_btns, UI_SURFACE, UI_TEXT, MATERIAL_SYMBOLS_STOP, on_stop_now);
        make_icon_btn(s_now_btns, UI_CAPSULE_OFF, UI_TEXT_SUB, MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN,
                      on_minimize);
        if (!wide) {
            lv_obj_add_flag(now_mic, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_mini_fab = lv_obj_create(parent);
    lv_obj_remove_style_all(s_mini_fab);
    lv_obj_set_size(s_mini_fab, SKILL_MINI_SZ, SKILL_MINI_SZ);
    lv_obj_align(s_mini_fab, LV_ALIGN_BOTTOM_RIGHT, -14, -(NL_UI_TAB_H + 32));
    lv_obj_set_style_radius(s_mini_fab, SKILL_MINI_SZ / 2, 0);
    lv_obj_set_style_bg_color(s_mini_fab, UI_TILE_SWITCH, 0);
    lv_obj_set_style_bg_opa(s_mini_fab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mini_fab, 2, 0);
    lv_obj_set_style_border_color(s_mini_fab, UI_ACCENT, 0);
    lv_obj_set_style_border_opa(s_mini_fab, SKILL_BORDER_OPA, 0);
    lv_obj_set_style_shadow_width(s_mini_fab, 0, 0);
    lv_obj_set_style_shadow_opa(s_mini_fab, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_mini_fab, UI_ACCENT, 0);
    lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_mini_fab, 10);
    lv_obj_add_event_cb(s_mini_fab, on_mini_fab, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_mini_fab, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(s_mini_fab, 1, LV_STATE_PRESSED);

    s_mini_ic = lv_label_create(s_mini_fab);
    lv_obj_set_style_text_font(s_mini_ic, &font_material_symbols_20_4, 0);
    lv_label_set_text(s_mini_ic, MATERIAL_SYMBOLS_MUSIC_NOTE);
    lv_obj_set_style_text_color(s_mini_ic, UI_ACCENT_SOFT, 0);
    lv_obj_set_style_transform_pivot_x(s_mini_ic, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_mini_ic, LV_PCT(50), 0);
    lv_obj_center(s_mini_ic);

    ensure_chrome_order();
}

void apply_card_chrome(void)
{
    const bool music = strcmp(s_env->card_type, "music_list") == 0;
    const bool news = strcmp(s_env->card_type, "news_list") == 0;
    const bool daily = strcmp(s_env->card_type, "calendar_weather") == 0;
    const bool dsh_detail =
        s_env->detail_body && s_env->detail_body[0] && nl_workbench_app(s_env->app_id);
    /* 曲库改全量滚动后不再占分页钮；新闻中间钮=收起；内存卡多批时中间钮=下一批 */
    const bool mid_btn = news || list_page_count() > 1 ||
                         (s_library_source == 1 && nodelio_sd_batch_count() > 1);
    const bool compact = !music && !news && !daily &&
                         !mid_btn && !dsh_detail && s_env->filter_n == 0 && s_env->track_n <= 4;
    /* 字号条仅新闻详情；切到其它卡或列表时收起 */
    if (!news || s_news_detail < 0) {
        set_news_zoom_hdr_visible(false);
    }
    if (s_hdr_mid_btn) {
        const bool workbench = nl_workbench_app(s_env->app_id);
        lv_obj_t* label = lv_obj_get_child(s_hdr_mid_btn, 0);
        if (workbench) {
            nl_ov_set_font(label);
            lv_label_set_text(label, "换电脑");
            lv_obj_set_width(s_hdr_mid_btn, nl_ov_ui_font_line_h() * 3 + 16);
            lv_obj_set_height(s_hdr_mid_btn, 48);
            lv_obj_center(label);
            if (s_env->workbench_picker && !s_env->bridge_id[0])
                lv_obj_add_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_text_font(label, &font_material_symbols_30_4, 0);
            lv_obj_set_size(s_hdr_mid_btn, SKILL_PLAY_CTRL_SZ, SKILL_PLAY_CTRL_SZ);
            if (dsh_detail || !mid_btn)
                lv_obj_add_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(s_hdr_mid_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 日历：顶部只留关闭。标题、副标题、刷新、翻页全部收起，
     * 关闭钮浮到卡片右上角，日历网格从卡片顶端开始排。 */
    s_hdr_minimal = daily;
    if (s_hdr) {
        if (s_hdr_minimal) {
            lv_obj_add_flag(s_hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_hdr, 0);
        } else {
            lv_obj_clear_flag(s_hdr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_hdr, LV_SIZE_CONTENT);
        }
    }
    if (s_close_btn) {
        if (s_hdr_minimal) {
            /* 脱离头部行，直接浮在结果卡右上角顶死，零边距；尺寸 44 保证可点。 */
            lv_obj_set_parent(s_close_btn, s_card);
            lv_obj_add_flag(s_close_btn, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_size(s_close_btn, 44, 44);
            lv_obj_align(s_close_btn, LV_ALIGN_TOP_RIGHT, 12, -12);
            lv_obj_set_style_radius(s_close_btn, 0, 0);
            /* 提到最前，避免被后建的列表盖住。 */
            lv_obj_move_to_index(s_close_btn, -1);
        } else if (s_hdr_btns && lv_obj_get_parent(s_close_btn) != s_hdr_btns) {
            lv_obj_set_parent(s_close_btn, s_hdr_btns);
            lv_obj_clear_flag(s_close_btn, LV_OBJ_FLAG_FLOATING);
            lv_obj_set_size(s_close_btn, 44, 44);
        }
    }

    if (!s_card) {
        return;
    }
    const int hmax = skill_card_h_max();
    const bool dock = skill_player_docked();
    if (compact && !dock) {
        lv_obj_set_height(s_card, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(s_card, hmax, 0);
        lv_obj_set_style_pad_bottom(s_card, 12, 0);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 0);
            lv_obj_set_height(s_card_list, LV_SIZE_CONTENT);
        }
    } else {
        lv_obj_set_height(s_card, hmax);
        lv_obj_set_style_max_height(s_card, hmax, 0);
        if (s_card_list) {
            lv_obj_set_flex_grow(s_card_list, 1);
        }
    }
    /* 日历用更紧的内边距，把省下来的高度全给网格。 */
    lv_obj_set_style_pad_all(s_card, s_hdr_minimal ? 8 : 12, 0);
    lv_obj_set_style_pad_row(s_card, s_hdr_minimal ? 0 : 8, 0);
    /* pad_all 会清掉底边距。播放条叠在框底，列表要留出它的高度，最后几条才能滚上来。 */
    if (dock) {
        lv_obj_set_style_pad_bottom(s_card, SKILL_NOW_H + 18, 0);
    }
}

void fill_list(void)
{
    if (!s_card_list || !s_env) {
        return;
    }
    fill_filters(); /* news 等无 filters 时收起横条 */
    fill_source_bar();
    if (strcmp(s_env->card_type, "calendar_weather") == 0) {
        stop_dsh_detail_poll();
        stop_dsh_status_anims();
        s_dsh_focus_row = nullptr;
        dsh_hide_confirm_bar();
        if (s_dsh_status) {
            lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_status, 0);
        }
        if (s_dsh_detail_bar) {
            lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_detail_bar, 0);
        }
        fill_calendar_weather();
        apply_card_chrome();
        return;
    }
    if (strcmp(s_env->card_type, "news_list") == 0) {
        stop_dsh_detail_poll();
        if (s_dsh_detail_bar) {
            lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_detail_bar, 0);
        }
        fill_news();
        apply_card_chrome();
        return;
    }
    /* 带 body 的 DSH 详情卡 */
    if (s_env->detail_body && s_env->detail_body[0] && nl_workbench_app(s_env->app_id)) {
        fill_dsh_detail();
        return;
    }
    stop_dsh_detail_poll();
    stop_dsh_status_anims();
    s_dsh_focus_row = nullptr;
    if (s_dsh_detail_bar) {
        lv_obj_clean(s_dsh_detail_bar);
        lv_obj_add_flag(s_dsh_detail_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_dsh_detail_bar, 0);
    }
    lv_obj_clean(s_card_list);
    /* 清空控件引用数组 */
    bzero(s_row_idx_labels, sizeof(s_row_idx_labels));
    bzero(s_row_btns, sizeof(s_row_btns));
    const bool can_play = nodelio_caps_has_media();
    const bool comfortable = list_use_comfortable();
    const bool music_card = strcmp(s_env->card_type, "music_list") == 0;
    /* 舒适单列 / 曲库双列：指滑看全量，禁再靠估行分页硬裁 */
    if (comfortable || music_card) {
        lv_obj_add_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_card_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s_card_list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_bottom(s_card_list, music_card ? 12 : 8, 0);
    } else {
        lv_obj_clear_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_bottom(s_card_list, 0, 0);
    }

    set_hdr_mid_icon(MATERIAL_SYMBOLS_SKIP_NEXT);

    if (s_card_sub) {
        if (s_env->track_n > 0) {
            char sub[72];
            const int pages = list_page_count();
            if (s_list_page >= pages) {
                s_list_page = 0;
            }
            if (music_card) {
                const bool sd = s_library_source == 1;
                if (sd) {
                    const int batches = nodelio_sd_batch_count();
                    if (batches > 1) {
                        snprintf(sub, sizeof(sub), "内存卡 %d 首 · 第 %d/%d 批", nodelio_sd_track_count(),
                                 nodelio_sd_batch() + 1, batches);
                    } else {
                        snprintf(sub, sizeof(sub), "内存卡 %d 首 · 点播或说第几首", nodelio_sd_track_count());
                    }
                } else if (pages > 1) {
                    snprintf(sub, sizeof(sub), "共 %d 首 · %d/%d", s_env->track_n, s_list_page + 1, pages);
                } else {
                    snprintf(sub, sizeof(sub), "共 %d 首 · 点播或说歌名", s_env->track_n);
                }
            } else if (pages > 1) {
                snprintf(sub, sizeof(sub), "共 %d 条 · %d/%d · 语音说指令派活", s_env->track_n,
                         s_list_page + 1, pages);
            } else if (nl_workbench_app(s_env->app_id)) {
                if (s_dsh_phase == DSH_UI_ARMED) {
                    snprintf(sub, sizeof(sub), "续聊中 · 直接说指令");
                } else if (s_dsh_phase == DSH_UI_DISPATCHED) {
                    snprintf(sub, sizeof(sub), "已投递 · 点刷新看进度");
                } else {
                    snprintf(sub, sizeof(sub), "共 %d 条 · 点卡片看回复 · 「续聊」开麦", s_env->track_n);
                }
            } else {
                snprintf(sub, sizeof(sub), "共 %d 条", s_env->track_n);
            }
            lv_label_set_text(s_card_sub, sub);
        } else if (s_env->filter_n > 0 && nl_workbench_app(s_env->app_id)) {
            lv_label_set_text(s_card_sub, "横滑选工作区 · 再说「让它…」");
        } else {
            lv_label_set_text(s_card_sub, "点关闭或点遮罩收起");
        }
    }

    if (s_card_sub && s_env->workbench_picker) {
        if (strcmp(s_env->workbench_picker_kind, "workspace") == 0)
            lv_label_set_text_fmt(s_card_sub, "选择工作区 · 第 %d 页", s_env->workbench_page + 1);
        else if (strcmp(s_env->workbench_picker_kind, "session") == 0)
            lv_label_set_text_fmt(s_card_sub, "选择已有会话 · 第 %d 页", s_env->workbench_page + 1);
        else if (s_env->bridge_id[0])
            lv_label_set_text(s_card_sub, "选择平台 · 点整行进入");
        else
            lv_label_set_text_fmt(s_card_sub, "选择电脑 · 上下滑动 · 第 %d 页",
                                  s_env->workbench_page + 1);
    }
    if (strcmp(s_env->status, "ok") != 0) {
        lv_obj_t* err = lv_label_create(s_card_list);
        nl_ov_set_font(err);
        lv_label_set_text(err, s_env->error_message[0] ? s_env->error_message : "技能失败");
        lv_obj_set_width(err, lv_pct(100));
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(err, UI_DANGER_FG, 0);
        apply_card_chrome();
        return;
    }

    if (s_env->track_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_card_list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, s_env->spoken[0] ? s_env->spoken : "无结果");
        lv_obj_set_style_text_color(empty, UI_TEXT, 0);
        lv_obj_set_width(empty, lv_pct(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        apply_card_chrome();
        return;
    }

    const int pages = list_page_count();
    if (s_list_page < 0 || s_list_page >= pages) {
        s_list_page = 0;
    }
    const int cols = list_cols();
    const int page_sz = list_page_size();
    const int row_h = list_row_h();
    const int start = s_list_page * page_sz;
    int end = start + page_sz;
    if (end > s_env->track_n) {
        end = s_env->track_n;
    }
    for (int base = start; base < end; base += cols) {
        lv_obj_t *line = lv_obj_create(s_card_list);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, row_h);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, 6, 0);
        lv_obj_set_style_pad_row(line, 6, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

        const int ncell = (base + cols - 1 < end) ? cols : (end - base);
        for (int c = 0; c < ncell; c++) {
            const int i = base + c;
            lv_obj_t *row = lv_obj_create(line);
            lv_obj_remove_style_all(row);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_height(row, row_h);
            lv_obj_set_style_min_width(row, 0, 0);
            lv_obj_set_style_bg_color(row, ui_palette_is_dark() ? UI_FIELD : UI_SURFACE, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 12, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, UI_TILE_OFF_BORDER, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
            lv_obj_set_style_pad_hor(row, (comfortable || music_card) ? 10 : 6, 0);
            lv_obj_set_style_pad_ver(row, music_card ? 6 : (comfortable ? 8 : 0), 0);
            lv_obj_set_style_pad_column(row, music_card ? 10 : 6, 0);
            lv_obj_set_style_clip_corner(row, true, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            /* 曲库：有 URL 点播；DSH：点行拉末轮全文/最新进度（无 playback_url 也要可点） */
            const bool dsh_detail_row = nl_workbench_app(s_env->app_id) && !music_card &&
                                        !s_env->tracks[i].playback_url[0] &&
                                        (!s_env->workbench_picker || s_env->tracks[i].invoke_n > 0);
            if ((can_play && s_env->tracks[i].playback_url[0]) || dsh_detail_row) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(row, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_set_style_bg_color(row, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
            }

            if (s_env->workbench_picker && s_env->tracks[i].invoke_n == 0) {
                lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_state(row, LV_STATE_DISABLED);
            }

            const int mark = music_card ? music_end_sz() : (comfortable ? 26 : 22);
            lv_obj_t *idx_pod = lv_obj_create(row);
            lv_obj_remove_style_all(idx_pod);
            lv_obj_set_size(idx_pod, mark, mark);
            lv_obj_set_style_radius(idx_pod, music_card ? mark / 2 : 6, 0);
            lv_obj_set_style_bg_color(idx_pod, UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(idx_pod, LV_OPA_COVER, 0);
            lv_obj_clear_flag(idx_pod, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *idx_l = lv_label_create(idx_pod);
            if (music_card) {
                paint_music_mark(idx_l, i == s_play_idx, i);
            } else {
                nl_ov_set_font(idx_l);
                if (i == s_play_idx) {
                    lv_obj_set_style_text_font(idx_l, &font_material_symbols_20_4, 0);
                    lv_label_set_text(idx_l, MATERIAL_SYMBOLS_HEADPHONES);
                    lv_obj_set_style_text_color(idx_l, UI_ACCENT, 0);
                } else {
                    char idx_buf[16];
                    snprintf(idx_buf, sizeof(idx_buf), "%d", i + 1);
                    lv_label_set_text(idx_l, idx_buf);
                    lv_obj_set_style_text_color(idx_l, UI_ACCENT_SOFT, 0);
                }
            }
            lv_obj_center(idx_l);
            if (i < 64) {
                s_row_idx_labels[i] = idx_l;
            }

            lv_obj_t *txt = lv_obj_create(row);
            lv_obj_remove_style_all(txt);
            lv_obj_set_flex_grow(txt, 1);
            /* 撑满行高，主轴居中：天气等单行副文不会顶在上沿 */
            lv_obj_set_height(txt, lv_pct(100));
            lv_obj_set_style_min_width(txt, 0, 0);
            lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_row(txt, music_card ? 2 : (comfortable ? 3 : 1), 0);
            lv_obj_set_style_clip_corner(txt, true, 0);
            lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *t = lv_label_create(txt);
            nl_ov_set_font(t);
            lv_label_set_text(t, s_env->tracks[i].title);
            lv_obj_set_style_text_color(t, UI_TEXT, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, lv_pct(100));
            const int font_h = nl_ov_ui_font_line_h();
            int lsp = nl_ov_ui_line_box() - font_h;
            if (lsp < 0) {
                lsp = 0;
            }
            lv_obj_set_height(t, font_h);

            lv_obj_t *a = lv_label_create(txt);
            nl_ov_set_font(a);
            if (s_env->tracks[i].artist[0]) {
                lv_label_set_text(a, s_env->tracks[i].artist);
            } else if (music_card) {
                lv_label_set_text(a, "未知艺人");
            } else {
                lv_label_set_text(a, " ");
            }
            lv_obj_set_style_text_color(a, UI_TEXT_MUTED, 0);
            lv_obj_set_width(a, lv_pct(100));
            /* 简介按真实行数定高。预留两行空盒会把字顶到播放钮上方。 */
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            const bool sub_two_lines =
                comfortable && s_env->app_id[0] && nl_workbench_app(s_env->app_id);
            int cell_w = skill_panel_w();
            if (cols > 1) {
                cell_w = (cell_w - 6 * (cols - 1)) / cols;
            }
            const int side_pad = (comfortable || music_card) ? 10 : 6;
            const int gap = music_card ? 10 : 6;
            const bool has_play = can_play && s_env->tracks[i].playback_url[0];
            const int play_w = has_play ? (music_card ? mark : SKILL_CTRL_SZ) : 0;
            int text_w = cell_w - side_pad * 2 - gap - mark - (has_play ? gap + play_w : 0);
            if (text_w < 48) {
                text_w = 48;
            }
            int sub_h = nl_ov_wrapped_text_h(lv_label_get_text(a), text_w);
            const int sub_cap = sub_two_lines ? (font_h * 2 + lsp) : font_h;
            if (sub_h > sub_cap) {
                sub_h = sub_cap;
            }
            lv_obj_set_height(a, sub_h);
            lv_obj_set_height(txt, LV_SIZE_CONTENT);

            if (can_play && s_env->tracks[i].playback_url[0]) {
                const char *icon = (i == s_play_idx) ? MATERIAL_SYMBOLS_PAUSE : MATERIAL_SYMBOLS_PLAY_ARROW;
                lv_obj_t *btn = make_icon_btn(row, UI_ACCENT, UI_BG, icon, nullptr);
                if (music_card) {
                    lv_obj_set_size(btn, mark, mark);
                    lv_obj_set_style_radius(btn, mark / 2, 0);
                }
                lv_obj_add_event_cb(btn, on_play_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                if (i < 64) {
                    s_row_btns[i] = btn;
                }
            }
            /* Protocol run:invoke（如 DSH 允许/拒绝）；最多 2 个 */
            for (int ai = 0; ai < s_env->tracks[i].invoke_n && ai < NL_ENV_ACTION_MAX; ai++) {
                const intptr_t packed = ((intptr_t)i << 8) | (intptr_t)ai;
                const char *btn_title = s_env->tracks[i].invokes[ai].title;
                bool restyle = false;
                lv_color_t btn_bg = UI_ACCENT;
                if (dsh_track_is_focus(i) && strstr(btn_title, "续聊") != nullptr) {
                    if (s_dsh_phase == DSH_UI_ARMED) {
                        btn_title = "取消";
                        btn_bg = UI_WARN;
                        restyle = true;
                    } else if (s_dsh_phase == DSH_UI_DISPATCHED) {
                        btn_title = "已投递";
                        btn_bg = UI_INFO;
                        restyle = true;
                    }
                }
                lv_obj_t *ab = make_text_action_btn(row, btn_title, on_invoke_action, (void *)packed);
                if (ab && restyle) {
                    lv_obj_set_style_bg_color(ab, btn_bg, 0);
                }
            }
            if (!s_env->workbench_picker)
                dsh_style_list_row(row, i, idx_pod, a, s_env->tracks[i].artist);
        }
        if (ncell < cols) {
            lv_obj_t *pad = lv_obj_create(line);
            lv_obj_remove_style_all(pad);
            lv_obj_set_flex_grow(pad, 1);
            lv_obj_set_height(pad, row_h);
            lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    if (nl_workbench_app(s_env->app_id)) {
        dsh_paint_status_strip();
    } else if (s_dsh_phase != DSH_UI_IDLE) {
        dsh_clear_ui_phase();
    }
    apply_card_chrome();
    /* 在播项可能在折下：卡高确定后滚入视野 */
    if (music_card && s_play_idx >= 0 && s_play_idx < 64 && s_row_btns[s_play_idx] &&
        lv_obj_has_flag(s_card_list, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_t *row = lv_obj_get_parent(s_row_btns[s_play_idx]);
        if (row) {
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        }
    }
}

bool skill_ui_is_up(void)
{
    if (s_have_env) {
        return true;
    }
    if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
        return true;
    }
    if (s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_mini_fab && !lv_obj_has_flag(s_mini_fab, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    if (s_scrim && !lv_obj_has_flag(s_scrim, LV_OBJ_FLAG_HIDDEN)) {
        return true;
    }
    return false;
}

bool nodelio_skill_result_is_visible(void)
{
    return skill_ui_is_up();
}

bool nodelio_skill_result_card_visible(void) {
    return s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_ui_closing;
}

bool nodelio_skill_news_ui_active(void)
{
    return s_have_env && s_env && strcmp(s_env->card_type, "news_list") == 0 && skill_ui_is_up();
}

bool nodelio_skill_is_live_radio(void)
{
    if (!s_have_env || !s_env) {
        return false;
    }
    if (env_media_is_live()) {
        return true;
    }
    return strstr(s_env->app_id, "radio") != nullptr;
}

bool nodelio_skill_reopen_media_list(void)
{
    if (!s_have_env || !s_env || s_env->track_n <= 0) {
        return false;
    }
    reopen_list();
    return true;
}

void nodelio_skill_close_all(void)
{
    const bool stop_news = nodelio_skill_news_ui_active();
    nodelio_media_stop();
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    dismiss_library_and_player();
    if (stop_news) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().AbortNewsSpeak();
        });
    }
}

bool nodelio_skill_try_dismiss_phrase(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    const bool close = strstr(text, "关闭") || strstr(text, "关掉") || strstr(text, "关上") ||
                       strstr(text, "退出") || strstr(text, "收起");
    if (!close) {
        return false;
    }
    /* 控家对象留给离线控家口令 */
    if (strstr(text, "灯") || strstr(text, "继电器") || strstr(text, "插座") ||
        strstr(text, "窗帘") || strstr(text, "卷帘") || strstr(text, "情景") ||
        strstr(text, "控家") || strstr(text, "小智") || strstr(text, "省电")) {
        return false;
    }
    /* 点名关播放器/曲库：即使弹窗已收成播放条也关 */
    const bool named =
        strstr(text, "播放器") || strstr(text, "曲库") || strstr(text, "音乐") ||
        strstr(text, "电台") || strstr(text, "播客") || strstr(text, "新闻") ||
        strstr(text, "技能") || strstr(text, "应用");
    if (!named && !skill_ui_is_up()) {
        return false;
    }
    if (named && !skill_ui_is_up() && !nodelio_media_is_playing() && !nodelio_media_is_paused()) {
        /* 无弹窗也无在播：不当作已处理，留给其它口令 */
        return false;
    }
    /* 弹窗/播放条已在屏上：关闭电台/关掉/收起…一律停播并退下 */
    ESP_LOGI(TAG, "voice dismiss skill ui: %s", text);
    nodelio_skill_close_all();
    return true;
}

void nodelio_skill_result_hide(void)
{
    /* 新闻卡与云端 TTS 解耦：收起弹出层须掐朗读，否则抽屉底下仍在念。 */
    const bool stop_news =
        s_have_env && s_env && strcmp(s_env->card_type, "news_list") == 0 && skill_ui_is_up();
    s_news_gen += 1;
    s_news_detail = -1;
    if (s_news_body_heap) {
        heap_caps_free(s_news_body_heap);
        s_news_body_heap = nullptr;
    }
    /* 保留 s_env 歌单：点播放条可 reopen；真正关掉走 dismiss/close_all → free_skill_env */
    clear_ttl();
    stop_dsh_detail_poll();
    s_dsh_detail_ws[0] = '\0';
    s_dsh_detail_sess[0] = '\0';
    nodelio_panel_ui_clear_dsh_arm();
    if (stop_news) {
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().AbortNewsSpeak();
        });
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        s_dsh_phase = DSH_UI_IDLE;
        s_dsh_focus_sess[0] = '\0';
        s_dsh_focus_ws[0] = '\0';
        s_dsh_last_instr[0] = '\0';
        s_dsh_focus_row = nullptr;
        s_dsh_dispatched_us = 0;
        return;
    }
    dsh_clear_ui_phase();
    s_minimized = false;
    stop_now_anims();
    hide_mini_fab();
    reset_obj_anim_style(s_scrim);
    reset_obj_anim_style(s_card);
    reset_obj_anim_style(s_now_bar);
    if (s_scrim) {
        lv_obj_add_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_card) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_now_bar && !lv_obj_has_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN)) {
        apply_now_media_chrome(nodelio_media_is_playing(), nodelio_media_is_paused(), false);
    }
    set_drawer_for_skill(false);
    nl_ov_apps_notify_skill_open(false);
    /* 保留歌单：hide 只收起卡片；点播放条仍可 reopen_list */
    if (!s_env || s_env->track_n <= 0) {
        s_have_env = false;
    }
    ensure_chrome_order();
}

void nodelio_skill_result_show(const nodelio_nvs_config_t *cfg, nl_content_envelope_t *env,
                               bool auto_play_first)
{
    if (!env) {
        return;
    }
    const uint32_t epoch = nodelio_panel_job_result_epoch();
    if (epoch) {
        const auto config = cfg ? *cfg : nodelio_nvs_config_t{};
        const bool have_cfg = cfg != nullptr;
        Application::GetInstance().Schedule([config, have_cfg, env, auto_play_first, epoch]() {
            DisplayLockGuard lock(nl_ov_disp());
            if (!lock || !nodelio_panel_skill_view_current(epoch)) {
                free_owned_env(env);
                return;
            }
            nodelio_skill_result_show(have_cfg ? &config : nullptr, env, auto_play_first);
        });
        return;
    }
    DisplayLockGuard ownership_lock(nl_ov_disp());
    if (!ownership_lock) {
        free_owned_env(env);
        return;
    }
    /* 仅「新弹开」算用户活动。静默刷新若也 NoteActivity，详情 10s 轮询会永久挡息屏 */
    const bool was_open =
        s_skill_ui_active && s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_minimized;
    if (!was_open) {
        Board::GetInstance().NoteUserActivity();
    }
    if (cfg) {
        s_skill_cfg = *cfg;
    }
    /* 列表已开且新包只有 1 首要播：若该曲已在旧列表 → 不替换歌单，只 play_at */
    if (auto_play_first && env->auto_play && env->track_n == 1 && s_have_env && s_env &&
        s_env->track_n > 1 && strcmp(s_env->card_type, "music_list") == 0 &&
        (strcmp(env->card_type, "music_list") == 0 || env->card_type[0] == '\0')) {
        int hit = find_track_in_env(s_env, &env->tracks[0]);
        if (hit >= 0 && env->tracks[0].playback_url[0]) {
            ESP_LOGI(TAG, "keep list n=%d play hit=%d title=%s", s_env->track_n, hit,
                     env->tracks[0].title);
            {
                const int ps = list_page_size();
                if (ps > 0) {
                    s_list_page = hit / ps;
                }
            }
            play_at(hit);
            DisplayLockGuard lock(nl_ov_disp());
            if (lock && s_ready && s_card) {
                fill_list();
                layout_skill_layers();
                raise_skill_layers();
            }
            free_owned_env(env);
            return;
        }
    }
    char keep_url[NL_ENV_URL];
    keep_url[0] = '\0';
    if (s_env && s_play_idx >= 0 && s_play_idx < s_env->track_n &&
        s_env->tracks[s_play_idx].playback_url[0]) {
        snprintf(keep_url, sizeof(keep_url), "%s", s_env->tracks[s_play_idx].playback_url);
    }
    // Switching applications must not retain the previous platform's armed voice target.
    if (s_env && nl_workbench_app(s_env->app_id)) {
        char previous[72], next[72];
        nl_workbench_status_id(s_env->app_id, previous, sizeof(previous));
        nl_workbench_status_id(env->app_id, next, sizeof(next));
        if (!nl_workbench_app(env->app_id) || strcmp(previous, next) != 0 ||
            strcmp(s_env->bridge_id, env->bridge_id) != 0 ||
            strcmp(s_env->workbench_provider, env->workbench_provider) != 0 ||
            env->workbench_picker) {
            nodelio_panel_ui_clear_dsh_arm();
            dsh_clear_ui_phase();
            stop_dsh_detail_poll();
        }
    }
    /* 接管堆信封：先摘掉旧指针再挂新的，峰值只保留一份 ~54KB */
    if (s_env && s_env != env) {
        free_skill_env();
    }
    s_env = env;
    s_have_env = true;
    /* 来源跟随信封：内存卡曲库 app_id=music.sd；其余音乐列表一律回在线曲库，
     * 否则切到电台/新闻后回来，「换一批」会误走本地批次。 */
    if (strcmp(s_env->card_type, "music_list") == 0) {
        s_library_source = strcmp(s_env->app_id, "music.sd") == 0 ? 1 : 0;
    }
    /* DSH：勿无清 DISPATCHED/ARMED；远端已 running/attention 或超时再清 */
    const bool is_dsh = nl_workbench_app(s_env->app_id);
    bool clear_dsh_ui = false;
    if (is_dsh) {
        if (s_dsh_phase == DSH_UI_ARMED) {
            /* 续聊武装保留到超时/取消 */
        } else if (s_dsh_phase == DSH_UI_DISPATCHED) {
            const char *art = dsh_focus_track_artist();
            const bool confirmed =
                dsh_subtitle_is_running(art) || dsh_subtitle_needs_you(art);
            const int64_t age =
                s_dsh_dispatched_us > 0 ? (esp_timer_get_time() - s_dsh_dispatched_us) : 0;
            if (confirmed || age > 25 * 1000000LL) {
                clear_dsh_ui = true;
            }
        } else {
            clear_dsh_ui = true;
        }
        if (clear_dsh_ui) {
            s_dsh_phase = DSH_UI_IDLE;
            s_dsh_focus_sess[0] = '\0';
            s_dsh_focus_ws[0] = '\0';
            s_dsh_last_instr[0] = '\0';
            s_dsh_focus_row = nullptr;
            s_dsh_dispatched_us = 0;
        }
    }
    s_list_page = 0;
    s_play_idx = -1;
    if (keep_url[0]) {
        for (int i = 0; i < s_env->track_n; i++) {
            if (strcmp(s_env->tracks[i].playback_url, keep_url) == 0) {
                s_play_idx = i;
                {
                    const int ps = list_page_size();
                    if (ps > 0) {
                        s_list_page = i / ps;
                    }
                }
                break;
            }
        }
    }

    if (!s_ready || !s_root) {
        return;
    }
    if (!s_visible) {
        nodelio_overlay_show(false);
    }

    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    /* 卡已开：刷新/派活回包只换内容，禁止再跑入场淡入（否则像关了又弹） */
    const bool card_already_open =
        s_card && !lv_obj_has_flag(s_card, LV_OBJ_FLAG_HIDDEN) && !s_minimized &&
        s_skill_ui_active && !s_ui_closing;

    if (s_ui_closing) {
        s_close_gen += 1;
        s_ui_closing = false;
        reset_obj_anim_style(s_scrim);
        reset_obj_anim_style(s_card);
        reset_obj_anim_style(s_now_bar);
    }
    ensure_card();
    if (!s_card) {
        return;
    }
    if (clear_dsh_ui) {
        stop_dsh_status_anims();
        if (s_dsh_status) {
            lv_obj_add_flag(s_dsh_status, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_dsh_status, 0);
        }
    }

    ensure_chrome_order();

    if (!s_news_screen_hook) {
        nodelio_panel_ui_set_screen_cb(news_on_screen);
        s_news_screen_hook = true;
    }
    s_news_detail = -1;
    const char *title = s_env->display_title[0] ? s_env->display_title : "搜索结果";
    if (strcmp(s_env->card_type, "news_list") == 0 && !s_env->display_title[0]) {
        title = "RSS 订阅";
    }
    /* 关音乐后再开新闻：残留 now_playing / 悬浮球会盖在卡上抢触摸，详情点不进去 */
    if (strcmp(s_env->card_type, "music_list") != 0) {
        if (nodelio_media_is_playing() || nodelio_media_is_paused()) {
            nodelio_media_stop();
        }
        if (s_now_bar) {
            lv_obj_add_flag(s_now_bar, LV_OBJ_FLAG_HIDDEN);
        }
        s_minimized = false;
        hide_mini_fab();
    }
    if (s_env->bridge_name[0])
        lv_label_set_text_fmt(s_card_title, "%s · %s", s_env->bridge_name, title);
    else
        lv_label_set_text(s_card_title, title);
    fill_list();

    s_minimized = false;
    hide_mini_fab();
    set_drawer_for_skill(true);

    if (s_scrim) {
        lv_obj_clear_flag(s_scrim, LV_OBJ_FLAG_HIDDEN);
        if (card_already_open) {
            reset_obj_anim_style(s_scrim);
        } else {
            anim_show_obj(s_scrim, false);
        }
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    if (card_already_open) {
        reset_obj_anim_style(s_card);
    } else {
        anim_show_obj(s_card, true);
    }
    layout_skill_layers();
    raise_skill_layers();
    ensure_chrome_order();
    nl_ov_apps_notify_skill_open(true);

    /* 已开卡静默刷新：勿重置 ttl，否则永远关不掉 */
    if (!card_already_open) {
        restart_skill_ttl();
    }

    if (auto_play_first && nodelio_caps_has_media() && s_env->track_n > 0 &&
        s_env->tracks[0].playback_url[0]) {
        ESP_LOGI(TAG, "auto play #0 title=%s", s_env->tracks[0].title);
        play_at(0);
    } else if (auto_play_first) {
        ESP_LOGW(TAG, "auto play skipped caps=%d tracks=%d url0=%d",
                 nodelio_caps_has_media() ? 1 : 0, s_env->track_n,
                 (s_env->track_n > 0 && s_env->tracks[0].playback_url[0]) ? 1 : 0);
    }

    ESP_LOGI(TAG, "show card_type=%s tracks=%d news=%d auto_play=%d", s_env->card_type, s_env->track_n,
             s_env->news_n, auto_play_first ? 1 : 0);

    /* 语音打开新闻：短 detect 追问（整句含前缀须 ≤16 字，勿带标题） */
    if (!card_already_open && strcmp(s_env->card_type, "news_list") == 0 &&
        strcmp(s_env->status, "ok") == 0 && s_env->news_n > 0 &&
        nodelio_panel_ui_take_news_read_offer()) {
        ESP_LOGI(TAG, "news read offer → 要读吗？");
        nodelio_panel_ui_request_cloud_speak("要读吗？");
        Application::GetInstance().ArmPendingConfirmReadNews();
    }
}
