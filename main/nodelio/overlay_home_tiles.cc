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
#include "nodelio_overlay.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include "jpg/jpeg_to_image.h"
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
#include <freertos/idf_additions.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <time.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <strings.h>

#define TAG "nl_home"

static lv_timer_t *s_cd_timer;
/** fill_home 世代：异步相册图回写时丢弃过期结果 */
static uint32_t s_home_gen;

#define NL_PHOTO_RT_MAX 8
typedef struct {
    char widget_id[NL_LAYOUT_STR];
    uint8_t idx;
    uint8_t paused;
    uint16_t interval_sec;
    int64_t next_ms;
} photo_rt_t;
static photo_rt_t s_photo_rt[NL_PHOTO_RT_MAX];
static int s_photo_rt_n;
static lv_timer_t *s_photo_timer;

static void format_countdown(int sec, char *buf, size_t buflen)
{
    if (!buf || buflen < 4) {
        return;
    }
    if (sec < 0) {
        sec = 0;
    }
    if (sec >= 3600) {
        std::snprintf(buf, buflen, "%dh%02d", sec / 3600, (sec % 3600) / 60);
    } else if (sec >= 60) {
        std::snprintf(buf, buflen, "%d:%02d", sec / 60, sec % 60);
    } else {
        std::snprintf(buf, buflen, "%ds", sec);
    }
}

/** 右上角药丸：文字在底色内水平垂直居中 */
static void style_hint_pill(lv_obj_t *hint, bool countdown)
{
    if (!hint) {
        return;
    }
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(hint, countdown ? 6 : 5, 0);
    lv_obj_set_style_pad_right(hint, countdown ? 6 : 5, 0);
    lv_obj_set_style_pad_top(hint, 3, 0);
    lv_obj_set_style_pad_bottom(hint, 3, 0);
    /* 数字倒计时略宽，避免「9:59」贴边显偏 */
    if (countdown) {
        lv_obj_set_style_min_width(hint, 36, 0);
    } else {
        lv_obj_set_style_min_width(hint, 0, 0);
    }
}

static bool any_countdown_active(void)
{
    if (!s_devs) {
        return false;
    }
    for (int i = 0; i < s_dev_n; i++) {
        if (s_devs[i].countdown > 0) {
            return true;
        }
        for (uint8_t c = 0; c < s_devs[i].ch_n; c++) {
            if (s_devs[i].ch_cd[c] > 0) {
                return true;
            }
        }
    }
    return false;
}

static void style_tile_visual(lv_obj_t *tile, const nl_widget_t *w);
/** 指令飞行中：防连点；失败/超时按 from 回滚。 */
static bool widget_is_cmd_pending(const nl_widget_t *w);

/** 倒计时 tick：只改角标文字；刚归零的磁贴单刷一次。禁止每秒全页 style_tile（RGB 全刷抢音频） */
static void countdown_tick_ui(void)
{
    if (!s_home || !s_devs) {
        return;
    }

    struct {
        lv_obj_t *tile;
        nl_widget_t *w;
    } active[24];
    int n = 0;
    const uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < cnt && n < 24; i++) {
        lv_obj_t *tile = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(tile));
        if (!w || nl_ov_widget_countdown(w) <= 0) {
            continue;
        }
        active[n].tile = tile;
        active[n].w = w;
        n++;
    }
    if (n == 0) {
        return;
    }

    for (int i = 0; i < s_dev_n; i++) {
        if (s_devs[i].countdown > 0) {
            s_devs[i].countdown--;
        }
        for (uint8_t c = 0; c < s_devs[i].ch_n; c++) {
            if (s_devs[i].ch_cd[c] > 0) {
                s_devs[i].ch_cd[c]--;
            }
        }
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *tile = active[i].tile;
        nl_widget_t *w = active[i].w;
        const int cd = nl_ov_widget_countdown(w);
        lv_obj_t *hint = nullptr;
        const uint32_t sub = lv_obj_get_child_count(tile);
        for (uint32_t j = 0; j < sub; j++) {
            lv_obj_t *ch = lv_obj_get_child(tile, j);
            if ((intptr_t)lv_obj_get_user_data(ch) == TILE_CHILD_HINT) {
                hint = ch;
                break;
            }
        }
        if (cd > 0) {
            if (!hint) {
                continue;
            }
            char hb[12];
            format_countdown(cd, hb, sizeof(hb));
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(hint, hb);
        } else {
            style_tile_visual(tile, w);
        }
    }
}

static void cd_timer_ensure(bool want)
{
    if (want) {
        if (!s_cd_timer) {
            s_cd_timer = lv_timer_create(
                [](lv_timer_t *) {
                    if (!s_screen_on || !s_devs) {
                        return;
                    }
                    if (!any_countdown_active()) {
                        if (s_cd_timer) {
                            lv_timer_pause(s_cd_timer);
                        }
                        return;
                    }
                    countdown_tick_ui();
                    if (!any_countdown_active() && s_cd_timer) {
                        lv_timer_pause(s_cd_timer);
                    }
                },
                1000, nullptr);
        } else {
            lv_timer_resume(s_cd_timer);
        }
    } else if (s_cd_timer) {
        lv_timer_pause(s_cd_timer);
    }
}

static void short_label(const char *in, char *out, size_t out_len, size_t max_runes = 3)
{
    if (!in || !out || out_len < 4) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    size_t in_len = strlen(in);
    size_t rune_count = 0;
    size_t n = 0;
    while (in[n] && n < in_len) {
        unsigned char c = (unsigned char)in[n];
        size_t step = 1;
        if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        if (n + step > in_len || n + step >= out_len) {
            break;
        }
        if (rune_count >= max_runes) {
            break;
        }
        memcpy(out + n, in + n, step);
        n += step;
        rune_count++;
    }
    out[n] = '\0';
    /* 超长由 LV_LABEL_LONG_DOT 按像素打点，此处不再硬拼 '.' */
}

/** 按占格给标题字数：与 APP displayTitle 对齐；大方块放宽，交给宽度打点 */
static size_t title_max_runes(const nl_widget_t *w)
{
    if (!w) {
        return 4;
    }
    const int gw = w->gw > 0 ? w->gw : 1;
    const int gh = w->gh > 0 ? w->gh : 1;
    if (gw <= 1 && gh <= 1) {
        return 0;
    }
    if (gw >= 2 && gh >= 2) {
        return 10; /* 2×2+：厨房热水等 */
    }
    if (gh <= 1) {
        if (gw >= 4) {
            return 10;
        }
        if (gw >= 3) {
            return 8;
        }
        return 6; /* 2×1 横条 */
    }
    return 4; /* 1×N 瘦高 */
}

static bool widget_is_dual_line(const nl_widget_t *w)
{
    return w && w->skin[0] && strcmp(w->skin, "dual_line") == 0;
}

/** 2×2+ 或 ≥3 宽横条：用互斥双段按钮；窄卡仍整点翻转 */
static bool dual_line_use_segments(const nl_widget_t *w)
{
    if (!widget_is_dual_line(w)) {
        return false;
    }
    const int gw = w->gw > 0 ? w->gw : 1;
    const int gh = w->gh > 0 ? w->gh : 1;
    if (gw >= 2 && gh >= 2) {
        return true;
    }
    return (gw >= 3 && gh <= 1);
}

/** labels.off=NC（关态导通）、labels.on=NO（开态导通），对齐 layout-schema */
static void dual_branch_names(const nl_widget_t *w, const char **off_name, const char **on_name)
{
    if (off_name) {
        *off_name = (w && w->labels_off[0]) ? w->labels_off : "NC";
    }
    if (on_name) {
        *on_name = (w && w->labels_on[0]) ? w->labels_on : "NO";
    }
}

static tile_kind_t tile_kind_of(const nl_widget_t *w)
{
    if (w->type == NL_WIDGET_SLIDER_TILE ||
        (w->bind.kind == NL_BIND_DEVICE_PIN && strcmp(w->bind.capability, "pwm") == 0)) {
        return TILE_PWM;
    }
    if (w->type == NL_WIDGET_SCENE_CHIP || w->type == NL_WIDGET_SCENE_GRID ||
        w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return TILE_SCENE;
    }
    if (w->type == NL_WIDGET_PIN_TOGGLE || w->bind.kind == NL_BIND_DEVICE_PIN) {
        return TILE_GPIO;
    }
    return TILE_SWITCH;
}

static uint32_t kind_fill_on(tile_kind_t kind)
{
    switch (kind) {
    case TILE_SCENE:
        return UI_C_TILE_SCENE;
    case TILE_GPIO:
        return UI_C_TILE_GPIO;
    case TILE_PWM:
        return UI_C_TILE_PWM;
    default:
        return UI_C_TILE_SWITCH;
    }
}

static uint32_t kind_fill_off(tile_kind_t kind)
{
    (void)kind;
    /* 关态统一石板：不跟自定义底色，开/关一眼可辨 */
    return UI_C_TILE_OFF;
}

static const char *kind_symbol(const nl_widget_t *w, tile_kind_t kind, bool on)
{
    (void)on;
    /* catalog id 命中当前字库才用；未知或尚未编进字体的字形回退 skin/kind */
    if (w && w->icon[0]) {
        const char *named = material_symbols_get_utf8(w->icon);
        if (named != NULL) {
            return named;
        }
    }
    if (w && w->skin[0]) {
        if (strcmp(w->skin, "fan") == 0) {
            return MATERIAL_SYMBOLS_MODE_FAN;
        }
        if (strcmp(w->skin, "dimmer") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        }
        if (strcmp(w->skin, "heater") == 0) {
            return MATERIAL_SYMBOLS_DEVICE_THERMOSTAT;
        }
        if (strcmp(w->skin, "momentary") == 0) {
            return MATERIAL_SYMBOLS_NOTIFICATIONS;
        }
        if (strcmp(w->skin, "dual_line") == 0) {
            return MATERIAL_SYMBOLS_REPEAT;
        }
        if (strcmp(w->skin, "gauge") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        }
        if (strcmp(w->skin, "tank") == 0) {
            return MATERIAL_SYMBOLS_WATER_DROP;
        }
    }
    switch (kind) {
    case TILE_SCENE:
        return MATERIAL_SYMBOLS_PLAY_ARROW;
    case TILE_PWM:
        return MATERIAL_SYMBOLS_BRIGHTNESS_6;
    default:
        /* 灯具、继电器、插座等标准开关：统一使用高雅经典的 Power 电源键符号 */
        return MATERIAL_SYMBOLS_POWER_SETTINGS_NEW;
    }
}

static bool parse_bg_hex(const char *s, uint32_t *out)
{
    if (!s || !out || s[0] != '#') {
        return false;
    }
    uint32_t v = 0;
    for (int i = 1; i <= 6; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            return false;
        }
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static uint32_t color_pressed(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xff) * 200 / 255;
    uint32_t g = ((c >> 8) & 0xff) * 200 / 255;
    uint32_t b = (c & 0xff) * 200 / 255;
    return (r << 16) | (g << 8) | b;
}

nl_dev_cache_t *nl_ov_find_dev(const char *device_id)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs || !device_id || !device_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_dev_n; i++) {
        if (strcmp(s_devs[i].id, device_id) == 0) {
            return &s_devs[i];
        }
    }
    return nullptr;
}

static nl_dev_cache_t *ensure_dev(const char *device_id)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs) {
        return nullptr;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (d) {
        return d;
    }
    if (!device_id || !device_id[0] || s_dev_n >= NL_DEV_CACHE_MAX) {
        return nullptr;
    }
    d = &s_devs[s_dev_n++];
    std::memset(d, 0, sizeof(*d));
    std::snprintf(d->id, sizeof(d->id), "%s", device_id);
    d->online = true;
    return d;
}

static bool state_is_unknown(const char *st)
{
    return !st || !st[0] || strcasecmp(st, "UNKNOWN") == 0;
}

static bool state_is_on(const char *st)
{
    if (state_is_unknown(st)) {
        return false;
    }
    if (strcasecmp(st, "ON") == 0 || strcasecmp(st, "OPEN") == 0) {
        return true;
    }
    if (strcasecmp(st, "OFF") == 0 || strcasecmp(st, "CLOSE") == 0) {
        return false;
    }
    /* 自锁 "1" / PWM duty "1"…"100"；勿把裸 GPIO 高低与逻辑开关联想 */
    char *end = nullptr;
    long v = strtol(st, &end, 10);
    if (end != st && *end == '\0' && v > 0) {
        return true;
    }
    return false;
}

static int bind_channel_index(const nl_widget_t *w)
{
    if (!w) {
        return -1;
    }
    if (w->bind.kind == NL_BIND_DEVICE_CHANNEL) {
        return w->bind.channel;
    }
    if (w->bind.kind == NL_BIND_DEVICE_PIN) {
        return w->bind.pin >= 0 ? w->bind.pin : w->bind.channel;
    }
    return -1;
}

static void set_channel_state(nl_dev_cache_t *d, int ch, const char *st)
{
    if (!d || ch < 0 || !st) {
        return;
    }
    for (uint8_t i = 0; i < d->ch_n; i++) {
        if (d->ch_idx[i] == ch) {
            std::snprintf(d->ch_st[i], sizeof(d->ch_st[i]), "%s", st);
            return;
        }
    }
    if (d->ch_n < NL_CH_PER_DEV) {
        d->ch_idx[d->ch_n] = (int16_t)ch;
        std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", st);
        d->ch_n++;
    }
}

/** 读绑定通道/整机当前缓存态字符串；无则 nullptr。 */
static const char *widget_cache_state(const nl_widget_t *w)
{
    if (!w || !w->bind.device_id[0]) {
        return nullptr;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(w->bind.device_id);
    if (!d) {
        return nullptr;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == ch) {
                return d->ch_st[i];
            }
        }
        return nullptr;
    }
    return d->power;
}

/** 通道/引脚只看对应通道态；勿回退整机 power（易把 UNKNOWN/其它路当成开）。 */
static bool widget_is_on(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w) {
        return false;
    }
    if (kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return true;
    }
    const char *st = widget_cache_state(w);
    if (state_is_unknown(st)) {
        return false;
    }
    return state_is_on(st);
}

static bool widget_state_unknown(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return false;
    }
    if (!w->bind.device_id[0]) {
        return true;
    }
    if (!nl_ov_find_dev(w->bind.device_id)) {
        return true;
    }
    return state_is_unknown(widget_cache_state(w));
}

static int widget_pwm_duty(const nl_widget_t *w)
{
    if (!w) {
        return 0;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(w->bind.device_id);
    if (!d) {
        return 0;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == ch) {
                return (int)strtol(d->ch_st[i], nullptr, 10);
            }
        }
        return 0;
    }
    return (int)strtol(d->power, nullptr, 10);
}

static void set_widget_cache_state(const nl_widget_t *w, const char *st)
{
    if (!w || !st || w->bind.device_id[0] == '\0') {
        return;
    }
    nl_dev_cache_t *d = ensure_dev(w->bind.device_id);
    if (!d) {
        return;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        set_channel_state(d, ch, st);
        return;
    }
    std::snprintf(d->power, sizeof(d->power), "%s", st);
}


static int bind_channel_index_of(const nl_bind_t *bind)
{
    if (!bind) {
        return -1;
    }
    if (bind->kind == NL_BIND_DEVICE_CHANNEL) {
        return bind->channel;
    }
    if (bind->kind == NL_BIND_DEVICE_PIN) {
        return bind->pin >= 0 ? bind->pin : bind->channel;
    }
    return -1;
}

static bool bind_matches_momentary(const nl_bind_t *bind)
{
    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
    if (!layout || !bind || bind->device_id[0] == '\0') {
        return false;
    }
    const int ch = bind_channel_index_of(bind);
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (strcmp(w->skin, "momentary") != 0) {
                    continue;
                }
                if (strcmp(w->bind.device_id, bind->device_id) != 0) {
                    continue;
                }
                if (bind_channel_index(w) == ch) {
                    return true;
                }
            }
        }
    }
    return false;
}

void nl_ov_set_countdown(const char *device_id, int channel_index, int seconds)
{
    if (!device_id || !device_id[0]) {
        return;
    }
    nl_dev_cache_t *d = ensure_dev(device_id);
    if (!d) {
        return;
    }
    if (seconds < 0) {
        seconds = 0;
    }
    if (seconds > 32767) {
        seconds = 32767;
    }
    if (channel_index < 0) {
        d->countdown = (int16_t)seconds;
    } else {
        bool found = false;
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == channel_index) {
                d->ch_cd[i] = (int16_t)seconds;
                found = true;
                break;
            }
        }
        if (!found && d->ch_n < NL_CH_PER_DEV) {
            d->ch_idx[d->ch_n] = (int16_t)channel_index;
            d->ch_st[d->ch_n][0] = '\0';
            d->ch_cd[d->ch_n] = (int16_t)seconds;
            d->ch_n++;
        }
    }
    cd_timer_ensure(any_countdown_active());
}

int nl_ov_widget_countdown(const nl_widget_t *w)
{
    if (!w || !w->bind.device_id[0] || w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return 0;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(w->bind.device_id);
    if (!d) {
        return 0;
    }
    int ch = bind_channel_index(w);
    if (ch < 0) {
        return d->countdown > 0 ? d->countdown : 0;
    }
    for (uint8_t i = 0; i < d->ch_n; i++) {
        if (d->ch_idx[i] == ch) {
            return d->ch_cd[i] > 0 ? d->ch_cd[i] : 0;
        }
    }
    return 0;
}

void nl_ov_apply_bind_state(const nl_bind_t *bind, const char *value)
{
    if (!bind || !value || !value[0] || bind->device_id[0] == '\0') {
        return;
    }
    if (bind->kind != NL_BIND_DEVICE_CHANNEL && bind->kind != NL_BIND_DEVICE_PIN) {
        return;
    }
    if (strcasecmp(value, "CANCEL_COUNTDOWN") == 0) {
        nl_ov_set_countdown(bind->device_id, bind_channel_index_of(bind), 0);
        return;
    }
    if (strncasecmp(value, "OFF_IN", 6) == 0) {
        const char *p = value + 6;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        int sec = atoi(p);
        if (sec > 0) {
            nl_ov_set_countdown(bind->device_id, bind_channel_index_of(bind), sec);
        }
        return;
    }
    if (strcasecmp(value, "SET_PWM") == 0) {
        return;
    }
    /* 开关会取消倒计时（固件 OFF 亦清） */
    if (strcasecmp(value, "ON") == 0 || strcasecmp(value, "OFF") == 0 ||
        strcasecmp(value, "TOGGLE") == 0 || strcasecmp(value, "OPEN") == 0 ||
        strcasecmp(value, "CLOSE") == 0) {
        nl_ov_set_countdown(bind->device_id, bind_channel_index_of(bind), 0);
    }
    /* 点动只发脉冲，禁止把本地态翻成开 */
    if (bind_matches_momentary(bind)) {
        return;
    }

    char st[12];
    if (strcasecmp(value, "TOGGLE") == 0) {
        nl_dev_cache_t *cur = nl_ov_find_dev(bind->device_id);
        bool on = false;
        int ch = bind_channel_index_of(bind);
        if (cur) {
            if (ch >= 0) {
                for (uint8_t i = 0; i < cur->ch_n; i++) {
                    if (cur->ch_idx[i] == ch) {
                        on = state_is_on(cur->ch_st[i]);
                        break;
                    }
                }
            } else {
                on = state_is_on(cur->power);
            }
        }
        std::snprintf(st, sizeof(st), "%s", on ? "OFF" : "ON");
    } else if (strcasecmp(value, "ON") == 0 || strcasecmp(value, "OPEN") == 0) {
        std::snprintf(st, sizeof(st), "ON");
    } else if (strcasecmp(value, "OFF") == 0 || strcasecmp(value, "CLOSE") == 0) {
        std::snprintf(st, sizeof(st), "OFF");
    } else {
        char *end = nullptr;
        (void)strtol(value, &end, 10);
        if (end == value || (end && *end != '\0')) {
            return; /* 非开关/PWM 数值，忽略 */
        }
        std::snprintf(st, sizeof(st), "%s", value);
    }

    nl_dev_cache_t *d = ensure_dev(bind->device_id);
    if (!d) {
        return;
    }
    int ch = bind_channel_index_of(bind);
    if (ch >= 0) {
        set_channel_state(d, ch, st);
    } else {
        std::snprintf(d->power, sizeof(d->power), "%s", st);
    }
}

/** snapshot/批量刷态时禁止色过渡（否则开机十几颗磁贴同时 ease 像闪屏） */
static bool s_tile_bulk_refresh;

static void ensure_tile_trans(void)
{
    if (s_tile_trans_inited) {
        return;
    }
    s_tile_trans_inited = true;
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_TEXT_COLOR, LV_STYLE_BG_OPA,
        LV_STYLE_OPA, LV_STYLE_PROP_INV};
    lv_style_transition_dsc_init(&s_tile_trans, props, lv_anim_path_ease_out, UI_TILE_TRANS_MS, 0,
                                 nullptr);
}

static void tile_apply_trans(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    if (s_tile_bulk_refresh) {
        /* 禁止写成 transition=NULL：按下时 LVGL update_obj_state 会解引用崩溃。 */
        lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSITION, 0);
        return;
    }
    ensure_tile_trans();
    lv_obj_set_style_transition(obj, &s_tile_trans, 0);
}

static void tile_restore_trans_tree(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    tile_apply_trans(obj);
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        tile_restore_trans_tree(lv_obj_get_child(obj, i));
    }
}

#define NL_TILE_VIS_MAX 32
typedef struct {
    char id[NL_LAYOUT_STR];
    bool on;
    bool valid;
} tile_vis_mem_t;
static tile_vis_mem_t s_tile_vis[NL_TILE_VIS_MAX];

/** 记录磁贴开态；返回是否 OFF→ON 上升沿（用于图标弹跳）。 */
static bool tile_vis_on_rising(const char *widget_id, bool now_on)
{
    if (!widget_id || !widget_id[0]) {
        return false;
    }
    int free_i = -1;
    for (int i = 0; i < NL_TILE_VIS_MAX; i++) {
        if (s_tile_vis[i].valid && strcmp(s_tile_vis[i].id, widget_id) == 0) {
            const bool rising = !s_tile_vis[i].on && now_on;
            s_tile_vis[i].on = now_on;
            return rising;
        }
        if (!s_tile_vis[i].valid && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = 0;
    }
    s_tile_vis[free_i].valid = true;
    std::snprintf(s_tile_vis[free_i].id, sizeof(s_tile_vis[free_i].id), "%s", widget_id);
    s_tile_vis[free_i].on = now_on;
    return false;
}

static void anim_obj_scale(void *var, int32_t v)
{
    lv_obj_t *obj = static_cast<lv_obj_t *>(var);
    if (!obj) {
        return;
    }
    lv_obj_set_style_transform_scale_x(obj, v, 0);
    lv_obj_set_style_transform_scale_y(obj, v, 0);
}

/** 开态：图标胶囊短弹一下（LV_SCALE_NONE=256）。 */
static void tile_icon_pop(lv_obj_t *cap)
{
    if (!cap) {
        return;
    }
    const int32_t w = lv_obj_get_width(cap);
    const int32_t h = lv_obj_get_height(cap);
    lv_obj_set_style_transform_pivot_x(cap, w > 0 ? w / 2 : 16, 0);
    lv_obj_set_style_transform_pivot_y(cap, h > 0 ? h / 2 : 16, 0);
    lv_anim_delete(cap, anim_obj_scale);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cap);
    lv_anim_set_values(&a, 210, LV_SCALE_NONE);
    lv_anim_set_time(&a, UI_TILE_ICON_POP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, anim_obj_scale);
    lv_anim_start(&a);
}

static uint32_t color_ambient_bg(uint32_t c)
{
    uint32_t r = ((c >> 16) & 0xff) * 26 / 100;
    uint32_t g = ((c >> 8) & 0xff) * 26 / 100;
    uint32_t b = (c & 0xff) * 26 / 100;
    r = r < 12 ? 12 : r;
    g = g < 15 ? 15 : g;
    b = b < 20 ? 20 : b;
    return (r << 16) | (g << 8) | b;
}

static uint32_t widget_fill_for_state(const nl_widget_t *w, tile_kind_t kind, bool online, bool on,
                                      bool unknown)
{
    if (!online || unknown) {
        return UI_C_OFFLINE;
    }
    if (!on) {
        return kind_fill_off(kind);
    }
    /* 开态：优先 layout 自定义底色（环境微光深色），否则语义亮色微光 */
    uint32_t custom = 0;
    if (w && parse_bg_hex(w->bg_color, &custom)) {
        return color_ambient_bg(custom);
    }
    return kind_fill_on(kind);
}

static void paint_dual_seg(lv_obj_t *seg, bool online, bool on, bool is_on_branch, lv_color_t active_c)
{
    if (!seg) {
        return;
    }
    tile_apply_trans(seg);
    const bool active = online && (on == is_on_branch);
    lv_obj_set_style_bg_color(seg, active ? active_c : UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(seg, online ? (active ? LV_OPA_COVER : LV_OPA_60) : LV_OPA_40, 0);
    lv_obj_set_style_border_width(seg, active ? 0 : 1, 0);
    lv_obj_set_style_border_color(seg, UI_BORDER, 0);
    lv_obj_set_style_border_opa(seg, active ? LV_OPA_TRANSP : LV_OPA_40, 0);
    ui_set_enabled(seg, online);
    if (lv_obj_get_child_count(seg) > 0) {
        lv_obj_t *lb = lv_obj_get_child(seg, 0);
        tile_apply_trans(lb);
        lv_obj_set_style_text_color(lb, active ? UI_ON_TILE : (online ? UI_TEXT_DIM : UI_DISABLED_FG), 0);
    }
}

static void style_tile_visual(lv_obj_t *tile, const nl_widget_t *w)
{
    if (!tile || !w) {
        return;
    }
    if (w->type == NL_WIDGET_SKILL_TILE || w->type == NL_WIDGET_SKILL_WIDGET ||
        w->type == NL_WIDGET_PHOTO_FRAME) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    bool online = true;
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0]) {
        online = nodelio_panel_ui_device_online(w->bind.device_id);
    }
    const bool unknown = widget_state_unknown(w, kind);
    /* 点按已乐观写缓存；失败/超时回滚。不转圈等真态 */
    bool on = !unknown && widget_is_on(w, kind);
    const bool on_rising = tile_vis_on_rising(w->id, on && online);
    uint32_t fill = widget_fill_for_state(w, kind, online, on, unknown);
    tile_apply_trans(tile);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    /* 首页磁贴无外边框，保持沉浸纯净 */
    lv_obj_set_style_border_width(tile, 0, 0);
    /* 双段卡整卡不可点，只由支路响应；勿用 ui_set_enabled 把 CLICKABLE 加回去 */
    if (dual_line_use_segments(w)) {
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        if (online) {
            lv_obj_remove_state(tile, LV_STATE_DISABLED);
            lv_obj_set_style_opa(tile, LV_OPA_COVER, 0);
        } else {
            lv_obj_add_state(tile, LV_STATE_DISABLED);
            lv_obj_set_style_opa(tile, LV_OPA_60, 0);
        }
    } else {
        ui_set_enabled(tile, online || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO);
        if (online) {
            lv_obj_set_style_opa(tile, LV_OPA_COVER, 0);
        }
    }

    /* 开态图标使用高饱和品牌/语义亮色，关态暗白，离线灰暗 */
    lv_color_t active_c = UI_ACCENT;
    uint32_t custom_bg = 0;
    if (w && parse_bg_hex(w->bg_color, &custom_bg)) {
        active_c = lv_color_hex(custom_bg);
    } else if (kind == TILE_PWM) {
        active_c = UI_TILE_PWM_BORDER;
    } else if (kind == TILE_GPIO) {
        active_c = UI_TILE_GPIO_BORDER;
    } else if (kind == TILE_SCENE) {
        active_c = UI_CAPSULE_SCENE;
    }

    const bool dim = !online || unknown;
    lv_color_t icon_c = dim ? UI_DISABLED_FG : (on ? active_c : UI_TEXT_DIM);
    lv_color_t title_c = dim ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT);
    lv_color_t hint_c = dim ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT_MUTED);
    lv_color_t badge_bg =
        dim ? UI_TILE_BADGE_OFFLINE : (on ? UI_TILE_BADGE_ON : UI_TILE_BADGE_OFF);
    if (!dim && on) {
        badge_bg = active_c;
    }

    bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
    bool is_row = (w && w->gw >= 2 && w->gh <= 1);
    bool dl_segs = dual_line_use_segments(w);
    const char *dl_off = nullptr;
    const char *dl_on = nullptr;
    if (widget_is_dual_line(w)) {
        dual_branch_names(w, &dl_off, &dl_on);
    }

    uint32_t sub = lv_obj_get_child_count(tile);
    for (uint32_t j = 0; j < sub; j++) {
        lv_obj_t *ch = lv_obj_get_child(tile, j);
        intptr_t role = (intptr_t)lv_obj_get_user_data(ch);
        if (role == TILE_CHILD_DL_ROW || role == TILE_CHILD_DL_OFF || role == TILE_CHILD_DL_ON) {
            if (role == TILE_CHILD_DL_ROW) {
                uint32_t nseg = lv_obj_get_child_count(ch);
                for (uint32_t s = 0; s < nseg; s++) {
                    lv_obj_t *seg = lv_obj_get_child(ch, s);
                    intptr_t sr = (intptr_t)lv_obj_get_user_data(seg);
                    if (sr == TILE_CHILD_DL_OFF) {
                        paint_dual_seg(seg, online, on, false, active_c);
                    } else if (sr == TILE_CHILD_DL_ON) {
                        paint_dual_seg(seg, online, on, true, active_c);
                    }
                }
            } else if (role == TILE_CHILD_DL_OFF) {
                paint_dual_seg(ch, online, on, false, active_c);
            } else {
                paint_dual_seg(ch, online, on, true, active_c);
            }
            continue;
        }
        if (role == TILE_CHILD_CAPSULE) {
            /* 彻底去除图标底座框框，保持 100% 透明无框 */
            lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(ch, 0, 0);
            if (lv_obj_get_child_count(ch) > 0) {
                lv_obj_t *icon_lbl = lv_obj_get_child(ch, 0);
                tile_apply_trans(icon_lbl);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(icon_lbl, icon_c, 0);
                lv_obj_set_style_text_opa(icon_lbl, (on && online) ? LV_OPA_COVER : LV_OPA_80, 0);
                lv_obj_center(icon_lbl);
            }
            if (on_rising && !s_tile_bulk_refresh) {
                tile_icon_pop(ch);
            }
        } else if (lv_obj_check_type(ch, &lv_label_class)) {
            tile_apply_trans(ch);
            if (role == TILE_CHILD_ICON) {
                lv_label_set_text(ch, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(ch, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(ch, icon_c, 0);
                if (on_rising && !s_tile_bulk_refresh) {
                    tile_icon_pop(ch);
                }
            } else if (role == TILE_CHILD_HINT) {
                const int cd = nl_ov_widget_countdown(w);
                if (is_1x1) {
                    /* 1x1：倒计时优先生效；否则仅离线显示「离」 */
                    if (cd > 0) {
                        char hb[12];
                        format_countdown(cd, hb, sizeof(hb));
                        lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                        lv_label_set_text(ch, hb);
                        nl_ov_set_font(ch);
                        style_hint_pill(ch, true);
                        lv_obj_set_style_text_color(ch, UI_WARN, 0);
                        lv_obj_set_style_bg_color(ch, UI_SURFACE_PRESSED, 0);
                        lv_obj_set_style_bg_opa(ch, LV_OPA_70, 0);
                        lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, -2, 2);
                    } else if (!online) {
                        lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                        lv_label_set_text(ch, "离");
                        nl_ov_set_font(ch);
                        style_hint_pill(ch, false);
                        lv_obj_set_style_text_color(ch, hint_c, 0);
                        lv_obj_set_style_bg_color(ch, badge_bg, 0);
                        lv_obj_set_style_bg_opa(ch, LV_OPA_60, 0);
                        lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, -2, 2);
                    } else {
                        lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
                    }
                    continue;
                }
                /* 双段按钮已表达状态：在线且无倒计时则隐藏角标 */
                if (dl_segs && cd <= 0 && online) {
                    lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                if (cd > 0) {
                    char hb[12];
                    format_countdown(cd, hb, sizeof(hb));
                    lv_label_set_text(ch, hb);
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, true);
                    lv_obj_set_style_text_color(ch, UI_WARN, 0);
                    lv_obj_set_style_bg_color(ch, UI_SURFACE_PRESSED, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_70, 0);
                } else if (kind == TILE_PWM) {
                    char hb[20];
                    const char *prefix = "";
                    if (w && w->skin[0]) {
                        if (strcmp(w->skin, "fan") == 0) {
                            prefix = "风 ";
                        } else if (strcmp(w->skin, "dimmer") == 0) {
                            prefix = "光 ";
                        } else if (strcmp(w->skin, "heater") == 0) {
                            prefix = "热 ";
                        } else if (strcmp(w->skin, "tank") == 0) {
                            prefix = "位 ";
                        } else if (strcmp(w->skin, "gauge") == 0) {
                            prefix = "表 ";
                        }
                    }
                    std::snprintf(hb, sizeof(hb), "%s%d%%", prefix, widget_pwm_duty(w));
                    lv_label_set_text(ch, hb);
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                } else if (kind == TILE_SCENE) {
                    lv_label_set_text(ch, "情景");
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, UI_TEXT_MUTED, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
                } else if (w && strcmp(w->skin, "momentary") == 0) {
                    /* 点动角标 */
                    lv_label_set_text(ch, (!online) ? "离" : "点动");
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, online ? LV_OPA_60 : LV_OPA_50, 0);
                } else if (widget_is_dual_line(w)) {
                    /* 窄卡 DualLine：角标显示当前导通支路名（开→NO，关→NC） */
                    const char *act = on ? dl_on : dl_off;
                    lv_label_set_text(ch, (!online) ? "离" : (act ? act : (on ? "NO" : "NC")));
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, on && online ? LV_OPA_70 : LV_OPA_50, 0);
                } else {
                    /* 开/关/离：右上角/右侧小角标，不占主列 */
                    lv_label_set_text(ch, (!online) ? "离" : (on ? "开" : "关"));
                    nl_ov_set_font(ch);
                    style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, on && online ? LV_OPA_70 : LV_OPA_50, 0);
                }
                /* 右上角药丸统一定位：横条靠右中，其他规格靠右上角预留 4px 呼吸边距 */
                lv_obj_align(ch, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, is_row ? -2 : -4, is_row ? 0 : 4);
            } else if (role == TILE_CHILD_TITLE) {
                if (is_1x1) {
                    lv_obj_add_flag(ch, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                    nl_ov_set_font(ch);
                    lv_obj_set_style_text_color(ch, title_c, 0);
                }
            }
        } else if (lv_obj_check_type(ch, &lv_slider_class)) {
            ui_set_enabled(ch, online);
            int duty = widget_pwm_duty(w);
            int mn = w->bind.min;
            int mx = w->bind.max > mn ? w->bind.max : 100;
            if (duty < mn) {
                duty = mn;
            }
            if (duty > mx) {
                duty = mx;
            }
            lv_slider_set_value(ch, duty, LV_ANIM_ON);
        }
    }
}

void nl_ov_refresh_home_tiles(void)
{
    if (!s_home) {
        return;
    }
    /* 批量刷态关掉色过渡，否则 bootstrap 一次改十几颗磁贴像整屏闪 */
    s_tile_bulk_refresh = true;
    uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(child));
        if (!w) {
            continue;
        }
        style_tile_visual(child, w);
    }
    s_tile_bulk_refresh = false;
    /* 刷态时拆掉过渡；刷完立刻挂回，否则下次按下走 NULL transition。 */
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(s_home, i);
        if (!lv_obj_get_user_data(child)) {
            continue;
        }
        tile_restore_trans_tree(child);
    }
}

static void place_grid_obj(lv_obj_t *obj, const nl_widget_t *w, int cell_w, int cell_h)
{
    int gw = w->gw > 0 ? w->gw : 1;
    int gh = w->gh > 0 ? w->gh : 1;
    int col = w->col >= 0 ? w->col : 0;
    int row = w->row >= 0 ? w->row : 0;
    int x = s_grid_ox + col * (cell_w + GRID_GAP);
    int y = s_grid_oy + row * (cell_h + GRID_GAP);
    int tw = gw * cell_w + (gw - 1) * GRID_GAP;
    int th = gh * cell_h + (gh - 1) * GRID_GAP;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, tw, th);
}

struct shot_job {
    std::string *jpeg;
    nodelio_nvs_config_t cfg;
};

static void capture_and_upload_screenshot(void)
{
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
        nl_ov_toast("网络未就绪");
        return;
    }
    Display *disp = Board::GetInstance().GetDisplay();
    if (!disp) {
        nl_ov_toast("未检测到显示屏");
        return;
    }
    if (s_cfg.server_url[0] == '\0' || s_cfg.device_id[0] == '\0') {
        nl_ov_toast("未配置服务器");
        return;
    }
    const size_t dram_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    /* 技能卡打开时常见 free≈17KB、largest≈7KB（碎片）。心跳仍能短 TCP，
     * 勿用 16KB 一刀切；仅在几乎无连续块时放弃。JPEG/任务走 PSRAM。 */
    if (dram_before < 3 * 1024) {
        ESP_LOGW(TAG, "screenshot skip: largest_dram=%u free=%u", (unsigned)dram_before,
                 (unsigned)dram_free);
        nl_ov_toast("内存过碎，先关掉技能卡再截");
        return;
    }
    /* JPEG 在 LVGL 线程生成；HTTP 必须扔后台，否则堵 UI / 撑爆 LVGL 栈 */
    auto *jpeg = new (std::nothrow) std::string();
    if (!jpeg) {
        nl_ov_toast("内存不足");
        return;
    }
    /* 碎片严重时压质量，减小编码峰值与上传体积 */
    const int quality = (dram_before < 20 * 1024) ? 40 : ((dram_before < 40 * 1024) ? 55 : 70);
    lv_obj_t *snap = lv_layer_top();
    if (!disp->SnapshotToJpeg(*jpeg, quality, snap) || jpeg->empty()) {
        ESP_LOGW(TAG, "SnapshotToJpeg failed q=%d dram=%u", quality, (unsigned)dram_before);
        delete jpeg;
        nl_ov_toast("截屏生成失败");
        return;
    }
    ESP_LOGI(TAG, "screenshot jpeg=%u q=%d dram_free_largest=%u", (unsigned)jpeg->size(), quality,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    auto *job = static_cast<shot_job *>(heap_caps_malloc(sizeof(shot_job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job) {
        delete jpeg;
        nl_ov_toast("无法上传");
        return;
    }
    job->jpeg = jpeg;
    job->cfg = s_cfg;
    nl_ov_toast("正在上传截图…");
    constexpr uint32_t kStackWords = 12288 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *job = static_cast<shot_job *>(p);
                /* 把 JPEG 拷到 PSRAM，释放 string 可能占的内部堆，给 lwIP TCP 缓冲腾地方 */
                const size_t n = job->jpeg->size();
                uint8_t *ps =
                    static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (ps) {
                    memcpy(ps, job->jpeg->data(), n);
                    delete job->jpeg;
                    job->jpeg = nullptr;
                }
                const void *body = ps ? static_cast<const void *>(ps) : job->jpeg->data();
                const size_t body_n = ps ? n : job->jpeg->size();

                char base[160];
                nl_url_strip_base(job->cfg.server_url, base, sizeof(base));
                char url[256];
                std::snprintf(url, sizeof(url), "%s/api/devices/%s/panel/screenshot", base, job->cfg.device_id);
                int http_status = 0;
                esp_err_t err = nodelio_http_post_image(url, job->cfg.device_key, body, body_n, &http_status);
                if (err == ESP_OK && http_status >= 200 && http_status < 300) {
                    ESP_LOGI(TAG, "Screenshot uploaded successfully: %u bytes", (unsigned)body_n);
                    /* P3c：多屏时提示当前屏序号，便于各拍一帧 */
                    const int pi = nl_ov_home_user_page_index();
                    const int pn = nl_ov_home_user_page_count();
                    if (pi >= 0 && pn > 1) {
                        char msg[40];
                        std::snprintf(msg, sizeof(msg), "已上传 · 屏%d/%d", pi + 1, pn);
                        nodelio_panel_ui_toast_defer(msg);
                    } else {
                        nodelio_panel_ui_toast_defer("已上传实机截图");
                    }
                } else {
                    ESP_LOGW(TAG, "Screenshot upload failed err=%s http=%d jpeg=%u dram=%u",
                             esp_err_to_name(err), http_status, (unsigned)body_n,
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE) {
                        nodelio_panel_ui_toast_defer("内存/网络忙，稍后再截");
                    } else {
                        nodelio_panel_ui_toast_defer("截图上传失败");
                    }
                }
                if (ps) {
                    heap_caps_free(ps);
                }
                if (job->jpeg) {
                    delete job->jpeg;
                }
                heap_caps_free(job);
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_shot", kStackWords, job, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        if (job->jpeg) {
            delete job->jpeg;
        }
        heap_caps_free(job);
        nl_ov_toast("无法上传");
    }
}

void nl_ov_on_brand_click(lv_event_t *e)
{
    (void)e;
    static uint32_t last_click_ms = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_click_ms < 1200) {
        return;
    }
    last_click_ms = now;
    capture_and_upload_screenshot();
}

/* —— 首页开关：点按乐观写缓存；HTTP 成败定生死；失败/超时回滚。不转圈等 snapshot —— */
#define NL_CMD_PENDING_MAX 8
#define NL_CMD_ROLLBACK_MS 8000

struct cmd_pending_t {
    bool active;
    char widget_id[NL_LAYOUT_STR];
    char device_id[NL_LAYOUT_STR];
    int16_t channel;
    char from[12]; /* 点击前缓存态，失败回滚用 */
    int64_t deadline_us;
};

static cmd_pending_t s_cmd_pending[NL_CMD_PENDING_MAX];
static lv_timer_t *s_pending_timer;

static bool widget_is_cmd_pending(const nl_widget_t *w)
{
    if (!w || !w->id[0]) {
        return false;
    }
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        if (s_cmd_pending[i].active && strcmp(s_cmd_pending[i].widget_id, w->id) == 0) {
            return true;
        }
    }
    return false;
}

static void cmd_pending_timer_ensure(bool want);

static void cmd_pending_clear_slot(cmd_pending_t *p)
{
    if (!p) {
        return;
    }
    p->active = false;
    p->widget_id[0] = '\0';
    p->device_id[0] = '\0';
    p->from[0] = '\0';
    p->channel = -1;
    p->deadline_us = 0;
}

static bool cmd_pending_any(void)
{
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        if (s_cmd_pending[i].active) {
            return true;
        }
    }
    return false;
}

void nl_ov_cmd_pending_clear_all(void)
{
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        cmd_pending_clear_slot(&s_cmd_pending[i]);
    }
    cmd_pending_timer_ensure(false);
}

/** 兼容旧调用点；乐观路径不再等 snapshot 对齐。 */
void nl_ov_cmd_pending_scan_match(void) {}

static void cmd_pending_clear_widget(const char *widget_id)
{
    if (!widget_id || !widget_id[0]) {
        return;
    }
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        if (s_cmd_pending[i].active && strcmp(s_cmd_pending[i].widget_id, widget_id) == 0) {
            cmd_pending_clear_slot(&s_cmd_pending[i]);
        }
    }
    cmd_pending_timer_ensure(cmd_pending_any());
}

/** 失败/超时：按 from 写回缓存并清 pending。 */
static void cmd_pending_rollback_widget(const char *widget_id)
{
    if (!widget_id || !widget_id[0]) {
        return;
    }
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        cmd_pending_t *p = &s_cmd_pending[i];
        if (!p->active || strcmp(p->widget_id, widget_id) != 0) {
            continue;
        }
        if (p->device_id[0] && p->from[0]) {
            nl_dev_cache_t *d = ensure_dev(p->device_id);
            if (d) {
                if (p->channel >= 0) {
                    set_channel_state(d, p->channel, p->from);
                } else {
                    std::snprintf(d->power, sizeof(d->power), "%s", p->from);
                }
            }
        }
        cmd_pending_clear_slot(p);
    }
    cmd_pending_timer_ensure(cmd_pending_any());
}

static void cmd_pending_begin(const nl_widget_t *w)
{
    if (!w || !w->id[0]) {
        return;
    }
    cmd_pending_clear_widget(w->id);
    int slot = -1;
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        if (!s_cmd_pending[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int64_t oldest = INT64_MAX;
        for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
            if (s_cmd_pending[i].deadline_us < oldest) {
                oldest = s_cmd_pending[i].deadline_us;
                slot = i;
            }
        }
        if (slot < 0) {
            slot = 0;
        }
        cmd_pending_rollback_widget(s_cmd_pending[slot].widget_id);
    }
    cmd_pending_t *p = &s_cmd_pending[slot];
    p->active = true;
    std::snprintf(p->widget_id, sizeof(p->widget_id), "%s", w->id);
    std::snprintf(p->device_id, sizeof(p->device_id), "%s", w->bind.device_id);
    p->channel = (int16_t)bind_channel_index(w);
    const char *cur = widget_cache_state(w);
    if (cur && cur[0]) {
        std::snprintf(p->from, sizeof(p->from), "%s", cur);
    } else {
        std::snprintf(p->from, sizeof(p->from), "%s", "UNKNOWN");
    }
    p->deadline_us = esp_timer_get_time() + (int64_t)NL_CMD_ROLLBACK_MS * 1000;
    cmd_pending_timer_ensure(true);
}

static void cmd_pending_tick(void)
{
    const int64_t now = esp_timer_get_time();
    bool rolled = false;
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        cmd_pending_t *p = &s_cmd_pending[i];
        if (!p->active) {
            continue;
        }
        if (now < p->deadline_us) {
            continue;
        }
        char wid[NL_LAYOUT_STR];
        std::snprintf(wid, sizeof(wid), "%s", p->widget_id);
        cmd_pending_rollback_widget(wid);
        rolled = true;
    }
    if (rolled) {
        nl_ov_toast("操作超时");
        nl_ov_refresh_home_tiles();
    }
    cmd_pending_timer_ensure(cmd_pending_any());
}

static void cmd_pending_timer_ensure(bool want)
{
    if (want) {
        if (!s_pending_timer) {
            s_pending_timer = lv_timer_create(
                [](lv_timer_t *) {
                    if (!s_screen_on) {
                        return;
                    }
                    cmd_pending_tick();
                },
                400, nullptr);
        } else {
            lv_timer_resume(s_pending_timer);
        }
    } else if (s_pending_timer) {
        lv_timer_pause(s_pending_timer);
    }
}

struct async_cmd_item_t {
    char widget_id[NL_LAYOUT_STR];
    nl_bind_t bind;
    char value[16];
    bool rollback_on_fail; /* 首页开关：点按已乐观写态，失败回滚 */
};

static QueueHandle_t s_cmd_queue = nullptr;
static TaskHandle_t s_cmd_task = nullptr;

static void async_cmd_worker(void *arg)
{
    (void)arg;
    async_cmd_item_t item;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int http = 0;
        /* 点按已写缓存；HTTP 成功不再二次乐观（DIOL 成功仍会落真态） */
        esp_err_t err = nodelio_panel_command_ex(&s_cfg, item.widget_id, "set", &item.bind, item.value, &http,
                                                 !item.rollback_on_fail);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "async cmd %s http=%d err=%s", item.widget_id, http, esp_err_to_name(err));
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                const char *msg = "操作未响应";
                if (http == 503) {
                    msg = "设备未在线";
                } else if (http == 401) {
                    msg = "需口令";
                }
                nl_ov_toast(msg);
                if (item.rollback_on_fail) {
                    cmd_pending_rollback_widget(item.widget_id);
                    nl_ov_refresh_home_tiles();
                }
            }
        } else {
            s_cloud_ok = true;
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                if (s_cloud_lbl) {
                    nl_ov_refresh_status_icons(s_wifi_ok, true, nullptr);
                }
                if (item.rollback_on_fail) {
                    cmd_pending_clear_widget(item.widget_id);
                }
            }
        }
    }
}

#define ASYNC_CMD_STACK_BYTES 10240

static StackType_t *s_async_cmd_stack = nullptr;
static StaticTask_t s_async_cmd_tcb;

static void ensure_cmd_worker(void)
{
    if (s_cmd_queue) {
        return;
    }
    s_cmd_queue = xQueueCreate(8, sizeof(async_cmd_item_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Fail to create async cmd queue");
        return;
    }
    if (!s_async_cmd_stack) {
        s_async_cmd_stack = (StackType_t *)heap_caps_malloc(ASYNC_CMD_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_async_cmd_stack) {
            s_async_cmd_stack = (StackType_t *)malloc(ASYNC_CMD_STACK_BYTES);
        }
    }
    if (s_async_cmd_stack) {
        s_cmd_task = xTaskCreateStatic(async_cmd_worker, "nl_cmd_async",
                                       ASYNC_CMD_STACK_BYTES / sizeof(StackType_t), nullptr, 3,
                                       s_async_cmd_stack, &s_async_cmd_tcb);
    } else {
        xTaskCreate(async_cmd_worker, "nl_cmd_async",
                    ASYNC_CMD_STACK_BYTES / sizeof(StackType_t), nullptr, 3, &s_cmd_task);
    }
}

static void dispatch_async_cmd(const nl_widget_t *w, const char *value, bool rollback_on_fail)
{
    ensure_cmd_worker();
    if (!s_cmd_queue || !w) {
        return;
    }
    async_cmd_item_t item;
    std::memset(&item, 0, sizeof(item));
    std::snprintf(item.widget_id, sizeof(item.widget_id), "%s", w->id);
    item.bind = w->bind;
    std::snprintf(item.value, sizeof(item.value), "%s", value ? value : "TOGGLE");
    item.rollback_on_fail = rollback_on_fail;
    if (xQueueSend(s_cmd_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, discard oldest");
        async_cmd_item_t dummy;
        xQueueReceive(s_cmd_queue, &dummy, 0);
        if (dummy.rollback_on_fail && dummy.widget_id[0]) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                cmd_pending_rollback_widget(dummy.widget_id);
                nl_ov_refresh_home_tiles();
                nl_ov_toast("操作太频繁");
            }
        }
        xQueueSend(s_cmd_queue, &item, 0);
    }
}

/** 首页开关：记 from → 乐观写缓存 → 下发；失败回滚。 */
static void home_switch_commit(nl_widget_t *w, const char *want)
{
    if (!w || !want || !want[0]) {
        return;
    }
    cmd_pending_begin(w);
    set_widget_cache_state(w, want);
    nl_ov_refresh_home_tiles();
    dispatch_async_cmd(w, want, true);
}

static void widget_cmd(const nl_widget_t *w, const char *value)
{
    if (!w) {
        return;
    }
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
        !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    dispatch_async_cmd(w, value, false);
}

/** DualLine 支路：点 NC→OFF、点 NO→ON（绝对态，非翻转） */
static void on_dual_seg(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *seg = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (seg && lv_obj_has_state(seg, LV_STATE_DISABLED)) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || !seg) {
        return;
    }
    if (widget_is_cmd_pending(w)) {
        return;
    }
    intptr_t role = (intptr_t)lv_obj_get_user_data(seg);
    const bool want_on = (role == TILE_CHILD_DL_ON);
    const char *want = want_on ? "ON" : "OFF";
    tile_kind_t kind = tile_kind_of(w);
    if (widget_is_on(w, kind) == want_on) {
        return; /* 已在目标支路 */
    }
    if (w->bind.device_id[0] && !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    home_switch_commit(w, want);
}

static void on_btn(lv_event_t *e)
{
    lv_obj_t *tgt = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (tgt && lv_obj_has_state(tgt, LV_STATE_DISABLED)) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w) {
        return;
    }
    if (w->type == NL_WIDGET_SCENE_CHIP || w->type == NL_WIDGET_SCENE_GRID ||
        w->bind.kind == NL_BIND_HOME_SCENARIO) {
        widget_cmd(w, w->bind.name);
        /* 轻反馈：边框闪 + 短弹 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
            tile_icon_pop(tgt);
        }
        return;
    }
    if (w && strcmp(w->skin, "momentary") == 0) {
        if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
            !nodelio_panel_ui_device_online(w->bind.device_id)) {
            nl_ov_toast("设备未在线");
            return;
        }
        /* 点动：下发触发脉冲，边框 + 弹跳反馈，不把本地缓存翻成开态 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
            tile_icon_pop(tgt);
        }
        dispatch_async_cmd(w, "TOGGLE", false);
        return;
    }
    if (widget_is_cmd_pending(w)) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    const char *want = widget_is_on(w, kind) ? "OFF" : "ON";
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
        !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    home_switch_commit(w, want);
}

static void on_slider(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (!w || !slider) {
        return;
    }
    int duty = (int)lv_slider_get_value(slider);
    char val[12];
    std::snprintf(val, sizeof(val), "%d", duty);
    set_widget_cache_state(w, val);
    nl_ov_refresh_home_tiles();
    widget_cmd(w, val);
}

static lv_obj_t *make_tile_shell(const nl_widget_t *w, tile_kind_t kind, int cell_w, int cell_h)
{
    uint32_t fill = widget_fill_for_state(w, kind, true, false, false);
    lv_obj_t *tile = lv_obj_create(s_home);
    lv_obj_set_user_data(tile, (void *)w);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, UI_RADIUS_MD, 0);
    lv_obj_set_style_pad_all(tile, UI_PAD_SM, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    place_grid_obj(tile, w, cell_w, cell_h);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tile, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(tile, 1, LV_STATE_PRESSED);
    ensure_tile_trans();
    lv_obj_set_style_transition(tile, &s_tile_trans, 0);
    return tile;
}

static lv_obj_t *make_dual_seg(lv_obj_t *parent, nl_widget_t *w, intptr_t role, const char *text,
                               bool is_row_layout)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_user_data(seg, (void *)role);
    lv_obj_set_flex_grow(seg, 1);
    lv_obj_set_height(seg, is_row_layout ? 30 : 40);
    lv_obj_set_style_radius(seg, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(seg, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_60, 0);
    lv_obj_set_style_pad_left(seg, 4, 0);
    lv_obj_set_style_pad_right(seg, 4, 0);
    lv_obj_set_style_pad_top(seg, 2, 0);
    lv_obj_set_style_pad_bottom(seg, 2, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(seg, 8);
    lv_obj_set_style_bg_color(seg, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(seg, on_dual_seg, LV_EVENT_CLICKED, w);

    char short_n[24];
    short_label(text, short_n, sizeof(short_n), is_row_layout ? 4 : 5);
    lv_obj_t *lb = lv_label_create(seg);
    lv_label_set_text(lb, short_n[0] ? short_n : text);
    nl_ov_set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT_DIM, 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, lv_pct(100));
    lv_obj_center(lb);
    return seg;
}

static void add_tile_labels(lv_obj_t *tile, nl_widget_t *w, tile_kind_t kind, const char *title)
{
    bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
    bool is_row = (w && w->gw >= 2 && w->gh <= 1);
    bool is_large = (w && (w->gw >= 2 && w->gh >= 2));
    bool is_col_slim = (w && w->gw <= 1 && w->gh >= 2);
    bool dl_segs = dual_line_use_segments(w);

    char short_t[48];
    short_label(title, short_t, sizeof(short_t), title_max_runes(w));

    if (is_1x1) {
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(tile, 4, 0);
    } else if (dl_segs && is_row) {
        /* DualLine 宽横条：左标题 + 右双段 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 10, 0);
        lv_obj_set_style_pad_right(tile, 8, 0);
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else if (dl_segs && is_large) {
        /* DualLine 大方块：上标题、下互斥双段（无大电源图标） */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_pad_row(tile, 10, 0);
    } else if (is_row) {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 10, 0);
        lv_obj_set_style_pad_right(tile, 8, 0);
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else if (is_col_slim) {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_set_style_pad_row(tile, 8, 0);
    } else {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_set_style_pad_row(tile, is_large ? 10 : 8, 0);
    }

    /* 1. 图标：双段大方块省略大图标，把空间留给支路名 */
    lv_coord_t cap_size = is_row ? 28 : (is_large ? 46 : (is_col_slim ? 36 : 42));
    if (dl_segs && is_large) {
        /* skip icon */
    } else if (is_1x1) {
        lv_obj_t *icon_lbl = lv_label_create(tile);
        lv_obj_set_user_data(icon_lbl, (void *)(intptr_t)TILE_CHILD_ICON);
        lv_label_set_text(icon_lbl, kind_symbol(w, kind, false));
        lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(false), 0);
        lv_obj_set_style_text_color(icon_lbl, UI_TEXT_DIM, 0);
        lv_obj_align(icon_lbl, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_t *capsule = lv_obj_create(tile);
        lv_obj_remove_style_all(capsule);
        lv_obj_set_user_data(capsule, (void *)(intptr_t)TILE_CHILD_CAPSULE);
        lv_obj_set_size(capsule, cap_size, cap_size);
        lv_obj_set_style_bg_opa(capsule, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(capsule, 0, 0);
        lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        lv_obj_t *icon_lbl = lv_label_create(capsule);
        lv_label_set_text(icon_lbl, kind_symbol(w, kind, false));
        lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
        lv_obj_set_style_text_color(icon_lbl, UI_TEXT_DIM, 0);
        lv_obj_center(icon_lbl);
    }

    /* 2. 标题 */
    lv_obj_t *lb = lv_label_create(tile);
    lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
    lv_label_set_text(lb, short_t[0] ? short_t : title);
    nl_ov_set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    if (is_1x1) {
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
    } else if (dl_segs && is_row) {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(lb, 96);
    } else if (is_row) {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_flex_grow(lb, 1);
        lv_obj_set_style_pad_right(lb, 26, 0);
    } else {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lb, lv_pct(94));
        lv_obj_set_height(lb, LV_SIZE_CONTENT);
    }
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

    /* 2b. DualLine 互斥双段 */
    if (dl_segs) {
        const char *off_n = nullptr;
        const char *on_n = nullptr;
        dual_branch_names(w, &off_n, &on_n);
        lv_obj_t *row = lv_obj_create(tile);
        lv_obj_remove_style_all(row);
        lv_obj_set_user_data(row, (void *)(intptr_t)TILE_CHILD_DL_ROW);
        lv_obj_set_width(row, lv_pct(is_row ? 58 : 100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        if (is_row) {
            lv_obj_set_flex_grow(row, 1);
        }
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        make_dual_seg(row, w, TILE_CHILD_DL_OFF, off_n, is_row);
        make_dual_seg(row, w, TILE_CHILD_DL_ON, on_n, is_row);
    }

    /* 3. 药丸浮动角标 */
    lv_obj_t *hint = lv_label_create(tile);
    lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
    lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
    lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_OFF, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
    style_hint_pill(hint, false);
    lv_obj_align(hint, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, is_row ? -2 : -4, is_row ? 0 : 4);

    if (is_1x1 || dl_segs) {
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }

    if (kind == TILE_SCENE) {
        lv_label_set_text(hint, "情景");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else if (w && strcmp(w->skin, "momentary") == 0) {
        lv_label_set_text(hint, "点动");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else if (widget_is_dual_line(w)) {
        const char *off_n = nullptr;
        dual_branch_names(w, &off_n, nullptr);
        lv_label_set_text(hint, off_n ? off_n : "NC");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else {
        lv_label_set_text(hint, "关");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    }
}

/** B0：可渲染控家磁贴（不含 spacer / header / unknown） */
static bool widget_is_control_tile(const nl_widget_t *w)
{
    if (!w) {
        return false;
    }
    switch (w->type) {
    case NL_WIDGET_SCENE_CHIP:
    case NL_WIDGET_SCENE_GRID:
    case NL_WIDGET_SWITCH_TILE:
    case NL_WIDGET_PIN_TOGGLE:
    case NL_WIDGET_SLIDER_TILE:
    case NL_WIDGET_STATUS_BADGE:
        return true;
    default:
        return false;
    }
}

static bool widget_is_home_content(const nl_widget_t *w)
{
    if (!w) {
        return false;
    }
    if (w->type == NL_WIDGET_SKILL_TILE || w->type == NL_WIDGET_SKILL_WIDGET) {
        return w->skill_id[0] != '\0';
    }
    if (w->type == NL_WIDGET_PHOTO_FRAME) {
        return true;
    }
    return widget_is_control_tile(w);
}

static int count_home_content_tiles(const nl_page_t *home)
{
    if (!home) {
        return 0;
    }
    int n = 0;
    for (int si = 0; si < home->section_count; si++) {
        const nl_section_t *sec = &home->sections[si];
        for (int wi = 0; wi < sec->widget_count; wi++) {
            if (widget_is_home_content(&sec->widgets[wi])) {
                n++;
            }
        }
    }
    return n;
}

/* —— 多用户屏（PLAN-HOME-MULTI P1）：只渲染当前屏，左右滑换屏 —— */
static char s_home_page_id[NL_LAYOUT_STR];
static int s_home_user_n;
static int s_home_user_idxs[NL_LAYOUT_MAX_USER_PAGES];
static int s_home_user_cur;
static constexpr int k_home_dots_h = 18;

static void home_collect_user_pages(void)
{
    s_home_user_n = 0;
    if (!s_layout) {
        return;
    }
    for (int i = 0; i < s_layout->page_count; i++) {
        if (s_layout->pages[i].system_page[0] != '\0') {
            continue;
        }
        if (s_home_user_n >= NL_LAYOUT_MAX_USER_PAGES) {
            ESP_LOGW(TAG, "UI user pages capped at %d (skip %s)", NL_LAYOUT_MAX_USER_PAGES,
                     s_layout->pages[i].id);
            break;
        }
        s_home_user_idxs[s_home_user_n++] = i;
    }
}

static void home_resolve_current(void)
{
    s_home_user_cur = 0;
    if (s_home_user_n <= 0 || !s_layout) {
        s_home_page_id[0] = '\0';
        return;
    }
    if (s_home_page_id[0] != '\0') {
        for (int i = 0; i < s_home_user_n; i++) {
            const nl_page_t *p = &s_layout->pages[s_home_user_idxs[i]];
            if (strncmp(p->id, s_home_page_id, NL_LAYOUT_STR) == 0) {
                s_home_user_cur = i;
                return;
            }
        }
    }
    s_home_user_cur = 0;
    const nl_page_t *first = &s_layout->pages[s_home_user_idxs[0]];
    std::snprintf(s_home_page_id, sizeof(s_home_page_id), "%s", first->id);
}

static int count_all_user_content_tiles(void)
{
    int n = 0;
    if (!s_layout) {
        return 0;
    }
    for (int i = 0; i < s_home_user_n; i++) {
        n += count_home_content_tiles(&s_layout->pages[s_home_user_idxs[i]]);
    }
    return n;
}

static int count_all_user_skill_tiles(void)
{
    int n = 0;
    if (!s_layout) {
        return 0;
    }
    for (int i = 0; i < s_home_user_n; i++) {
        const nl_page_t *pg = &s_layout->pages[s_home_user_idxs[i]];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                if ((sec->widgets[wi].type == NL_WIDGET_SKILL_TILE ||
                     sec->widgets[wi].type == NL_WIDGET_SKILL_WIDGET) &&
                    sec->widgets[wi].skill_id[0]) {
                    n++;
                }
            }
        }
    }
    return n;
}

static void on_skill_tile_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || w->type != NL_WIDGET_SKILL_TILE || !w->skill_id[0]) {
        nl_ov_toast("应用未配置");
        return;
    }
    const char *body = w->default_invoke[0] ? w->default_invoke : "{}";
    nl_ov_invoke_skill_async(w->skill_id, body);
}

static void fill_skill_tile(nl_widget_t *w, int cell_w, int cell_h)
{
    if (!w) {
        return;
    }
    const char *title = w->title[0] ? w->title : w->skill_id;
    lv_obj_t *tile = make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
    lv_obj_set_style_bg_color(tile, UI_TILE_OFF, 0);
    lv_obj_set_style_bg_color(tile, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    bool is_1x1 = (w->gw <= 1 && w->gh <= 1);
    bool is_row = (w->gw >= 2 && w->gh <= 1);
    if (is_row) {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 10, 0);
        lv_obj_set_style_pad_right(tile, 8, 0);
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_set_style_pad_row(tile, 6, 0);
    }
    lv_obj_add_event_cb(tile, on_skill_tile_click, LV_EVENT_CLICKED, w);

    lv_coord_t cap_size = is_row ? 28 : 42;

    lv_obj_t *capsule = lv_obj_create(tile);
    lv_obj_remove_style_all(capsule);
    lv_obj_set_user_data(capsule, (void *)(intptr_t)TILE_CHILD_CAPSULE);
    lv_obj_set_size(capsule, cap_size, cap_size);
    lv_obj_set_style_radius(capsule, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(capsule, UI_CAPSULE_SCENE, 0);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    const char *skill_glyph = MATERIAL_SYMBOLS_EXPLORE;
    if (w->icon[0]) {
        const char *named = material_symbols_get_utf8(w->icon);
        if (named != NULL) {
            skill_glyph = named;
        }
    }
    lv_obj_t *icon_lbl = lv_label_create(capsule);
    lv_label_set_text(icon_lbl, skill_glyph);
    lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
    lv_obj_set_style_text_color(icon_lbl, UI_ON_TILE, 0);
    lv_obj_center(icon_lbl);

    /* 1×1 只留图标；其余：上名称、下「应用」（不叠浮动角标） */
    if (is_1x1) {
        return;
    }
    char short_t[48];
    short_label(title, short_t, sizeof(short_t), title_max_runes(w));
    lv_obj_t *txt = lv_obj_create(tile);
    lv_obj_remove_style_all(txt);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 2, 0);
    lv_obj_clear_flag(txt, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (is_row) {
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_width(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    } else {
        lv_obj_set_width(txt, lv_pct(96));
        lv_obj_set_flex_align(txt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }

    lv_obj_t *lb = lv_label_create(txt);
    lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
    lv_label_set_text(lb, short_t[0] ? short_t : title);
    nl_ov_set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    lv_obj_set_style_text_align(lb, is_row ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, lv_pct(100));

    lv_obj_t *sub = lv_label_create(txt);
    nl_ov_set_font(sub);
    lv_label_set_text(sub, "应用");
    lv_obj_set_style_text_color(sub, UI_PURPLE, 0);
    lv_obj_set_style_text_align(sub, is_row ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_CLIP);
}

static void on_skill_widget_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    nl_widget_t *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || w->type != NL_WIDGET_SKILL_WIDGET || !w->skill_id[0]) {
        return;
    }
    const char *body = w->default_invoke[0] ? w->default_invoke : "{}";
    nl_ov_invoke_skill_async(w->skill_id, body);
}

static lv_obj_t *widget_icon_pod(lv_obj_t *parent, const char *glyph, lv_color_t bg, int sz)
{
    lv_obj_t *pod = lv_obj_create(parent);
    lv_obj_remove_style_all(pod);
    lv_obj_set_size(pod, sz, sz);
    lv_obj_set_style_radius(pod, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(pod, bg, 0);
    lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pod, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *ic = lv_label_create(pod);
    lv_label_set_text(ic, glyph);
    lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
    lv_obj_center(ic);
    return pod;
}

static void on_photo_img_deleted(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) {
        return;
    }
    auto *dsc = static_cast<lv_image_dsc_t *>(lv_event_get_user_data(e));
    if (!dsc) {
        return;
    }
    if (dsc->data) {
        heap_caps_free(const_cast<uint8_t *>(dsc->data));
        dsc->data = nullptr;
    }
    free(dsc);
}

static lv_obj_t *find_home_tile_by_widget_id(const char *widget_id)
{
    if (!s_home || !widget_id || !widget_id[0]) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(s_home, i);
        if (!ch) {
            continue;
        }
        auto *w = static_cast<const nl_widget_t *>(lv_obj_get_user_data(ch));
        if (w && w->type == NL_WIDGET_PHOTO_FRAME && strcmp(w->id, widget_id) == 0) {
            return ch;
        }
    }
    return nullptr;
}

static void photo_apply_rgb(const char *widget_id, uint8_t *rgb, size_t rgb_len, size_t w, size_t h,
                            size_t stride, bool fit_contain, uint32_t gen)
{
    if (gen != s_home_gen || !s_home || s_active_tab != NL_TAB_HOME) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return;
    }
    lv_obj_t *tile = find_home_tile_by_widget_id(widget_id);
    if (!tile || !rgb || w == 0 || h == 0) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        if (tile) {
            lv_obj_t *hint = lv_obj_get_child(tile, 0);
            if (hint && lv_obj_check_type(hint, &lv_label_class)) {
                lv_label_set_text(hint, "加载失败");
                lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
            }
        }
        return;
    }
    /* 清掉占位文案 */
    while (lv_obj_get_child_count(tile) > 0) {
        lv_obj_t *ch = lv_obj_get_child(tile, 0);
        lv_obj_del(ch);
    }
    auto *dsc = static_cast<lv_image_dsc_t *>(calloc(1, sizeof(lv_image_dsc_t)));
    if (!dsc) {
        heap_caps_free(rgb);
        lv_obj_t *lb = lv_label_create(tile);
        nl_ov_set_font(lb);
        lv_label_set_text(lb, "内存不足");
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
        lv_obj_center(lb);
        return;
    }
    dsc->data = rgb;
    dsc->data_size = rgb_len;
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = (uint32_t)w;
    dsc->header.h = (uint32_t)h;
    dsc->header.stride = (uint32_t)stride;

    lv_obj_t *img = lv_image_create(tile);
    lv_obj_set_size(img, lv_pct(100), lv_pct(100));
    lv_obj_center(img);
    lv_image_set_src(img, dsc);
    lv_image_set_inner_align(img, fit_contain ? LV_IMAGE_ALIGN_CONTAIN : LV_IMAGE_ALIGN_COVER);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, on_photo_img_deleted, LV_EVENT_DELETE, dsc);
}

static photo_rt_t *photo_rt_find(const char *widget_id)
{
    if (!widget_id || !widget_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_photo_rt_n; i++) {
        if (strcmp(s_photo_rt[i].widget_id, widget_id) == 0) {
            return &s_photo_rt[i];
        }
    }
    return nullptr;
}

static void photo_show_loading(const char *widget_id, const char *text)
{
    lv_obj_t *tile = find_home_tile_by_widget_id(widget_id);
    if (!tile) {
        return;
    }
    while (lv_obj_get_child_count(tile) > 0) {
        lv_obj_del(lv_obj_get_child(tile, 0));
    }
    lv_obj_t *lb = lv_label_create(tile);
    nl_ov_set_font(lb);
    lv_label_set_text(lb, text ? text : "加载中");
    lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    lv_obj_center(lb);
}

static void photo_fetch_async(const nl_widget_t *w, uint8_t idx)
{
    if (!w || w->photo_n == 0 || idx >= w->photo_n || !w->photo_ids[idx][0]) {
        return;
    }
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
        return;
    }
    if (!s_cfg.server_url[0] || !s_cfg.device_id[0]) {
        return;
    }
    struct photo_arg {
        nodelio_nvs_config_t cfg;
        char widget_id[NL_LAYOUT_STR];
        char photo_id[NL_PHOTO_ID_MAX];
        uint8_t fit_contain;
        uint32_t gen;
    };
    auto *arg = static_cast<photo_arg *>(malloc(sizeof(photo_arg)));
    if (!arg) {
        return;
    }
    arg->cfg = s_cfg;
    snprintf(arg->widget_id, sizeof(arg->widget_id), "%s", w->id);
    snprintf(arg->photo_id, sizeof(arg->photo_id), "%s", w->photo_ids[idx]);
    arg->fit_contain = w->photo_fit_contain;
    arg->gen = s_home_gen;

    constexpr uint32_t kStackWords = 12288 / sizeof(StackType_t);
    if (xTaskCreateWithCaps(
            [](void *p) {
                auto *a = static_cast<photo_arg *>(p);
                char base[160];
                nl_url_strip_base(a->cfg.server_url, base, sizeof(base));
                char url[420];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/photos/%s", base, a->cfg.device_id,
                         a->photo_id);
                constexpr int kJpegCap = 90 * 1024;
                uint8_t *jpeg = static_cast<uint8_t *>(
                    heap_caps_malloc((size_t)kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                size_t jpeg_n = 0;
                int status = 0;
                esp_err_t err = ESP_FAIL;
                if (jpeg && !nodelio_wifi_is_asleep() && nodelio_wifi_has_ip()) {
                    err = nodelio_http_get_bin(url, a->cfg.device_key, &status, jpeg, kJpegCap, &jpeg_n);
                }
                uint8_t *rgb = nullptr;
                size_t rgb_len = 0, iw = 0, ih = 0, stride = 0;
                if (err == ESP_OK && status == 200 && jpeg_n > 0) {
                    if (jpeg_to_image(jpeg, jpeg_n, &rgb, &rgb_len, &iw, &ih, &stride) != ESP_OK) {
                        rgb = nullptr;
                    }
                }
                if (jpeg) {
                    heap_caps_free(jpeg);
                }
                std::string wid(a->widget_id);
                const bool fit = a->fit_contain != 0;
                const uint32_t gen = a->gen;
                free(a);
                Application::GetInstance().Schedule([wid, rgb, rgb_len, iw, ih, stride, fit, gen]() {
                    DisplayLockGuard lock(nl_ov_disp());
                    photo_apply_rgb(wid.c_str(), rgb, rgb_len, iw, ih, stride, fit, gen);
                });
                vTaskDeleteWithCaps(nullptr);
            },
            "nl_photo", kStackWords, arg, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(arg);
    }
}

static const nl_widget_t *photo_widget_by_id(const char *widget_id)
{
    if (!s_layout || !widget_id) {
        return nullptr;
    }
    for (int pi = 0; pi < s_layout->page_count; pi++) {
        const nl_page_t *pg = &s_layout->pages[pi];
        if (pg->system_page[0] != '\0') {
            continue;
        }
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->type == NL_WIDGET_PHOTO_FRAME && strcmp(w->id, widget_id) == 0) {
                    return w;
                }
            }
        }
    }
    return nullptr;
}

static void photo_advance(photo_rt_t *rt)
{
    if (!rt) {
        return;
    }
    const nl_widget_t *w = photo_widget_by_id(rt->widget_id);
    if (!w || w->photo_n < 2) {
        return;
    }
    rt->idx = (uint8_t)((rt->idx + 1) % w->photo_n);
    photo_show_loading(rt->widget_id, "加载中");
    photo_fetch_async(w, rt->idx);
    const int sec = rt->interval_sec >= 3 ? rt->interval_sec : 8;
    rt->next_ms = esp_timer_get_time() / 1000 + (int64_t)sec * 1000;
}

static void on_photo_timer(lv_timer_t *t)
{
    (void)t;
    if (s_active_tab != NL_TAB_HOME || !s_home) {
        return;
    }
    if (nodelio_wifi_is_asleep()) {
        return;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    for (int i = 0; i < s_photo_rt_n; i++) {
        photo_rt_t *rt = &s_photo_rt[i];
        if (rt->paused || rt->next_ms <= 0 || now < rt->next_ms) {
            continue;
        }
        photo_advance(rt);
    }
}

static void photo_timer_ensure(void)
{
    if (s_photo_timer) {
        return;
    }
    s_photo_timer = lv_timer_create(on_photo_timer, 1000, nullptr);
}

static void on_photo_frame_click(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    auto *w = static_cast<nl_widget_t *>(lv_event_get_user_data(e));
    if (!w || w->type != NL_WIDGET_PHOTO_FRAME || w->photo_n < 2) {
        return;
    }
    photo_rt_t *rt = photo_rt_find(w->id);
    if (!rt) {
        return;
    }
    rt->paused = rt->paused ? 0 : 1;
    if (!rt->paused) {
        const int sec = rt->interval_sec >= 3 ? rt->interval_sec : 8;
        rt->next_ms = esp_timer_get_time() / 1000 + (int64_t)sec * 1000;
        nl_ov_toast("继续轮播");
    } else {
        nl_ov_toast("已暂停");
    }
}

static void photo_rt_register(const nl_widget_t *w)
{
    if (!w || w->photo_n < 2 || s_photo_rt_n >= NL_PHOTO_RT_MAX) {
        return;
    }
    photo_rt_t *rt = &s_photo_rt[s_photo_rt_n++];
    memset(rt, 0, sizeof(*rt));
    snprintf(rt->widget_id, sizeof(rt->widget_id), "%s", w->id);
    rt->idx = 0;
    rt->paused = 0;
    rt->interval_sec = w->photo_interval_sec >= 3 ? w->photo_interval_sec : 8;
    rt->next_ms = esp_timer_get_time() / 1000 + (int64_t)rt->interval_sec * 1000;
    photo_timer_ensure();
}

static void fill_photo_frame(nl_widget_t *w, int cell_w, int cell_h)
{
    if (!w) {
        return;
    }
    lv_obj_t *tile = make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
    lv_obj_set_style_bg_color(tile, UI_TILE_OFF, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lb = lv_label_create(tile);
    nl_ov_set_font(lb);
    if (w->photo_n == 0 || !w->photo_ids[0][0]) {
        lv_label_set_text(lb, "去 APP 添加");
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
        return;
    }
    lv_label_set_text(lb, "加载中");
    lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    if (w->photo_n >= 2) {
        lv_obj_add_event_cb(tile, on_photo_frame_click, LV_EVENT_CLICKED, w);
        photo_rt_register(w);
    }
    photo_fetch_async(w, 0);
}

static void fill_skill_widget(nl_widget_t *w, int cell_w, int cell_h)
{
    if (!w || !w->skill_id[0]) {
        return;
    }
    const char *fallback = w->title[0] ? w->title : w->skill_id;
    lv_obj_t *tile = make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
    lv_obj_set_style_bg_color(tile, UI_TILE_OFF, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(tile, on_skill_widget_click, LV_EVENT_CLICKED, w);

    nl_widget_preview pv{};
    const bool have = nl_ov_skill_widget_preview(w->skill_id, &pv);
    const bool one_row = w->gh <= 1;
    const char *invoke = w->default_invoke[0] ? w->default_invoke : "{}";
    nl_ov_skill_widget_prefetch(w->skill_id, invoke, w->refresh_sec > 0 ? w->refresh_sec : 600);

    const char *widget_glyph = MATERIAL_SYMBOLS_CHAT_BUBBLE;
    if (w->icon[0]) {
        const char *named = material_symbols_get_utf8(w->icon);
        if (named != NULL) {
            widget_glyph = named;
        }
    }

    /* 一行高（每日一句）：正文必须占满格子，标题单独一行会被裁掉 */
    if (one_row || !pv.list) {
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(tile, 8, 0);
        lv_obj_set_style_pad_right(tile, 8, 0);
        lv_obj_set_style_pad_column(tile, 8, 0);
        widget_icon_pod(tile, widget_glyph, UI_CAPSULE_SCENE, one_row ? 28 : 32);
        lv_obj_t *body = lv_label_create(tile);
        nl_ov_set_font(body);
        const char *text = (have && pv.n > 0 && pv.title[0][0]) ? pv.title[0] : "加载中";
        lv_label_set_text(body, text);
        lv_obj_set_style_text_color(body, have && pv.ok ? UI_TEXT : UI_TEXT_MUTED, 0);
        lv_obj_set_flex_grow(body, 1);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
        return;
    }

    /* 宽卡片：左上只留图标和标题，预报靠右，把空出来的右半边用上 */
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tile, 10, 0);
    lv_obj_set_style_pad_right(tile, 12, 0);
    lv_obj_set_style_pad_top(tile, 8, 0);
    lv_obj_set_style_pad_bottom(tile, 8, 0);
    lv_obj_set_style_pad_column(tile, 10, 0);

    lv_obj_t *side = lv_obj_create(tile);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(side, 6, 0);
    lv_obj_clear_flag(side, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    widget_icon_pod(side, widget_glyph, UI_CAPSULE_PWM, 28);
    lv_obj_t *ht = lv_label_create(side);
    nl_ov_set_font(ht);
    lv_label_set_text(ht, pv.head[0] ? pv.head : fallback);
    lv_obj_set_style_text_color(ht, UI_TEXT, 0);
    lv_label_set_long_mode(ht, LV_LABEL_LONG_DOT);

    /* 天气等 list 组件：首条 subtitle 常带「城市 · …」，提出地址显示在标题下 */
    char loc[40] = {0};
    if (have && pv.sub[0][0]) {
        const char *sep = std::strstr(pv.sub[0], " · ");
        if (sep && (size_t)(sep - pv.sub[0]) < sizeof(loc)) {
            size_t n = (size_t)(sep - pv.sub[0]);
            std::memcpy(loc, pv.sub[0], n);
            loc[n] = '\0';
        } else {
            size_t n = std::strlen(pv.sub[0]);
            if (n >= sizeof(loc)) {
                n = sizeof(loc) - 1;
            }
            std::memcpy(loc, pv.sub[0], n);
            loc[n] = '\0';
        }
    }
    if (loc[0]) {
        lv_obj_t *loc_lb = lv_label_create(side);
        nl_ov_set_font(loc_lb);
        lv_label_set_text(loc_lb, loc);
        lv_obj_set_style_text_color(loc_lb, UI_TEXT_MUTED, 0);
        lv_label_set_long_mode(loc_lb, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_width(col, lv_pct(70));
    lv_obj_set_height(col, lv_pct(100));
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    int rows = pv.n > 3 ? 3 : pv.n;
    if (rows <= 0) {
        lv_obj_t *empty = lv_label_create(col);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "加载中");
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_RIGHT, 0);
        return;
    }
    for (int i = 0; i < rows; i++) {
        const char *src = pv.title[i][0] ? pv.title[i] : "—";
        const char *sep = std::strstr(src, " · ");
        char day[24];
        const char *rest = src;
        if (sep && (size_t)(sep - src) < sizeof(day)) {
            size_t n = (size_t)(sep - src);
            std::memcpy(day, src, n);
            day[n] = '\0';
            rest = sep + 3;
        } else {
            day[0] = '\0';
        }

        lv_obj_t *row = lv_obj_create(col);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        if (day[0]) {
            lv_obj_t *dl = lv_label_create(row);
            nl_ov_set_font(dl);
            lv_label_set_text(dl, day);
            lv_obj_set_style_text_color(dl, i == 0 ? UI_TEXT_SUB : UI_TEXT_MUTED, 0);
        }
        lv_obj_t *rr = lv_label_create(row);
        nl_ov_set_font(rr);
        lv_label_set_text(rr, rest);
        lv_obj_set_style_text_color(rr, i == 0 ? UI_TEXT : UI_TEXT_DIM, 0);
        lv_label_set_long_mode(rr, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(rr, 1);
        lv_obj_set_style_text_align(rr, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(rr, day[0] ? lv_pct(70) : lv_pct(100));
    }
}

static void home_remember_current_id(void)
{
    if (!s_layout || s_home_user_n <= 0) {
        s_home_page_id[0] = '\0';
        return;
    }
    if (s_home_user_cur < 0 || s_home_user_cur >= s_home_user_n) {
        s_home_user_cur = 0;
    }
    const nl_page_t *p = &s_layout->pages[s_home_user_idxs[s_home_user_cur]];
    std::snprintf(s_home_page_id, sizeof(s_home_page_id), "%s", p->id);
}

bool nl_ov_layout_current_page_has_skill(const char *skill_id)
{
    if (!skill_id || !skill_id[0] || !s_layout || s_home_user_n <= 0) {
        return false;
    }
    if (s_home_user_cur < 0 || s_home_user_cur >= s_home_user_n) {
        return false;
    }
    const nl_page_t *home = &s_layout->pages[s_home_user_idxs[s_home_user_cur]];
    for (int si = 0; si < home->section_count; si++) {
        const nl_section_t *sec = &home->sections[si];
        for (int wi = 0; wi < sec->widget_count; wi++) {
            const nl_widget_t *w = &sec->widgets[wi];
            if ((w->type == NL_WIDGET_SKILL_TILE || w->type == NL_WIDGET_SKILL_WIDGET) &&
                w->skill_id[0] &&
                strncmp(w->skill_id, skill_id, NL_WIDGET_SKILL_ID_MAX) == 0) {
                return true;
            }
        }
    }
    return false;
}

int nl_ov_home_user_page_index(void)
{
    if (s_home_user_n <= 0) {
        return -1;
    }
    return s_home_user_cur;
}

int nl_ov_home_user_page_count(void)
{
    return s_home_user_n;
}

static void home_apply_delta(int delta)
{
    if (s_home_user_n <= 1 || delta == 0 || !s_home) {
        return;
    }
    int next = s_home_user_cur + delta;
    if (next < 0 || next >= s_home_user_n) {
        return;
    }
    s_home_user_cur = next;
    home_remember_current_id();
    nl_ov_fill_home();
}

static void home_switch_by(int delta)
{
    if (s_home_user_n <= 1 || delta == 0) {
        return;
    }
    /* 勿在 GESTURE 回调里直接 clean s_home */
    Application::GetInstance().Schedule([delta]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (!lock) {
            return;
        }
        home_apply_delta(delta);
    });
}

esp_err_t nl_ov_home_page_peek(int delta)
{
    if (delta == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_home_user_n <= 1) {
        return ESP_ERR_NOT_FOUND;
    }
    int next = s_home_user_cur + delta;
    if (next < 0 || next >= s_home_user_n) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void nl_ov_home_page_apply(int delta)
{
    home_apply_delta(delta);
}

static void on_home_gesture(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    /* 左滑 → 下一屏；右滑 → 上一屏；顶栏/底 Tab 不在 s_home 内 */
    if (dir == LV_DIR_LEFT) {
        lv_indev_wait_release(indev);
        home_switch_by(1);
    } else if (dir == LV_DIR_RIGHT) {
        lv_indev_wait_release(indev);
        home_switch_by(-1);
    }
}

static void append_home_page_dots(lv_obj_t *parent, int strip_h)
{
    if (!parent || s_home_user_n <= 1) {
        return;
    }
    lv_obj_t *dots = lv_obj_create(parent);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, lv_pct(100), k_home_dots_h);
    lv_obj_set_style_pad_column(dots, 7, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    const int yoff = strip_h > 0 ? -(strip_h + 2) : -2;
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, yoff);

    for (int i = 0; i < s_home_user_n; i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        const bool on = (i == s_home_user_cur);
        lv_obj_set_size(d, on ? 14 : 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, on ? UI_ACCENT : UI_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(d, on ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *make_launcher_card(lv_obj_t *parent, const char *icon, const char *title,
                                    const char *subtitle, lv_event_cb_t on_click)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 88);
    lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_column(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(card, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(card, 1, LV_STATE_PRESSED);
    if (on_click) {
        lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *pod = lv_obj_create(card);
    lv_obj_remove_style_all(pod);
    lv_obj_set_size(pod, 48, 48);
    lv_obj_set_style_radius(pod, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(pod, lv_color_hex(UI_C_CAPSULE_ON), 0);
    lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pod, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t *ic = lv_label_create(pod);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
    lv_obj_set_style_text_color(ic, UI_ON_TILE, 0);
    lv_obj_center(ic);

    lv_obj_t *col = lv_obj_create(card);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_clear_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *tit = lv_label_create(col);
    nl_ov_set_font(tit);
    lv_label_set_text(tit, title);
    lv_obj_set_style_text_color(tit, UI_TEXT, 0);
    lv_label_set_long_mode(tit, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tit, lv_pct(100));

    lv_obj_t *sub = lv_label_create(col);
    nl_ov_set_font(sub);
    lv_label_set_text(sub, subtitle);
    lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, lv_pct(100));
    return card;
}

/** 空控家启动台：已钉 home_tile + 全部应用入口 + 进入对话（不拉市场） */
static void fill_home_launcher(void)
{
    lv_obj_set_layout(s_home, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_home, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_home, 20, 0);
    lv_obj_set_style_pad_ver(s_home, 12, 0);
    lv_obj_set_style_pad_row(s_home, 10, 0);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_home, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_home, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *hdr = lv_obj_create(s_home);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hdr, 6, 0);
    lv_obj_clear_flag(hdr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *title = lv_label_create(hdr);
    nl_ov_set_font(title);
    lv_label_set_text(title, "启动台");
    lv_obj_set_style_text_color(title, UI_TEXT, 0);

    lv_obj_t *hint = lv_label_create(hdr);
    nl_ov_set_font(hint);
    /* P3b：多屏全空引导去 APP；有钉选则提示底入口 */
    if (s_home_user_n > 1) {
        lv_label_set_text(hint, "多屏均未编排 · 请用手机 APP「编辑大屏布局」");
    } else if (nl_ov_home_skill_pinned_count() > 0) {
        lv_label_set_text(hint, "未配置控家 · 下方为已钉应用");
    } else {
        lv_label_set_text(hint, "未配置控家 · 可先用应用与对话，或去 APP 编排");
    }
    lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));

    /* C3：已钉技能磁贴（技能通道，非 panel/command） */
    nl_ov_home_skills_append_launcher(s_home);

    make_launcher_card(
        s_home, MATERIAL_SYMBOLS_EXPLORE, "全部应用", "打开技能列表 · 手机可钉选或挂画布",
        [](lv_event_t *) { nl_ov_show_page(NL_TAB_APPS); });

    make_launcher_card(s_home, MATERIAL_SYMBOLS_MIC, "进入对话", "唤醒助手 · 语音控家或提问",
                       [](lv_event_t *) { nodelio_overlay_show_chat(); });
    if (s_have_layout && s_layout && s_layout->layout_version > 0) {
        nodelio_panel_ui_mark_home_filled(s_layout->layout_version);
    }
}

void nl_ov_fill_home(void)
{
    if (!s_home) {
        return;
    }
    s_home_gen++;
    s_photo_rt_n = 0;
    nl_ov_cmd_pending_clear_all();
    /* C3：钉选清单仅开机/invalidate 后拉一次，无定时轮询市场 */
    nl_ov_home_skills_ensure_fetched();

    lv_obj_remove_event_cb(s_home, on_home_gesture);
    lv_obj_clean(s_home);
    lv_obj_set_layout(s_home, 0);
    lv_obj_set_style_pad_left(s_home, NL_UI_HOME_PAD_H, 0);
    lv_obj_set_style_pad_right(s_home, NL_UI_HOME_PAD_H, 0);
    lv_obj_set_style_pad_top(s_home, NL_UI_HOME_PAD_V, 0);
    lv_obj_set_style_pad_bottom(s_home, NL_UI_HOME_PAD_V, 0);
    lv_obj_set_style_bg_color(s_home, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_have_layout || !s_layout) {
        /* 未拉到布局：静态「初始化中」（禁止无限 lv_anim：RGB full_refresh 易撑爆 LVGL/主任务栈） */
        lv_obj_t *card = lv_button_create(s_home);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 380, 180);
        lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_C_TILE_ON_BORDER), 0);
        lv_obj_set_style_border_opa(card, 48, 0);
        lv_obj_set_style_bg_color(card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
        lv_obj_set_style_translate_y(card, 1, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 20, 0);
        lv_obj_set_style_pad_row(card, 10, 0);
        lv_obj_center(card);
        lv_obj_add_event_cb(
            card,
            [](lv_event_t *) {
                nl_ov_show_page(NL_TAB_SET);
                nl_ov_open_connect();
            },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_t *halo_wrap = lv_obj_create(card);
        lv_obj_remove_style_all(halo_wrap);
        lv_obj_set_size(halo_wrap, 72, 72);
        lv_obj_clear_flag(halo_wrap,
                          (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

        lv_obj_t *ring = lv_obj_create(halo_wrap);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 72, 72);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 2, 0);
        lv_obj_set_style_border_color(ring, UI_ACCENT, 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_50, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_center(ring);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *pod = lv_obj_create(halo_wrap);
        lv_obj_remove_style_all(pod);
        lv_obj_set_size(pod, 48, 48);
        lv_obj_set_style_radius(pod, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pod, lv_color_hex(UI_C_CAPSULE_ON), 0);
        lv_obj_set_style_bg_opa(pod, LV_OPA_COVER, 0);
        lv_obj_center(pod);
        lv_obj_clear_flag(pod, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *glyph = lv_label_create(pod);
        lv_label_set_text(glyph, "NL");
        nl_ov_set_font(glyph);
        lv_obj_set_style_text_color(glyph, UI_ON_TILE, 0);
        lv_obj_center(glyph);

        lv_obj_t *tit = lv_label_create(card);
        lv_label_set_text(tit, "初始化中");
        nl_ov_set_font(tit);
        lv_obj_set_style_text_color(tit, UI_TEXT, 0);

        lv_obj_t *dots = lv_obj_create(card);
        lv_obj_remove_style_all(dots);
        lv_obj_set_size(dots, LV_SIZE_CONTENT, 10);
        lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(dots, 8, 0);
        lv_obj_clear_flag(dots, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        static const lv_opa_t k_dot_opa[3] = {LV_OPA_COVER, LV_OPA_70, LV_OPA_40};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *d = lv_obj_create(dots);
            lv_obj_remove_style_all(d);
            lv_obj_set_size(d, 8, 8);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(d, UI_ACCENT, 0);
            lv_obj_set_style_bg_opa(d, k_dot_opa[i], 0);
            lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_text(sub, "正在同步面板 · 点击可去网络设置");
        nl_ov_set_font(sub);
        lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);

        return;
    }

    home_collect_user_pages();
    home_resolve_current();
    if (s_home_user_n <= 0) {
        fill_home_launcher();
        return;
    }

    /* 控家关闭且无画布应用/相册 → 启动台；有 skill_tile 或相册仍显示网格 */
    if (!nodelio_controls_enabled() && count_all_user_skill_tiles() == 0 &&
        count_all_user_content_tiles() == 0) {
        fill_home_launcher();
        return;
    }

    /* 全部用户屏都无控家/应用磁贴 → 启动台；单屏空内容时仍可滑到其它屏 */
    if (count_all_user_content_tiles() == 0) {
        fill_home_launcher();
        return;
    }

    const int home_idx = s_home_user_idxs[s_home_user_cur];
    const nl_page_t *home = &s_layout->pages[home_idx];
    home_remember_current_id();

    int cols = s_layout->grid_cols > 0 ? s_layout->grid_cols : 6;
    int rows = s_layout->grid_rows > 0 ? s_layout->grid_rows : 6;
    lv_obj_update_layout(s_home);
    lv_coord_t area_w = lv_obj_get_content_width(s_home);
    lv_coord_t area_h = lv_obj_get_content_height(s_home);
    if (area_w < 40) {
        area_w = 452;
    }
    if (area_h < 40) {
        area_h = 380;
    }
    /* C4：为底部应用条预留高度，控家网格与 home_tile 同屏不互踩 */
    const int strip_h = nl_ov_home_skill_strip_h();
    if (strip_h > 0 && area_h > strip_h + 80) {
        area_h -= strip_h;
    }
    /* 多屏指示点 */
    if (s_home_user_n > 1 && area_h > k_home_dots_h + 60) {
        area_h -= k_home_dots_h;
    }
    int cell_w = (area_w - GRID_GAP * (cols - 1)) / cols;
    int cell_h = (area_h - GRID_GAP * (rows - 1)) / rows;
    if (cell_w < 20) {
        cell_w = 20;
    }
    if (cell_h < 20) {
        cell_h = 20;
    }
    int used_w = cell_w * cols + GRID_GAP * (cols - 1);
    s_grid_ox = (area_w > used_w) ? (area_w - used_w) / 2 : 0;
    s_grid_oy = 0;

    for (int si = 0; si < home->section_count; si++) {
        const nl_section_t *sec = &home->sections[si];
        for (int wi = 0; wi < sec->widget_count; wi++) {
            nl_widget_t *w = &s_layout->pages[home_idx].sections[si].widgets[wi];
            const char *title = w->title[0] ? w->title : (w->bind.name[0] ? w->bind.name : w->id);
            if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER) {
                continue;
            }
            if (w->type == NL_WIDGET_SKILL_TILE) {
                if (w->skill_id[0] == '\0') {
                    continue;
                }
                fill_skill_tile(w, cell_w, cell_h);
                continue;
            }
            if (w->type == NL_WIDGET_SKILL_WIDGET) {
                fill_skill_widget(w, cell_w, cell_h);
                continue;
            }
            if (w->type == NL_WIDGET_PHOTO_FRAME) {
                fill_photo_frame(w, cell_w, cell_h);
                continue;
            }
            if (!nodelio_controls_enabled()) {
                continue;
            }
            tile_kind_t kind = tile_kind_of(w);
            if (kind == TILE_PWM) {
                bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
                bool is_row = (w && w->gw >= 2 && w->gh <= 1);
                lv_obj_t *row = make_tile_shell(w, kind, cell_w, cell_h);
                if (is_1x1) {
                    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
                    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                    lv_obj_set_style_pad_all(row, 4, 0);
                } else if (is_row) {
                    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                    lv_obj_set_style_pad_column(row, 8, 0);
                    lv_obj_set_style_pad_left(row, 10, 0);
                    lv_obj_set_style_pad_right(row, 8, 0);
                    lv_obj_set_style_pad_top(row, 4, 0);
                    lv_obj_set_style_pad_bottom(row, 4, 0);
                } else {
                    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
                    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                         LV_FLEX_ALIGN_CENTER);
                    lv_obj_set_style_pad_all(row, 6, 0);
                    lv_obj_set_style_pad_row(row, 6, 0);
                }

                char short_t[48];
                short_label(title, short_t, sizeof(short_t), title_max_runes(w));
                lv_coord_t cap_size = is_row ? 28 : 42;
                lv_obj_t *capsule = lv_obj_create(row);
                lv_obj_remove_style_all(capsule);
                lv_obj_set_user_data(capsule, (void *)(intptr_t)TILE_CHILD_CAPSULE);
                lv_obj_set_size(capsule, cap_size, cap_size);
                lv_obj_set_style_bg_opa(capsule, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(capsule, 0, 0);
                lv_obj_clear_flag(capsule, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

                lv_obj_t *icon_lbl = lv_label_create(capsule);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, true));
                lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(icon_lbl, UI_ON_TILE, 0);
                lv_obj_center(icon_lbl);

                lv_obj_t *lb = lv_label_create(row);
                lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
                lv_label_set_text(lb, short_t[0] ? short_t : title);
                nl_ov_set_font(lb);
                lv_obj_set_style_text_color(lb, UI_TEXT, 0);
                if (is_1x1) {
                    lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
                } else if (is_row) {
                    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
                    lv_obj_set_flex_grow(lb, 1);
                    lv_obj_set_style_pad_right(lb, 26, 0);
                } else {
                    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
                    lv_obj_set_width(lb, lv_pct(96));
                    lv_obj_set_height(lb, 18);
                }
                lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

                lv_obj_t *hint = lv_label_create(row);
                lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
                lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
                lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
                lv_label_set_text(hint, "0%");
                nl_ov_set_font(hint);
                lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
                lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_ON, 0);
                lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
                style_hint_pill(hint, false);
                lv_obj_align(hint, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, is_row ? -2 : -4, is_row ? 0 : 4);

                if (is_1x1) {
                    lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
                }

                lv_obj_t *sl = lv_slider_create(row);
                if (is_1x1) {
                    lv_obj_add_flag(sl, LV_OBJ_FLAG_HIDDEN);
                }
                lv_obj_set_width(sl, lv_pct(88));
                lv_obj_set_height(sl, 8);
                lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_MAIN);
                lv_obj_set_style_radius(sl, UI_RADIUS_XS, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(sl, UI_PWM_TRACK, LV_PART_MAIN);
                lv_obj_set_style_bg_color(sl, UI_TILE_PWM_BORDER, LV_PART_INDICATOR);
                lv_obj_set_style_bg_color(sl, UI_ON_TILE, LV_PART_KNOB);
                lv_slider_set_range(sl, w->bind.min, w->bind.max > w->bind.min ? w->bind.max : 100);
                lv_obj_add_event_cb(sl, on_slider, LV_EVENT_ALL, w);
                style_tile_visual(row, w);
                continue;
            }
            lv_obj_t *tile = make_tile_shell(w, kind, cell_w, cell_h);
            if (dual_line_use_segments(w)) {
                /* 双段按钮各自响应；整卡不再翻转，避免误触 */
                lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_event_cb(tile, on_btn, LV_EVENT_CLICKED, w);
            }
            add_tile_labels(tile, w, kind, title);
            style_tile_visual(tile, w);
        }
    }

    /* P3b：本屏无磁贴但其它屏有内容 → 空态引导（仍可滑屏） */
    if (count_home_content_tiles(home) == 0) {
        lv_obj_t *empty = lv_label_create(s_home);
        nl_ov_set_font(empty);
        if (s_home_user_n > 1) {
            lv_label_set_text(empty, "本屏空 · 左右滑换屏\n或去 APP 编排本屏");
        } else {
            lv_label_set_text(empty, "本屏空 · 请用手机 APP 编排");
        }
        lv_obj_set_style_text_color(empty, UI_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, lv_pct(80));
        lv_obj_center(empty);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
    }

    /* C4：控家磁贴走 panel/command；应用条走技能 invoke，通道不串 */
    if (strip_h > 0) {
        nl_ov_home_skills_append_strip(s_home);
    }
    append_home_page_dots(s_home, strip_h);

    /* 多屏：内容区左右滑换屏（Tabbar 不跟滑）；非当前屏不建树 */
    if (s_home_user_n > 1) {
        lv_obj_add_event_cb(s_home, on_home_gesture, LV_EVENT_GESTURE, nullptr);
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s_home, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }

    if (s_have_layout && s_layout && s_layout->layout_version > 0) {
        nodelio_panel_ui_mark_home_filled(s_layout->layout_version);
    }
}

/* —— connect overlay —— */
