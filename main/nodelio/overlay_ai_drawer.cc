#include "overlay_priv.h"

#include "skill_result_ui.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "display.h"
#include "fonts/font_puhui_14_1.h"
#include "layout_cache.h"
#include "mqtt_panel.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

#include "audio_codec.h"
#include "backlight.h"

#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_ai"

/** 说话时正文限频（RGB 全刷太贵） */
static int64_t s_speak_text_last_us;
static char s_speak_text_pending[NL_CHAT_CAP];
static bool s_speak_text_dirty;
/** 助手告别后：禁止 … / listening / restore 再闪抽屉，至下次用户开口或建链 */
static bool s_ai_farewell_closing;
/** 用户点关闭：本轮听/说的状态刷新不得再弹出，至下次建链或用户开口 */
static bool s_ai_user_dismissed;
/** 用户最小化：抽屉藏起，右下角仅图标；会话与文案仍更新 */
static bool s_ai_minimized;
static lv_obj_t *s_ai_mini_ic;
/** 息屏断网冷唤：CONNECT 晚 1s 再弹紧凑条；听/说提前到达则取消并展开正式抽屉 */
#define CONNECT_SHOW_DELAY_MS 1000
static lv_timer_t *s_connect_delay_timer = nullptr;
static char s_connect_pending_status[48];
static char s_connect_pending_body[96];
static bool s_connect_pending_has_body;
static bool s_connect_delay_force; /* timer 到期强制弹出，绕过 delay 门 */

static void hide_ai_mini_fab(void);
static void show_ai_mini_fab(chat_phase_t phase);
static void minimize_ai_drawer(void);
static void expand_ai_drawer(void);
static void paint_mini_fab_phase(chat_phase_t phase);
static void apply_drawer_chrome(bool connect_compact);
static void start_eq_anims(chat_phase_t phase);
static void stop_eq_anims(void);

static bool text_is_farewell(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    return strstr(text, "再见") || strstr(text, "退下") || strstr(text, "拜拜") ||
           strstr(text, "退出") || strstr(text, "我先休息了") || strstr(text, "随时叫我") ||
           strstr(text, "先不聊了") || strstr(text, "有事再叫我");
}

bool nl_ov_ai_farewell_closing(void)
{
    return s_ai_farewell_closing;
}

bool nl_ov_ai_user_dismissed(void)
{
    return s_ai_user_dismissed;
}

bool nl_ov_ai_is_minimized(void)
{
    return s_ai_minimized;
}

static void hide_ai_mini_fab(void)
{
    if (s_ai_mini_fab) {
        lv_obj_add_flag(s_ai_mini_fab, LV_OBJ_FLAG_HIDDEN);
    }
}

static void paint_mini_fab_phase(chat_phase_t phase)
{
    if (!s_ai_mini_fab) {
        return;
    }
    uint32_t border = UI_C_INFO;
    uint32_t icon = UI_C_INFO;
    if (phase == CHAT_SPEAK) {
        border = UI_C_TILE_ON_BORDER;
        icon = UI_C_ACCENT_SOFT;
    } else if (phase == CHAT_CONNECT) {
        border = UI_C_WARN;
        icon = UI_C_WARN;
    }
    lv_obj_set_style_border_color(s_ai_mini_fab, lv_color_hex(border), 0);
    if (s_ai_mini_ic) {
        lv_obj_set_style_text_color(s_ai_mini_ic, lv_color_hex(icon), 0);
    }
}

static void show_ai_mini_fab(chat_phase_t phase)
{
    if (!s_ai_mini_fab) {
        return;
    }
    paint_mini_fab_phase(phase);
    lv_obj_clear_flag(s_ai_mini_fab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_mini_fab);
}

void nl_ov_ai_raise_mini_fab(void)
{
    if (!s_ai_minimized) {
        return;
    }
    show_ai_mini_fab(s_chat_phase);
}

static void minimize_ai_drawer(void)
{
    if (!s_ai_drawer) {
        return;
    }
    s_ai_minimized = true;
    lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    show_ai_mini_fab(s_chat_phase != CHAT_IDLE ? s_chat_phase : CHAT_LISTEN);
}

static void expand_ai_drawer(void)
{
    s_ai_minimized = false;
    hide_ai_mini_fab();
    if (!s_ai_drawer || s_skill_ui_active || !nl_ov_ai_session_active()) {
        return;
    }
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_drawer);
    apply_drawer_chrome(s_chat_phase == CHAT_CONNECT);
    start_eq_anims(s_chat_phase);
}

static void eq_anim_exec(void *obj, int32_t v)
{
    lv_obj_set_height(static_cast<lv_obj_t *>(obj), v);
}

static void stop_eq_anims(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_eq_bars[i]) {
            lv_anim_delete(s_eq_bars[i], eq_anim_exec);
            lv_obj_set_height(s_eq_bars[i], 10);
            lv_obj_set_style_bg_color(s_eq_bars[i], UI_TEXT_MUTED, 0);
        }
    }
}

/** CONNECT 用紧凑条，不再刷长正文（首屏太快读不完） */
static void apply_drawer_chrome(bool connect_compact)
{
    if (!s_ai_drawer) {
        return;
    }
    if (connect_compact) {
        lv_obj_set_size(s_ai_drawer, UI_AI_DRAWER_COMPACT_W, UI_AI_DRAWER_COMPACT_H);
        lv_obj_set_style_pad_bottom(s_ai_drawer, 8, 0);
        if (s_ai_drawer_body) {
            lv_obj_add_flag(s_ai_drawer_body, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_set_size(s_ai_drawer, UI_AI_DRAWER_W, UI_AI_DRAWER_H);
        lv_obj_set_style_pad_bottom(s_ai_drawer, 12, 0);
        if (s_ai_drawer_body) {
            lv_obj_clear_flag(s_ai_drawer_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_align(s_ai_drawer, LV_ALIGN_BOTTOM_MID, 0, -UI_AI_DRAWER_BOTTOM);
}

static void start_eq_anims(chat_phase_t phase)
{
    stop_eq_anims();
    if (phase != CHAT_LISTEN && phase != CHAT_SPEAK && phase != CHAT_CONNECT) {
        return;
    }
    /* 禁止无限 anim：RGB full_refresh 抢带宽 → 语音 Opus 断续；仅静态矮条示意相位 */
    uint32_t color = UI_C_ACCENT;
    int32_t h0 = 14;
    if (phase == CHAT_SPEAK) {
        color = UI_C_ACCENT_SOFT;
        h0 = 18;
    } else if (phase == CHAT_CONNECT) {
        color = UI_C_WARN;
        h0 = 12;
    }
    for (int i = 0; i < 3; i++) {
        if (!s_eq_bars[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(color), 0);
        lv_obj_set_height(s_eq_bars[i], h0 - (i * 3));
    }
}



static chat_phase_t phase_from_status(const char *status)
{
    if (!status) {
        return CHAT_IDLE;
    }
    if (strstr(status, "聆听") || strstr(status, "Listening") || strstr(status, "listening")) {
        return CHAT_LISTEN;
    }
    if (strstr(status, "说话") || strstr(status, "Speaking") || strstr(status, "speaking")) {
        return CHAT_SPEAK;
    }
    /* 仅建链进度文案，勿匹配「连接成功 / 已连接」 */
    if (strstr(status, "连接中") || strstr(status, "Connecting") || strstr(status, "登录") ||
        strstr(status, "正在恢复网络") || strstr(status, "唤醒网络") || strstr(status, "入网中") ||
        strstr(status, "获取地址") || strstr(status, "接通语音") || strstr(status, "网络未就绪") ||
        strstr(status, "本地控家")) {
        return strstr(status, "本地控家") ? CHAT_LISTEN : CHAT_CONNECT;
    }
    if (strstr(status, "待命") || strstr(status, "Standby") || strstr(status, "standby")) {
        return CHAT_IDLE;
    }
    return CHAT_IDLE;
}

static bool status_is_phase(const char *status)
{
    if (!status) {
        return false;
    }
    return strstr(status, "聆听") || strstr(status, "说话") || strstr(status, "待命") ||
           strstr(status, "连接中") || strstr(status, "Listening") || strstr(status, "Speaking") ||
           strstr(status, "Standby") || strstr(status, "Connecting") || strstr(status, "登录") ||
           strstr(status, "正在恢复网络") || strstr(status, "唤醒网络") || strstr(status, "入网中") ||
           strstr(status, "获取地址") || strstr(status, "接通语音") || strstr(status, "网络未就绪") ||
           strstr(status, "本地控家");
}

/** 状态栏时钟 "HH:MM" —— 不写进对话上区 caption */
static bool status_is_clock(const char *status)
{
    if (!status || strlen(status) < 4 || strlen(status) > 5) {
        return false;
    }
    /* 允许 H:MM / HH:MM */
    const char *colon = strchr(status, ':');
    if (!colon || colon == status || colon[1] == '\0' || colon[2] == '\0' || colon[3] != '\0') {
        return false;
    }
    for (const char *p = status; *p; p++) {
        if (p != colon && (*p < '0' || *p > '9')) {
            return false;
        }
    }
    return true;
}

static void cancel_connect_delay_timer(void)
{
    if (s_connect_delay_timer) {
        lv_timer_del(s_connect_delay_timer);
        s_connect_delay_timer = nullptr;
    }
    s_connect_pending_status[0] = '\0';
    s_connect_pending_body[0] = '\0';
    s_connect_pending_has_body = false;
    s_connect_delay_force = false;
}

void nl_ov_hide_ai_drawer(void)
{
    cancel_connect_delay_timer();
    s_ai_minimized = false;
    hide_ai_mini_fab();
    if (s_ai_drawer) {
        lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
        apply_drawer_chrome(false); /* 收起时恢复全高，避免下次卡在紧凑条 */
    }
    stop_eq_anims();
    if (s_ai_hide_timer) {
        lv_timer_del(s_ai_hide_timer);
        s_ai_hide_timer = nullptr;
    }
}

void nl_ov_create_ai_drawer(lv_obj_t *parent)
{
    if (s_ai_drawer || !parent) {
        return;
    }
    s_ai_drawer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ai_drawer);
    lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_FLOATING);
    /* 收窄居中胶囊，两侧留白，避免贴边通栏 */
    lv_obj_set_size(s_ai_drawer, UI_AI_DRAWER_W, UI_AI_DRAWER_H);
    lv_obj_align(s_ai_drawer, LV_ALIGN_BOTTOM_MID, 0, -UI_AI_DRAWER_BOTTOM);
    lv_obj_set_style_bg_color(s_ai_drawer, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_ai_drawer, UI_AI_DRAWER_BG_OPA, 0);
    lv_obj_set_style_radius(s_ai_drawer, 20, 0);
    lv_obj_set_style_border_width(s_ai_drawer, 1, 0);
    lv_obj_set_style_border_color(s_ai_drawer, UI_INFO, 0);
    lv_obj_set_style_border_opa(s_ai_drawer, 72, 0);
    lv_obj_set_style_shadow_width(s_ai_drawer, 24, 0);
    lv_obj_set_style_shadow_offset_y(s_ai_drawer, 8, 0);
    lv_obj_set_style_shadow_color(s_ai_drawer, UI_BG, 0);
    lv_obj_set_style_shadow_opa(s_ai_drawer, 100, 0);
    lv_obj_set_style_pad_left(s_ai_drawer, 14, 0);
    lv_obj_set_style_pad_right(s_ai_drawer, 10, 0);
    lv_obj_set_style_pad_top(s_ai_drawer, 8, 0);
    lv_obj_set_style_pad_bottom(s_ai_drawer, 12, 0);
    lv_obj_set_style_pad_row(s_ai_drawer, 6, 0);
    lv_obj_set_flex_flow(s_ai_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_SCROLLABLE);
    /* 整卡可点：点空处也不穿透到下层设备卡 */
    lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);

    /* Header：声波 + 状态 · 收起/关闭（44px 触控） */
    lv_obj_t *hdr = lv_obj_create(s_ai_drawer);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), UI_AI_DRAWER_HDR_H);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *left_box = lv_obj_create(hdr);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left_box, 1);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 10, 0);
    lv_obj_set_style_pad_right(left_box, 8, 0);
    lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(left_box, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 3; i++) {
        s_eq_bars[i] = lv_obj_create(left_box);
        lv_obj_remove_style_all(s_eq_bars[i]);
        lv_obj_set_size(s_eq_bars[i], 3, 10);
        lv_obj_set_style_radius(s_eq_bars[i], 2, 0);
        lv_obj_set_style_bg_color(s_eq_bars[i], UI_ACCENT, 0);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_eq_bars[i], LV_OBJ_FLAG_CLICKABLE);
    }

    s_ai_drawer_status = lv_label_create(left_box);
    nl_ov_set_font(s_ai_drawer_status);
    lv_label_set_text(s_ai_drawer_status, "小智在听…");
    lv_obj_set_style_text_color(s_ai_drawer_status, UI_INFO, 0);
    lv_obj_clear_flag(s_ai_drawer_status, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hdr_btns = lv_obj_create(hdr);
    lv_obj_remove_style_all(hdr_btns);
    lv_obj_set_size(hdr_btns, LV_SIZE_CONTENT, UI_AI_DRAWER_BTN_SZ);
    lv_obj_set_flex_flow(hdr_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr_btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr_btns, 8, 0);
    lv_obj_clear_flag(hdr_btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hdr_btns, LV_OBJ_FLAG_CLICKABLE);

    auto make_hdr_btn = [](lv_obj_t *parent, const char *sym, lv_event_cb_t cb) -> lv_obj_t * {
        lv_obj_t *btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, UI_AI_DRAWER_BTN_SZ, UI_AI_DRAWER_BTN_SZ);
        lv_obj_set_style_radius(btn, UI_AI_DRAWER_BTN_SZ / 2, 0);
        lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(btn, 1, LV_STATE_PRESSED);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(btn, 12);
        lv_obj_t *ic = lv_label_create(btn);
        lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
        lv_label_set_text(ic, sym);
        lv_obj_set_style_text_color(ic, UI_TEXT_SUB, 0);
        lv_obj_center(ic);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        return btn;
    };

    /* 最小化：会话不停，右下角只留图标；文字不显示 */
    make_hdr_btn(hdr_btns, MATERIAL_SYMBOLS_KEYBOARD_ARROW_DOWN, [](lv_event_t *) {
        minimize_ai_drawer();
    });
    make_hdr_btn(hdr_btns, MATERIAL_SYMBOLS_CLOSE, [](lv_event_t *) {
        /* 先锁住：说话中关窗后，回复字幕/相位刷新不得把抽屉再拉出来 */
        s_ai_user_dismissed = true;
        nl_ov_hide_ai_drawer();
        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            Application::GetInstance().ToggleChatState();
        }
    });

    /* 正文区固定高度，可滚；标签按内容增高，展示最近 5 句 */
    s_ai_drawer_body = lv_obj_create(s_ai_drawer);
    lv_obj_remove_style_all(s_ai_drawer_body);
    lv_obj_set_size(s_ai_drawer_body, lv_pct(100), UI_AI_DRAWER_TEXT_H);
    lv_obj_set_style_pad_hor(s_ai_drawer_body, 2, 0);
    lv_obj_add_flag(s_ai_drawer_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_ai_drawer_body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_ai_drawer_body, LV_DIR_VER);

    s_ai_drawer_text = lv_label_create(s_ai_drawer_body);
    lv_label_set_text(s_ai_drawer_text, "说唤醒词「你好小智」或吩咐指令…");
    lv_obj_set_width(s_ai_drawer_text, lv_pct(100));
    lv_label_set_long_mode(s_ai_drawer_text, LV_LABEL_LONG_WRAP);
    nl_ov_set_font(s_ai_drawer_text);
    lv_obj_set_style_text_color(s_ai_drawer_text, UI_TEXT, 0);
    lv_obj_set_style_text_line_space(s_ai_drawer_text, 4, 0);
    lv_obj_align(s_ai_drawer_text, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 最小化圆钮：挂 layer_top，右下角仅 mic 图标（避开播放器 mini 最右位） */
    s_ai_mini_fab = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ai_mini_fab);
    lv_obj_set_size(s_ai_mini_fab, UI_AI_MINI_SZ, UI_AI_MINI_SZ);
    lv_obj_align(s_ai_mini_fab, LV_ALIGN_BOTTOM_RIGHT, -(UI_AI_MINI_RIGHT + UI_AI_MINI_SZ + 10),
                 -(NL_UI_TAB_H + 14));
    lv_obj_set_style_radius(s_ai_mini_fab, UI_AI_MINI_SZ / 2, 0);
    lv_obj_set_style_bg_color(s_ai_mini_fab, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(s_ai_mini_fab, UI_AI_DRAWER_BG_OPA, 0);
    lv_obj_set_style_border_width(s_ai_mini_fab, 2, 0);
    lv_obj_set_style_border_color(s_ai_mini_fab, UI_INFO, 0);
    lv_obj_set_style_border_opa(s_ai_mini_fab, 200, 0);
    lv_obj_set_style_shadow_width(s_ai_mini_fab, 14, 0);
    lv_obj_set_style_shadow_opa(s_ai_mini_fab, LV_OPA_40, 0);
    lv_obj_set_style_shadow_color(s_ai_mini_fab, UI_INFO, 0);
    lv_obj_add_flag(s_ai_mini_fab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_ai_mini_fab, 10);
    lv_obj_set_style_bg_color(s_ai_mini_fab, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(s_ai_mini_fab, 1, LV_STATE_PRESSED);
    lv_obj_add_flag(s_ai_mini_fab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ai_mini_fab, [](lv_event_t *) { expand_ai_drawer(); }, LV_EVENT_CLICKED,
                        nullptr);

    s_ai_mini_ic = lv_label_create(s_ai_mini_fab);
    lv_obj_set_style_text_font(s_ai_mini_ic, &font_material_symbols_20_4, 0);
    lv_label_set_text(s_ai_mini_ic, MATERIAL_SYMBOLS_MIC);
    lv_obj_set_style_text_color(s_ai_mini_ic, UI_INFO, 0);
    lv_obj_center(s_ai_mini_ic);
    lv_obj_clear_flag(s_ai_mini_ic, LV_OBJ_FLAG_CLICKABLE);
}

static const char *chat_tail_lines(const char *src, int n)
{
    if (!src || !src[0] || n <= 0) {
        return "";
    }
    const char *p = src + strlen(src);
    int newlines = 0;
    while (p > src && newlines < n) {
        --p;
        if (*p == '\n') {
            ++newlines;
        }
    }
    if (newlines == n) {
        return p + 1; /* 落在从尾数第 n 个换行上，从其后开始 */
    }
    return src;
}

static void connect_delay_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_connect_delay_timer = nullptr;
    if (!s_ai_drawer || !lv_obj_has_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN)) {
        s_connect_pending_status[0] = '\0';
        s_connect_pending_body[0] = '\0';
        s_connect_pending_has_body = false;
        return;
    }
    s_connect_pending_status[0] = '\0';
    s_connect_pending_body[0] = '\0';
    s_connect_pending_has_body = false;
    s_connect_delay_force = true;
    nl_ov_show_ai_drawer_locked(CHAT_CONNECT, "初始化中，请稍后...", nullptr);
}

static void arm_connect_delay(const char *status_text, const char *msg_text)
{
    (void)status_text;
    (void)msg_text;
    nl_ov_utf8_copy(s_connect_pending_status, sizeof(s_connect_pending_status),
                    "初始化中，请稍后...");
    s_connect_pending_body[0] = '\0';
    s_connect_pending_has_body = false;
    if (!s_connect_delay_timer) {
        s_connect_delay_timer = lv_timer_create(connect_delay_timer_cb, CONNECT_SHOW_DELAY_MS, nullptr);
        lv_timer_set_repeat_count(s_connect_delay_timer, 1);
    }
}

void nl_ov_show_ai_drawer_locked(chat_phase_t phase, const char *status_text, const char *msg_text)
{
    if (!s_ai_drawer) {
        return;
    }
    /* 技能结果卡优先：只更新文案，不弹抽屉，并保持结果卡在最前 */
    if (s_skill_ui_active) {
        if (phase == CHAT_IDLE) {
            nodelio_skill_result_raise();
            return;
        }
        if (msg_text && msg_text[0] && s_ai_drawer_text) {
            nl_ov_utf8_copy(s_ai_show_text, sizeof(s_ai_show_text), msg_text);
            lv_label_set_text(s_ai_drawer_text, s_ai_show_text);
        }
        if (status_text && status_text[0] && s_ai_drawer_status) {
            lv_label_set_text(s_ai_drawer_status, status_text);
        }
        s_chat_phase = phase;
        if (s_ai_drawer) {
            lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
        }
        hide_ai_mini_fab(); /* 最小化态暂藏，关技能卡后 restore 再出 */
        nodelio_skill_result_raise();
        return;
    }
    if (phase == CHAT_CONNECT) {
        s_ai_farewell_closing = false;
        s_ai_user_dismissed = false;
    } else if (s_ai_user_dismissed && phase != CHAT_IDLE) {
        /* 用户已关窗：本轮听/说只改文案，不再弹出 */
        if (msg_text && msg_text[0] && s_ai_drawer_text) {
            lv_label_set_text(s_ai_drawer_text, msg_text);
        }
        if (status_text && status_text[0] && s_ai_drawer_status) {
            lv_label_set_text(s_ai_drawer_status, status_text);
        }
        s_chat_phase = phase;
        lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
        return;
    } else if (s_ai_farewell_closing && phase != CHAT_IDLE) {
        /* 告别收束中：listening/speaking 尾包勿再弹 */
        nl_ov_hide_ai_drawer();
        return;
    }
    if (phase == CHAT_IDLE) {
        s_ai_user_dismissed = false;
        if (s_speak_text_dirty && s_ai_drawer_text && s_speak_text_pending[0]) {
            lv_label_set_text(s_ai_drawer_text, s_speak_text_pending);
            s_speak_text_dirty = false;
        }
        nl_ov_hide_ai_drawer();
        return;
    }
    /* 离开说话相位时刷出被限频的最后一句 */
    if (phase != CHAT_SPEAK && s_speak_text_dirty && s_ai_drawer_text && s_speak_text_pending[0]) {
        lv_label_set_text(s_ai_drawer_text, s_speak_text_pending);
        s_speak_text_dirty = false;
        s_speak_text_last_us = 0;
    }

    s_chat_phase = phase;
    uint32_t border_color = UI_C_INFO;
    uint32_t status_color = UI_C_INFO;
    const char *st_def = "小智在听…";
    if (phase == CHAT_SPEAK) {
        border_color = UI_C_TILE_ON_BORDER;
        status_color = UI_C_ACCENT_SOFT;
        st_def = "小智回复";
    } else if (phase == CHAT_CONNECT) {
        border_color = UI_C_WARN;
        status_color = UI_C_WARN;
        st_def = "初始化中，请稍后...";
    }

    /* 最小化：只刷隐藏抽屉文案 + 右下角图标色，禁止再展开 */
    if (s_ai_minimized) {
        cancel_connect_delay_timer();
        if (s_ai_drawer_status) {
            const char *st = (phase == CHAT_CONNECT)
                                 ? "初始化中，请稍后..."
                                 : (status_text && status_text[0] ? status_text : st_def);
            lv_label_set_text(s_ai_drawer_status, st);
            lv_obj_set_style_text_color(s_ai_drawer_status, lv_color_hex(status_color), 0);
        }
        if (s_ai_drawer_text && phase != CHAT_CONNECT && msg_text && msg_text[0]) {
            lv_label_set_text(s_ai_drawer_text, msg_text);
        }
        lv_obj_set_style_border_color(s_ai_drawer, lv_color_hex(border_color), 0);
        lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
        show_ai_mini_fab(phase);
        if (s_ai_hide_timer) {
            lv_timer_del(s_ai_hide_timer);
            s_ai_hide_timer = nullptr;
        }
        return;
    }

    /* 隐藏时仅连接中/倾听/说话可弹出 */
    if (lv_obj_has_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN)) {
        if (phase != CHAT_LISTEN && phase != CHAT_SPEAK && phase != CHAT_CONNECT) {
            return;
        }
        /* CONNECT：晚 1s 再弹紧凑「唤醒中」条；听/说提前到则取消 */
        if (phase == CHAT_CONNECT && !s_connect_delay_force) {
            arm_connect_delay(status_text, msg_text);
            return;
        }
        s_connect_delay_force = false;
        if (phase != CHAT_CONNECT) {
            cancel_connect_delay_timer();
        }
    } else {
        s_connect_delay_force = false;
        if (phase != CHAT_CONNECT) {
            cancel_connect_delay_timer();
        }
    }

    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_drawer);
    hide_ai_mini_fab();

    apply_drawer_chrome(phase == CHAT_CONNECT);

    lv_obj_set_style_border_color(s_ai_drawer, lv_color_hex(border_color), 0);
    if (s_ai_drawer_status) {
        /* CONNECT 固定省电唤醒文案，不跟入网/DHCP 文案乱跳 */
        const char *st = (phase == CHAT_CONNECT)
                             ? "初始化中，请稍后..."
                             : (status_text && status_text[0] ? status_text : st_def);
        lv_label_set_text(s_ai_drawer_status, st);
        lv_obj_set_style_text_color(s_ai_drawer_status, lv_color_hex(status_color), 0);
    }

    if (s_ai_drawer_text && phase != CHAT_CONNECT) {
        const char *body = msg_text;
        if (body && body[0]) {
            bool apply = true;
            if (phase == CHAT_SPEAK) {
                int64_t now = esp_timer_get_time();
                if (s_speak_text_last_us > 0 && (now - s_speak_text_last_us) < 400000) {
                    nl_ov_utf8_copy(s_speak_text_pending, sizeof(s_speak_text_pending), body);
                    s_speak_text_dirty = true;
                    apply = false;
                } else {
                    s_speak_text_last_us = now;
                    s_speak_text_dirty = false;
                }
            } else {
                s_speak_text_last_us = 0;
                s_speak_text_dirty = false;
            }
            if (apply) {
                lv_label_set_text(s_ai_drawer_text, body);
                lv_obj_set_width(s_ai_drawer_text, lv_pct(100));
                lv_label_set_long_mode(s_ai_drawer_text, LV_LABEL_LONG_WRAP);
                if (s_ai_drawer_body) {
                    lv_obj_scroll_to_y(s_ai_drawer_body, LV_COORD_MAX, LV_ANIM_OFF);
                }
            }
        }
    }

    start_eq_anims(phase);

    if (s_ai_hide_timer) {
        lv_timer_del(s_ai_hide_timer);
        s_ai_hide_timer = nullptr;
    }
}

void nl_ov_show_ai_drawer_session(chat_phase_t phase)
{
    const char *status = "小智在听…";
    if (phase == CHAT_SPEAK) {
        status = "小智回复";
    } else if (phase == CHAT_CONNECT) {
        status = "初始化中，请稍后...";
    } else if (phase == CHAT_IDLE) {
        status = "待命";
    }

    const char *body = "说唤醒词「你好小智」或吩咐指令…";
    if (phase == CHAT_CONNECT) {
        body = nullptr; /* 紧凑条无正文 */
    } else if (s_chat_text[0]) {
        nl_ov_utf8_copy(s_ai_show_text, sizeof(s_ai_show_text),
                        chat_tail_lines(s_chat_text, NL_CHAT_SHOW_LINES));
        body = s_ai_show_text;
    }
    nl_ov_show_ai_drawer_locked(phase, status, body);
}

static void append_chat_line(const char *role, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    const char *tag = "助手";
    if (role && strcmp(role, "user") == 0) {
        tag = "我";
    } else if (role && strcmp(role, "system") == 0) {
        tag = "系统";
    }
    char line[192];
    std::snprintf(line, sizeof(line), "%s：%s", tag, text);
    if (s_chat_text[0] == '\0') {
        nl_ov_utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    size_t a = strlen(s_chat_text);
    size_t b = strlen(line);
    if (a + 1 + b + 1 <= sizeof(s_chat_text)) {
        s_chat_text[a] = '\n';
        memcpy(s_chat_text + a + 1, line, b + 1);
        return;
    }
    size_t max_old = (b + 2 < sizeof(s_chat_text)) ? (sizeof(s_chat_text) - 2 - b) : 0;
    if (max_old == 0) {
        nl_ov_utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    const char *p = s_chat_text;
    if (a > max_old) {
        p = s_chat_text + (a - max_old);
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }
    size_t keep = strlen(p);
    if (keep + 1 + b + 1 > sizeof(s_chat_text)) {
        nl_ov_utf8_copy(s_chat_text, sizeof(s_chat_text), line);
        return;
    }
    memmove(s_chat_text, p, keep);
    s_chat_text[keep] = '\n';
    memcpy(s_chat_text + keep + 1, line, b + 1);
}

void nodelio_overlay_on_chat_locked(const char *role, const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    append_chat_line(role, text);

    /* 系统通知/版本输出仅记入记录，绝不弹出抽屉 */
    if (!role || (strcmp(role, "user") != 0 && strcmp(role, "assistant") != 0)) {
        return;
    }

    if (strcmp(role, "user") == 0) {
        s_ai_farewell_closing = false;
        s_ai_user_dismissed = false; /* 用户又开口：允许再显示 */
    } else if (s_ai_user_dismissed) {
        return; /* 关掉之后的回复尾句不再弹出 */
    } else if (s_ai_farewell_closing) {
        /* 告别后的「…」等尾句只入账，不再弹 */
        return;
    }

    /* 抽屉展示最近 NL_CHAT_SHOW_LINES 句（含角色前缀），不再只刷最新一句 */
    chat_phase_t ph = (strcmp(role, "assistant") == 0) ? CHAT_SPEAK : CHAT_LISTEN;
    const char *status_str = (strcmp(role, "assistant") == 0) ? "小智回复" : "我说：";
    nl_ov_utf8_copy(s_ai_show_text, sizeof(s_ai_show_text),
              chat_tail_lines(s_chat_text, NL_CHAT_SHOW_LINES));

    nl_ov_show_ai_drawer_locked(ph, status_str, s_ai_show_text);

    /* 告别语义：收起并锁住，避免 … / listening / 待命 再闪 */
    if (strcmp(role, "assistant") == 0 && text_is_farewell(text)) {
        s_ai_farewell_closing = true;
        nl_ov_hide_ai_drawer();
    }
}

void nodelio_overlay_on_status_locked(const char *status)
{
    if (!status || !status[0] || status_is_clock(status)) {
        return;
    }
    /* 只跟会话相位；检查更新/登录等杂讯不进对话 */
    if (status_is_phase(status)) {
        chat_phase_t ph = phase_from_status(status);
        if (ph == CHAT_IDLE) {
            /* 退下/回 idle：直接收起，勿再闪一次「待命」 */
            nl_ov_hide_ai_drawer();
            return;
        }
        if (s_ai_farewell_closing && ph != CHAT_CONNECT) {
            /* 告别收束：挡 listening，但允许下次唤醒建链弹窗 */
            nl_ov_hide_ai_drawer();
            return;
        }
        nl_ov_show_ai_drawer_locked(ph, status, nullptr);
    }
}
