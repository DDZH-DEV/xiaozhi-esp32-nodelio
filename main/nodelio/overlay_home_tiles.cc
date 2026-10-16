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

#define TAG "nl_home"

static lv_timer_t *s_cd_timer;

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
    if (in[n] != '\0' && n + 2 < out_len) {
        out[n] = '.';
        out[n + 1] = '\0';
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
    if (w && w->skin[0]) {
        if (strcmp(w->skin, "fan") == 0) {
            return MATERIAL_SYMBOLS_REFRESH;
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
        if (strcmp(w->skin, "gauge") == 0 || strcmp(w->skin, "tank") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
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

static bool state_is_on(const char *st)
{
    if (!st || !st[0]) {
        return false;
    }
    if (strcasecmp(st, "ON") == 0 || strcasecmp(st, "OPEN") == 0) {
        return true;
    }
    if (strcasecmp(st, "OFF") == 0 || strcasecmp(st, "CLOSE") == 0 ||
        strcasecmp(st, "UNKNOWN") == 0) {
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

/** 通道/引脚只看对应通道态；勿回退整机 power（易把 UNKNOWN/其它路当成开）。 */
static bool widget_is_on(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w) {
        return false;
    }
    if (kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO) {
        return true;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(w->bind.device_id);
    if (!d) {
        return false;
    }
    int ch = bind_channel_index(w);
    if (ch >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == ch) {
                return state_is_on(d->ch_st[i]);
            }
        }
        return false;
    }
    return state_is_on(d->power);
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

static void flip_widget_cache(const nl_widget_t *w)
{
    if (!w) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    bool on = widget_is_on(w, kind);
    set_widget_cache_state(w, on ? "OFF" : "ON");
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

static void ensure_tile_trans(void)
{
    if (s_tile_trans_inited) {
        return;
    }
    s_tile_trans_inited = true;
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR, LV_STYLE_BORDER_COLOR,
                                            LV_STYLE_PROP_INV};
    lv_style_transition_dsc_init(&s_tile_trans, props, lv_anim_path_ease_out, 140, 0, nullptr);
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

static uint32_t widget_fill_for_state(const nl_widget_t *w, tile_kind_t kind, bool online, bool on)
{
    if (!online) {
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

static void style_tile_visual(lv_obj_t *tile, const nl_widget_t *w)
{
    if (!tile || !w) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    bool online = true;
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0]) {
        online = nodelio_panel_ui_device_online(w->bind.device_id);
    }
    bool on = widget_is_on(w, kind);
    uint32_t fill = widget_fill_for_state(w, kind, online, on);
    ensure_tile_trans();
    lv_obj_set_style_transition(tile, &s_tile_trans, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    /* 首页磁贴无外边框，保持沉浸纯净 */
    lv_obj_set_style_border_width(tile, 0, 0);
    ui_set_enabled(tile, online || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO);

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

    lv_color_t icon_c = (!online) ? UI_DISABLED_FG : (on ? active_c : UI_TEXT_DIM);
    lv_color_t title_c = (!online) ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT);
    lv_color_t hint_c = (!online) ? UI_DISABLED_FG : (on ? UI_ON_TILE : UI_TEXT_MUTED);
    lv_color_t badge_bg =
        (!online) ? UI_TILE_BADGE_OFFLINE : (on ? UI_TILE_BADGE_ON : UI_TILE_BADGE_OFF);
    if (online && on) {
        badge_bg = active_c;
    }

    bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
    bool is_row = (w && w->gw >= 2 && w->gh <= 1);
    uint32_t sub = lv_obj_get_child_count(tile);
    for (uint32_t j = 0; j < sub; j++) {
        lv_obj_t *ch = lv_obj_get_child(tile, j);
        intptr_t role = (intptr_t)lv_obj_get_user_data(ch);
        if (role == TILE_CHILD_CAPSULE) {
            /* 彻底去除图标底座框框，保持 100% 透明无框 */
            lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(ch, 0, 0);
            if (lv_obj_get_child_count(ch) > 0) {
                lv_obj_t *icon_lbl = lv_obj_get_child(ch, 0);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(icon_lbl, icon_c, 0);
                lv_obj_set_style_text_opa(icon_lbl, (on && online) ? LV_OPA_COVER : LV_OPA_80, 0);
                lv_obj_center(icon_lbl);
            }
        } else if (lv_obj_check_type(ch, &lv_label_class)) {
            if (role == TILE_CHILD_ICON) {
                lv_label_set_text(ch, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(ch, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(ch, icon_c, 0);
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
                } else if (w && strcmp(w->skin, "dual_line") == 0) {
                    /* DualLine 互斥双段：开态显示 NC 名，关态显示 NO 名 */
                    const char *nc = (w->labels_on[0]) ? w->labels_on : "NC";
                    const char *no = (w->labels_off[0]) ? w->labels_off : "NO";
                    lv_label_set_text(ch, (!online) ? "离" : (on ? nc : no));
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
    uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(child));
        if (!w) {
            continue;
        }
        style_tile_visual(child, w);
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

static void capture_and_upload_screenshot(void)
{
    Display *disp = Board::GetInstance().GetDisplay();
    if (!disp) {
        nl_ov_toast("未检测到显示屏");
        return;
    }
    /* 播放器挂在 lv_layer_top()，与首页同级。拍整层，不要只拍 s_root。 */
    lv_obj_t *snap = lv_layer_top();
    std::string jpeg;
    if (!disp->SnapshotToJpeg(jpeg, 80, snap) || jpeg.empty()) {
        ESP_LOGW(TAG, "SnapshotToJpeg failed");
        nl_ov_toast("截屏生成失败");
        return;
    }

    if (s_cfg.server_url[0] == '\0' || s_cfg.device_id[0] == '\0') {
        nl_ov_toast("未配置服务器");
        return;
    }

    char base[160];
    nl_url_strip_base(s_cfg.server_url, base, sizeof(base));
    char url[256];
    std::snprintf(url, sizeof(url), "%s/api/devices/%s/panel/screenshot", base, s_cfg.device_id);

    int http_status = 0;
    esp_err_t err = nodelio_http_post_image(url, s_cfg.device_key, jpeg.data(), jpeg.size(), &http_status);
    if (err == ESP_OK && http_status >= 200 && http_status < 300) {
        ESP_LOGI(TAG, "Screenshot uploaded successfully: %u bytes", (unsigned)jpeg.size());
        nl_ov_toast("已上传实机截图");
    } else {
        ESP_LOGW(TAG, "Screenshot upload failed err=%s http=%d", esp_err_to_name(err), http_status);
        nl_ov_toast("截图上传失败");
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

struct async_cmd_item_t {
    char widget_id[NL_LAYOUT_STR];
    nl_bind_t bind;
    char value[16];
    bool rollback_on_fail;
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
        esp_err_t err = nodelio_panel_command(&s_cfg, item.widget_id, "set", &item.bind, item.value, &http);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "async cmd %s http=%d err=%s", item.widget_id, http, esp_err_to_name(err));
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_toast(http == 503 ? "设备未在线" : "操作未响应");
                if (item.rollback_on_fail) {
                    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
                    if (layout) {
                        for (int pi = 0; pi < layout->page_count; pi++) {
                            const nl_page_t *pg = &layout->pages[pi];
                            for (int si = 0; si < pg->section_count; si++) {
                                const nl_section_t *sec = &pg->sections[si];
                                for (int wi = 0; wi < sec->widget_count; wi++) {
                                    if (strcmp(sec->widgets[wi].id, item.widget_id) == 0) {
                                        flip_widget_cache(&sec->widgets[wi]);
                                        nl_ov_refresh_home_tiles();
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            s_cloud_ok = true;
            DisplayLockGuard lock(nl_ov_disp());
            if (lock && s_cloud_lbl) {
                nl_ov_refresh_status_icons(s_wifi_ok, true, nullptr);
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
        xTaskCreate(async_cmd_worker, "nl_cmd_async", ASYNC_CMD_STACK_BYTES, nullptr, 3, &s_cmd_task);
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
        xQueueSend(s_cmd_queue, &item, 0);
    }
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
        /* 轻反馈：按下后闪一下边框 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
        }
        return;
    }
    if (w && strcmp(w->skin, "momentary") == 0) {
        if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
            !nodelio_panel_ui_device_online(w->bind.device_id)) {
            nl_ov_toast("设备未在线");
            return;
        }
        /* 点动：下发触发脉冲，轻微边框反馈，不把本地缓存翻成开态 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
        }
        dispatch_async_cmd(w, "TOGGLE", false);
        return;
    }
    /* 先翻本地态，再异步下发绝对 ON/OFF；联动同引脚所有卡片瞬间同步响应（0ms 响应，完全不卡死 UI） */
    flip_widget_cache(w);
    nl_ov_refresh_home_tiles();
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0] &&
        !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        flip_widget_cache(w);
        nl_ov_refresh_home_tiles();
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    const char *want = widget_is_on(w, kind) ? "ON" : "OFF";
    dispatch_async_cmd(w, want, true);
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
    uint32_t fill = widget_fill_for_state(w, kind, true, false);
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

static void add_tile_labels(lv_obj_t *tile, const nl_widget_t *w, tile_kind_t kind, const char *title)
{
    bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
    bool is_row = (w && w->gw >= 2 && w->gh <= 1);
    bool is_large = (w && (w->gw >= 2 && w->gh >= 2));
    bool is_col_slim = (w && w->gw <= 1 && w->gh >= 2);

    char short_t[24];
    short_label(title, short_t, sizeof(short_t), is_row ? 5 : 3);

    if (is_1x1) {
        /* 1x1 极小单格：图标居中，无底部标题；「离」角标浮在右上角 */
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(tile, 4, 0);
    } else if (is_row) {
        /* 行高为 1 且宽 >= 2：左右结构（左图标胶囊、中标题、右角标真正靠右） */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 10, 0);
        lv_obj_set_style_pad_right(tile, 8, 0); /* 贴近右边界，右侧状态微标真正靠右 */
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else if (is_col_slim) {
        /* 瘦高卡片（1x2 / 1x3）：上下结构，图标与标题在中央成组紧凑对齐，避免名称沉底或中间过度空旷 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 4, 0);
        lv_obj_set_style_pad_row(tile, 8, 0);
    } else {
        /* 大方块（>=2x2）：两行及以上大方块，图标与标题在中央成组对齐，层次分明，微标置于右上角 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_set_style_pad_row(tile, is_large ? 10 : 8, 0);
    }

    /* 1. 图标容器（无底座框框，纯净大图标直接悬浮在磁贴上） */
    lv_coord_t cap_size = is_row ? 28 : (is_large ? 46 : (is_col_slim ? 36 : 42));
    if (is_1x1) {
        /* 1x1：图标直接 center 对齐，不通过 flex 胶囊容器 */
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

        /* 矢量图标：非横条全用 30px 超大矢量图标 */
        lv_obj_t *icon_lbl = lv_label_create(capsule);
        lv_label_set_text(icon_lbl, kind_symbol(w, kind, false));
        lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
        lv_obj_set_style_text_color(icon_lbl, UI_TEXT_DIM, 0);
        lv_obj_center(icon_lbl);
    }

    /* 2. 标题文字（1x1 隐藏） */
    lv_obj_t *lb = lv_label_create(tile);
    lv_obj_set_user_data(lb, (void *)(intptr_t)TILE_CHILD_TITLE);
    lv_label_set_text(lb, short_t[0] ? short_t : title);
    nl_ov_set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    if (is_1x1) {
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
    } else if (is_row) {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_flex_grow(lb, 1);
        lv_obj_set_style_pad_right(lb, 26, 0); /* 预留右侧浮动药丸空隙，长文字自动打点截断 */
    } else {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lb, lv_pct(94));
        lv_obj_set_height(lb, LV_SIZE_CONTENT); /* 高度自适应，不硬编码 */
    }
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);

    /* 3. 药丸浮动角标：左右结构靠中右，上下结构靠右上预留呼吸空隙 */
    lv_obj_t *hint = lv_label_create(tile);
    lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
    lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
    lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_OFF, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
    style_hint_pill(hint, false);
    lv_obj_align(hint, is_row ? LV_ALIGN_RIGHT_MID : LV_ALIGN_TOP_RIGHT, is_row ? -2 : -4, is_row ? 0 : 4);

    if (is_1x1) {
        /* 1x1 正常在线隐藏微标，只在离线时展示 */
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
    } else if (w && strcmp(w->skin, "dual_line") == 0) {
        const char *no = (w->labels_off[0]) ? w->labels_off : "NO";
        lv_label_set_text(hint, no);
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    } else {
        lv_label_set_text(hint, "关");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    }
}

void nl_ov_fill_home(void)
{
    if (!s_home) {
        return;
    }
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

    int home_idx = -1;
    for (int i = 0; i < s_layout->page_count; i++) {
        if (s_layout->pages[i].system_page[0] == '\0') {
            home_idx = i;
            break;
        }
    }
    if (home_idx < 0) {
        lv_obj_t *hint = lv_label_create(s_home);
        lv_label_set_text(hint, "无首页\n请在 APP 编辑布局");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_SUB, 0);
        lv_obj_center(hint);
        return;
    }

    const nl_page_t *home = &s_layout->pages[home_idx];
    int widget_total = 0;
    for (int si = 0; si < home->section_count; si++) {
        widget_total += home->sections[si].widget_count;
    }
    if (widget_total == 0) {
        lv_obj_t *hint = lv_label_create(s_home);
        lv_label_set_text(hint, "首页为空\nAPP 添加开关后自动刷新");
        nl_ov_set_font(hint);
        lv_obj_set_style_text_color(hint, UI_TEXT_SUB, 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(hint);
        return;
    }

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
            tile_kind_t kind = tile_kind_of(w);
            if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER) {
                continue;
            }
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

                char short_t[24];
                short_label(title, short_t, sizeof(short_t), is_row ? 5 : 3);
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
            lv_obj_add_event_cb(tile, on_btn, LV_EVENT_CLICKED, w);
            add_tile_labels(tile, w, kind, title);
            style_tile_visual(tile, w);
        }
    }
}

/* —— connect overlay —— */
