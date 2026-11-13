#pragma once

#include "sdkconfig.h"
#include "skill_result_ui.h"
#include "news_read_progress.h"
#include "panel_jobs.h"

#include "device_caps.h"
#include "lunar_date.h"
#include "media_player.h"
#include "sd_library.h"
#include "sd_photo_ui.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nodelio_overlay.h"
#include "overlay_priv.h"
#include "panel_ui.h"
#include "skill_runtime.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "offline_cmd.h"
#include "offline_cmd_map.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <material_symbols.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#define TAG "nl_skill_ui"

#define SKILL_CARD_W (nodelio_display_profile()->result_width)
/* result_max_height 仍按「顶栏占位」算。顶栏现已透明且空闲隐藏，开面板时把这段高度加回去。
 * 有 now_playing 条时须再扣 SKILL_NOW_H+skill_ui_gap()，否则卡底会画出屏外。 */
#define SKILL_CARD_H_MAX (nodelio_display_profile()->result_max_height + NL_UI_STATUS_H)
#define SKILL_NOW_H 88
#define SKILL_GAP 12
/**
 * 设置详情 + 应用卡并开：应用写死贴上下右、无圆角（与全局「卡片圆角」无关）。
 * 仅宽屏；方屏无并排。
 */
static inline bool skill_flush_with_detail(void)
{
    return nodelio_display_profile()->nav_width != 0 && s_settings_detail != nullptr;
}
/** 并开时中间缝；应用右侧贴边故不再留右缝。 */
#define SKILL_DETAIL_MID_GAP 12
/**
 * 宽屏应用卡外边距。设置「卡片圆角=0」时贴边无缝（便于对比靠边效果）。
 * 方屏仍用 SKILL_GAP。并开详情时外边距强制 0。
 */
static inline int skill_ui_gap(void)
{
    if (skill_flush_with_detail()) {
        return 0;
    }
    if (nodelio_display_profile()->nav_width != 0 && nodelio_card_radius_px() == 0) {
        return 0;
    }
    return SKILL_GAP;
}
/** 顶栏不占位，面板从屏顶留白起排（贴边模式为 0）。 */
static inline int skill_top_y(void)
{
    return skill_ui_gap();
}
#define SKILL_CARD_H_MIN 180
#define SKILL_BORDER_OPA 200
#define SKILL_SCRIM_OPA 170
#define SKILL_EQ_N 4
#if CONFIG_BOARD_TYPE_SILA_P4C5_508
/* 508：顶栏仍要紧，但刷新/关闭须可点 */
#define SKILL_CTRL_SZ 44
#define SKILL_PLAY_CTRL_SZ 44
#define SKILL_CARD_PAD 8
#define SKILL_CARD_PAD_ROW 4
#define SKILL_HDR_TXT_PAD_ROW 0
#define SKILL_LIST_HDR_BUDGET 48
#define SKILL_LIST_CHROME_PAD 20 /* pad×2 + pad_row */
#else
#define SKILL_CTRL_SZ 52
#define SKILL_PLAY_CTRL_SZ 52
#define SKILL_CARD_PAD 12
#define SKILL_CARD_PAD_ROW 8
#define SKILL_HDR_TXT_PAD_ROW 0
#define SKILL_LIST_HDR_BUDGET 56
#define SKILL_LIST_CHROME_PAD 32 /* card pad_all×2 + pad_row */
#endif
#define SKILL_MINI_SZ 52
#define SKILL_ANIM_MS 280
#define SKILL_ANIM_SLIDE 28
#define SKILL_ANIM_SLIDE_X 72
#define SKILL_PAGE_COLS 2
#define SKILL_PAGE_ROWS 5
#define SKILL_PAGE_SIZE (SKILL_PAGE_COLS * SKILL_PAGE_ROWS)
/* 普通 list（DSH 等）：单列舒适行，避免双列挤成省略号 */
#define SKILL_LIST_COLS 1
#define SKILL_LIST_ROWS 4
#define SKILL_LIST_ROW_H 68
/* 曲库：标题 + 艺人各一行；序号圆标克制。矮屏有 now_playing 时靠动态行数分页 */
#define SKILL_MUSIC_ROW_H 56
#define NEWS_ARTICLE_CAP (20 * 1024)
/* JSON 转义后的正文预算；预留标题、提示及 MCP/session 包装，总包控制在 2KB 内。 */
#define NEWS_MCP_CHUNK 640
/* 本地 detect 播报：整包 JSON 须 ≤720B（聆听 DRAM~9KB）；正文约 360B ≈120 字，多段「继续读」。 */
#define NEWS_LOCAL_SPEAK_CHUNK 360
/* 云端回源拉正文最多约 8s，默认 HTTP 读超时 2.5s 会在状态行到达前断开 */
#define NEWS_HTTP_IO_MS 12000
#define NEWS_TASK_STACK 10240
/* 服务端 dsh.status 桥超时 20s；面板须略长，否则等桥时 SO_RCVTIMEO 先断 → recv=0「加载失败」 */
#define DSH_HTTP_IO_MS 22000
/* 任务详情页静默拉最新末轮；仅亮屏+停留详情时跑，省电 */
#define DSH_DETAIL_POLL_MS 10000
/** DSH 无 ttl 时默认 120s 无操作关卡（省电）；桥可下发更大/更小 ttl_sec */
#define DSH_IDLE_TTL_SEC_DEFAULT 120

enum dsh_ui_phase_t : uint8_t { DSH_UI_IDLE = 0, DSH_UI_ARMED, DSH_UI_DISPATCHED };

extern lv_obj_t *s_scrim;
extern lv_obj_t *s_card;
extern lv_obj_t *s_filter_bar;
/** 曲库来源切换：在线曲库 / 内存卡，排在顶栏刷新钮左侧；非曲库卡时收起。 */
extern lv_obj_t *s_src_bar;
extern lv_obj_t *s_dsh_status;      /* 续聊/执行中状态条：EQ + 文案 */
extern lv_obj_t *s_dsh_status_lbl;
extern lv_obj_t *s_dsh_eq[3];
extern lv_obj_t *s_dsh_confirm_bar; /* 续聊待确认：发送 / 取消 */
extern lv_obj_t *s_dsh_detail_bar;  /* 详情底栏：续聊悬浮，不随正文滚走 */
extern char s_dsh_pending_preview[96];
extern lv_obj_t *s_card_list;
extern lv_obj_t *s_card_title;
extern lv_obj_t *s_title_row;
extern lv_obj_t *s_support_nav;
extern lv_obj_t *s_card_sub;
extern lv_obj_t *s_now_bar;
extern lv_obj_t *s_now_eq_wrap;
extern lv_obj_t *s_now_eq[SKILL_EQ_N];
extern lv_obj_t *s_now_lbl;
extern lv_obj_t *s_now_sub;
extern lv_obj_t *s_now_btns;
extern lv_obj_t *s_now_pause_btn;
extern lv_obj_t *s_now_prev_btn;
extern lv_obj_t *s_now_next_btn;
extern lv_obj_t *s_mini_fab;
extern lv_obj_t *s_mini_ic;
extern lv_timer_t *s_ttl_tmr;
extern lv_timer_t *s_dsh_detail_poll;
extern char s_dsh_detail_ws[200];
extern char s_dsh_detail_sess[80];
extern std::atomic<bool> s_dsh_detail_busy;
extern lv_anim_t s_now_border_anim;
extern bool s_now_border_anim_on;
extern bool s_mini_pulse_on;
extern bool s_dsh_eq_anim_on;
extern bool s_dsh_row_pulse_on;
extern lv_obj_t *s_dsh_focus_row; /* 续聊焦点行，rebuild 时清空 */
extern lv_obj_t *s_dsh_pulse_rows[SKILL_LIST_ROWS]; /* 边框呼吸：焦点 + 执行中 */
extern int s_dsh_pulse_n;
extern dsh_ui_phase_t s_dsh_phase;
extern char s_dsh_focus_sess[80];
extern char s_dsh_focus_ws[200];
extern char s_dsh_last_instr[96];
extern int64_t s_dsh_dispatched_us; /* DISPATCHED 起算，约 25s 内保留乐观态 */
extern bool s_minimized;
extern bool s_ui_closing;
extern uint32_t s_close_gen;
extern nodelio_nvs_config_t s_skill_cfg;
/** ~54KB：必须 PSRAM。由 deliver 堆分配后交给 show 接管，禁止再拷一份。 */
extern nl_content_envelope_t *s_env;
extern bool s_have_env;
extern int s_play_idx;
extern int s_list_page;
extern lv_obj_t *s_hdr_mid_btn;
extern lv_obj_t *s_hdr_btns;
extern lv_obj_t *s_hdr;        /* 头部行；精简模式下整体收起 */
extern lv_obj_t *s_hdr_txt;    /* 标题 + 副标题；精简模式下只留标题 */
extern lv_obj_t *s_close_btn;  /* 关闭钮；精简模式下浮到卡片右上角 */
extern bool s_hdr_minimal;     /* 日历：顶部只留关闭，把高度让给内容 */
extern lv_obj_t *s_news_zoom_bar; /* 新闻详情：顶栏刷新左侧 A-/A+ */
extern lv_obj_t *s_row_idx_labels[64];  /* 序号胶囊标签（数字/耳机） */
extern lv_obj_t *s_row_btns[64];       /* 播放/暂停按钮 */
extern int s_news_detail;
extern uint32_t s_news_gen;
extern bool s_news_screen_hook;
extern lv_obj_t *s_news_body_lbl;
/** 详情正文驻 PSRAM，供 lv_label_set_text_static，避免长文二次占 LVGL 池失败后仍显示摘要 */
extern char *s_news_body_heap;

/* Core helpers (skill_result_ui.cc) */
void free_skill_env(void);
void free_owned_env(nl_content_envelope_t *env);
void ensure_chrome_order(void);
void apply_card_chrome(void);
bool skill_player_docked(void);
int skill_card_h_max(void);
int skill_panel_w(void);
void style_skill_frame(void);
void layout_skill_layers(void);
void raise_skill_layers(void);
void sync_player_block_scrim(void);
void set_drawer_for_skill(bool skill_on);
void clear_ttl(void);
void restart_skill_ttl(void);
const char *env_media_kind(void);
bool env_media_is_live(void);
void close_list_keep_player(void);
bool skill_ui_is_up(void);
void paint_music_mark(lv_obj_t *lbl, bool playing, int index);
void style_music_track_row(lv_obj_t *row, bool playing);
void reflow_music_list_if_open(void);
void set_hdr_mid_icon(const char *sym);
void reopen_list(void);
void json_escape_str(char *out, size_t out_len, const char *in);
void run_invoke_action(const nl_env_invoke_action_t *ia);
void on_invoke_action(lv_event_t *e);
int parse_ordinal_zh(const char *phrase);
void invoke_filter_workspace(const nl_env_filter_t *f);
lv_obj_t *make_text_action_btn(lv_obj_t *parent, const char *title, lv_event_cb_t cb, void *ud);
lv_obj_t *make_icon_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym, lv_event_cb_t cb);
lv_obj_t *make_play_ctrl_btn(lv_obj_t *parent, lv_color_t bg, lv_color_t fg, const char *sym, lv_event_cb_t cb);
void anim_eq_height(void *var, int32_t v);
void anim_border_opa(void *var, int32_t v);
void anim_show_obj(lv_obj_t *obj, bool from_below);
void reset_obj_anim_style(lv_obj_t *obj);
void dismiss_library_and_player(void);
void fill_list(void);

/* Library sources (skill_result_ui.cc + sd_library.c) */

/* Media bar & FAB (skill_media_bar.cc) */
void style_now_bar(bool docked);
void apply_now_media_chrome(bool playing, bool paused, bool list_open);
void on_minimize(lv_event_t *e);
void on_mini_fab(lv_event_t *e);
void on_stop_now(lv_event_t *e);
void on_pause_now(lv_event_t *e);
void on_voice_now(lv_event_t *e);
void on_prev_now(lv_event_t *e);
void on_next_now(lv_event_t *e);
void set_pause_btn_icon(bool playing);
void clear_play_mark(void);
void update_row_icon(int idx);
void play_at(int idx);
int neighbor_playable(int from, int dir);
void stop_mini_pulse(void);
void start_mini_pulse(void);
void hide_mini_fab(void);
void show_mini_fab(bool playing);
void minimize_to_fab(void);
void expand_from_fab(void);
void stop_now_anims(void);
void start_now_anims(void);

/* DSH UI (skill_dsh_ui.cc) */
void stop_dsh_detail_poll(void);
void start_dsh_detail_poll(void);
void on_dsh_detail_poll(lv_timer_t *t);
bool dsh_is_detail_view(void);
void dsh_extract_arm(const nl_env_track_t *tr, char *ws, size_t ws_len, char *sess, size_t sess_len);
bool dsh_track_is_focus(int track_idx);
void stop_dsh_row_pulses(void);
void stop_dsh_eq_anims(void);
void stop_dsh_status_anims(void);
void start_dsh_row_pulses(void);
void start_dsh_status_anims(void);
void dsh_hide_confirm_bar(void);
void dsh_clear_ui_phase(void);
void dsh_paint_status_strip(void);
void dsh_style_list_row(lv_obj_t *row, int track_idx, lv_obj_t *idx_pod, lv_obj_t *artist_lbl, const char *artist);
void dsh_enter_armed(const char *ws, const char *sess);
void dsh_refresh_list(void);
void on_dsh_back_list(lv_event_t *e);
void dsh_fetch_detail(const char *ws, const char *sess, bool silent);
void dsh_fetch_session_detail(int track_idx);
bool dsh_subtitle_is_running(const char *sub);
bool dsh_subtitle_needs_you(const char *sub);
const char *dsh_focus_track_artist(void);
void dsh_detail_paint_running(const char *subtitle);
void fill_dsh_detail(void);

/* News UI (skill_news_ui.cc) */
void news_on_screen(bool on);
void news_sync_global_text_scale(void);
void set_news_zoom_hdr_visible(bool on);
void fill_news(void);

/* Daily UI (skill_daily_ui.cc) */
void fill_calendar_weather(void);

/* Support inbox / thread (skill_support_ui.cc) */
void fill_support_thread(void);
