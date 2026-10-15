#include "overlay_priv.h"

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

static bool text_is_farewell(const char *text)
{
    if (!text || !text[0]) {
        return false;
    }
    return strstr(text, "再见") || strstr(text, "退下") || strstr(text, "拜拜") ||
           strstr(text, "退出") || strstr(text, "我先休息了") || strstr(text, "随时叫我");
}

bool nl_ov_ai_farewell_closing(void)
{
    return s_ai_farewell_closing;
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

/** 建链状态 → 正文：让 1~2s 等待看上去在推进，而非死句「请稍候」 */
static const char *connect_body_for_status(const char *status)
{
    if (!status || !status[0]) {
        return "正在唤醒无线，马上就好…";
    }
    if (strstr(status, "恢复网络") || strstr(status, "唤醒网络")) {
        return "熄屏后正在唤醒无线…";
    }
    if (strstr(status, "入网")) {
        return "正在联系路由器…";
    }
    if (strstr(status, "获取地址") || strstr(status, "地址")) {
        return "路由器已应答，正在获取地址…";
    }
    if (strstr(status, "接通语音") || strstr(status, "登录")) {
        return "网络已就绪，正在接通语音…";
    }
    if (strstr(status, "网络未就绪")) {
        return "暂时连不上网，可先用本地控家…";
    }
    if (strstr(status, "连接中") || strstr(status, "Connecting")) {
        return "正在建立安全通道…";
    }
    return "正在准备通话…";
}

static void start_eq_anims(chat_phase_t phase)
{
    stop_eq_anims();
    if (phase != CHAT_LISTEN && phase != CHAT_SPEAK && phase != CHAT_CONNECT) {
        return;
    }
    /* 说话期禁止无限 anim：RGB full_refresh 会整屏刷，抢 PSRAM 导致 Opus 断续 */
    if (phase == CHAT_SPEAK) {
        for (int i = 0; i < 3; i++) {
            if (!s_eq_bars[i]) {
                continue;
            }
            lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(UI_C_ACCENT_SOFT), 0);
            lv_obj_set_height(s_eq_bars[i], 22 - (i * 4));
        }
        return;
    }
    const bool connect = (phase == CHAT_CONNECT);
    const uint32_t color = connect ? UI_C_WARN : UI_C_ACCENT;
    /* 建链：偏慢轻跳，营造「在干活」；聆听：稍快 */
    const int32_t hi = connect ? 28 : 36;
    const uint32_t period = connect ? 620 : 480;
    for (int i = 0; i < 3; i++) {
        if (!s_eq_bars[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(s_eq_bars[i], lv_color_hex(color), 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_eq_bars[i]);
        lv_anim_set_values(&a, 8, hi - (i * 4));
        lv_anim_set_duration(&a, period + (uint32_t)i * 70);
        lv_anim_set_reverse_duration(&a, period + (uint32_t)i * 50);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_set_exec_cb(&a, eq_anim_exec);
        lv_anim_set_delay(&a, (uint32_t)i * 90);
        lv_anim_start(&a);
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

void nl_ov_hide_ai_drawer(void)
{
    if (s_ai_drawer) {
        lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_bg_opa(s_ai_drawer, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ai_drawer, 20, 0);
    lv_obj_set_style_border_width(s_ai_drawer, 1, 0);
    lv_obj_set_style_border_color(s_ai_drawer, UI_INFO, 0);
    lv_obj_set_style_border_opa(s_ai_drawer, 72, 0);
    lv_obj_set_style_shadow_width(s_ai_drawer, 32, 0);
    lv_obj_set_style_shadow_offset_y(s_ai_drawer, 10, 0);
    lv_obj_set_style_shadow_color(s_ai_drawer, UI_BG, 0);
    lv_obj_set_style_shadow_opa(s_ai_drawer, 160, 0);
    lv_obj_set_style_pad_left(s_ai_drawer, 14, 0);
    lv_obj_set_style_pad_right(s_ai_drawer, 12, 0);
    lv_obj_set_style_pad_top(s_ai_drawer, 10, 0);
    lv_obj_set_style_pad_bottom(s_ai_drawer, 12, 0);
    lv_obj_set_style_pad_row(s_ai_drawer, 6, 0);
    lv_obj_set_flex_flow(s_ai_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);

    /* Header：声波 + 状态 · 收起 */
    lv_obj_t *hdr = lv_obj_create(s_ai_drawer);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 28);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left_box = lv_obj_create(hdr);
    lv_obj_remove_style_all(left_box);
    lv_obj_set_size(left_box, LV_SIZE_CONTENT, 24);
    lv_obj_set_flex_flow(left_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left_box, 6, 0);
    lv_obj_clear_flag(left_box, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        s_eq_bars[i] = lv_obj_create(left_box);
        lv_obj_remove_style_all(s_eq_bars[i]);
        lv_obj_set_size(s_eq_bars[i], 3, 10);
        lv_obj_set_style_radius(s_eq_bars[i], 2, 0);
        lv_obj_set_style_bg_color(s_eq_bars[i], UI_ACCENT, 0);
        lv_obj_set_style_bg_opa(s_eq_bars[i], LV_OPA_COVER, 0);
    }

    s_ai_drawer_status = lv_label_create(left_box);
    nl_ov_set_font(s_ai_drawer_status);
    lv_label_set_text(s_ai_drawer_status, "小智在听…");
    lv_obj_set_style_text_color(s_ai_drawer_status, UI_INFO, 0);

    lv_obj_t *close_btn = lv_obj_create(hdr);
    lv_obj_remove_style_all(close_btn);
    lv_obj_set_size(close_btn, 44, 26);
    lv_obj_set_style_radius(close_btn, 13, 0);
    lv_obj_set_style_bg_color(close_btn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(close_btn, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(close_btn, 1, LV_STATE_PRESSED);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close_btn, UI_EXT_CLICK);

    lv_obj_t *close_ic = lv_label_create(close_btn);
    nl_ov_set_icon_font(close_ic);
    lv_label_set_text(close_ic, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_ic, UI_TEXT_SUB, 0);
    lv_obj_center(close_ic);
    lv_obj_clear_flag(close_ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(close_ic, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(
        close_btn,
        [](lv_event_t *e) {
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
                nl_ov_hide_ai_drawer();
                auto state = Application::GetInstance().GetDeviceState();
                if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                    Application::GetInstance().ToggleChatState();
                }
            }
        },
        LV_EVENT_ALL, nullptr);

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

void nl_ov_show_ai_drawer_locked(chat_phase_t phase, const char *status_text, const char *msg_text)
{
    if (!s_ai_drawer) {
        return;
    }
    if (phase == CHAT_CONNECT) {
        s_ai_farewell_closing = false;
    } else if (s_ai_farewell_closing && phase != CHAT_IDLE) {
        /* 告别收束中：listening/speaking 尾包勿再弹 */
        nl_ov_hide_ai_drawer();
        return;
    }
    if (phase == CHAT_IDLE) {
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
    /* 开机/静默防误弹：隐藏时仅连接中/倾听/说话可弹出（先弹窗再文字体现进度） */
    if (lv_obj_has_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN)) {
        if (phase != CHAT_LISTEN && phase != CHAT_SPEAK && phase != CHAT_CONNECT) {
            return;
        }
    }
    lv_obj_clear_flag(s_ai_drawer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ai_drawer);

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
        st_def = "连接中…";
    }

    lv_obj_set_style_border_color(s_ai_drawer, lv_color_hex(border_color), 0);
    if (s_ai_drawer_status) {
        lv_label_set_text(s_ai_drawer_status, status_text && status_text[0] ? status_text : st_def);
        lv_obj_set_style_text_color(s_ai_drawer_status, lv_color_hex(status_color), 0);
    }

    if (s_ai_drawer_text) {
        const char *body = msg_text;
        if ((!body || !body[0]) && phase == CHAT_CONNECT) {
            body = connect_body_for_status(status_text);
        }
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
        status = "连接中…";
    } else if (phase == CHAT_IDLE) {
        status = "待命";
    }

    const char *body = "说唤醒词「你好小智」或吩咐指令…";
    if (phase == CHAT_CONNECT) {
        body = connect_body_for_status(status);
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
