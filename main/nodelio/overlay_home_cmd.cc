#include "overlay_priv.h"
#include "panel_jobs.h"

#include "application.h"
#include "display.h"
#include "layout_cache.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "ui_theme.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>
#include <nvs.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "nl_cmd"

static lv_timer_t *s_cd_timer;

void nl_ov_format_countdown(int sec, char *buf, size_t buflen)
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
void nl_ov_style_hint_pill(lv_obj_t *hint, bool countdown)
{
    if (!hint) {
        return;
    }
    nl_ov_set_font_compact(hint);
    lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(hint, countdown ? 5 : 4, 0);
    lv_obj_set_style_pad_right(hint, countdown ? 5 : 4, 0);
    lv_obj_set_style_pad_top(hint, 1, 0);
    lv_obj_set_style_pad_bottom(hint, 1, 0);
    /* 数字倒计时略宽，避免「9:59」贴边显偏 */
    if (countdown) {
        lv_obj_set_style_min_width(hint, 28, 0);
    } else {
        lv_obj_set_style_min_width(hint, 18, 0);
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
    const bool visible = nl_ov_home_is_visible_and_clean();
    const uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; visible && i < cnt && n < 24; i++) {
        lv_obj_t *tile = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(tile));
        if (!w || nl_ov_widget_countdown(w) <= 0) {
            continue;
        }
        active[n].tile = tile;
        active[n].w = w;
        n++;
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
    if (!visible) {
        nl_ov_home_mark_state_dirty();
        return;
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
            nl_ov_format_countdown(cd, hb, sizeof(hb));
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(hint, hb);
        } else {
            nl_ov_style_tile_visual(tile, w);
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
        /* 单路开关（RELAY 等）snapshot 只有整机 powerState，没有 channels。
           点按会把通道 0 乐观写成 ON，随后 snapshot 清掉通道行，卡片又变关。
           仅通道 0 且本机没有任何通道行时回退整机态；已有通道行不回退，避免串路。 */
        if (ch == 0 && d->ch_n == 0 && d->power[0]) {
            return d->power;
        }
        return nullptr;
    }
    return d->power;
}

const char *nl_ov_state_label_zh(const char *st)
{
    if (state_is_unknown(st)) {
        return "未知";
    }
    return state_is_on(st) ? "开" : "关";
}

const char *nl_ov_widget_cache_state(const nl_widget_t *w)
{
    return widget_cache_state(w);
}

/** 通道/引脚只看对应通道态；勿回退整机 power（易把 UNKNOWN/其它路当成开）。 */
bool nl_ov_widget_is_on(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w) {
        return false;
    }
    if (kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO ||
        w->bind.kind == NL_BIND_ACTION_BUTTON) {
        return true;
    }
    const char *st = widget_cache_state(w);
    if (state_is_unknown(st)) {
        return false;
    }
    return state_is_on(st);
}

bool nl_ov_widget_state_unknown(const nl_widget_t *w, tile_kind_t kind)
{
    if (!w || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO ||
        w->bind.kind == NL_BIND_ACTION_BUTTON) {
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

int nl_ov_widget_pwm_duty(const nl_widget_t *w)
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

void nl_ov_set_widget_cache_state(const nl_widget_t *w, const char *st)
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
    if (!w || !w->bind.device_id[0] || w->bind.kind == NL_BIND_HOME_SCENARIO ||
        w->bind.kind == NL_BIND_ACTION_BUTTON) {
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

/* —— 首页开关：点按乐观写缓存；HTTP 成败定生死；失败/超时回滚。不转圈等 snapshot —— */
#define NL_CMD_PENDING_MAX 10  // 8 queued + 1 running + 1 tentative submission
#define NL_CMD_ROLLBACK_MS 8000

struct cmd_pending_t {
    bool active;
    bool hold; /* 语音已下发成功：快照对上 want 之前，不许旧 power 把卡片刷回去 */
    uint32_t sequence;
    char widget_id[NL_LAYOUT_STR];
    char device_id[NL_LAYOUT_STR];
    int16_t channel;
    char from[12]; /* 点击前缓存态，失败回滚用 */
    char want[12];
    int64_t start_us;
    int64_t deadline_us;
};

static cmd_pending_t s_cmd_pending[NL_CMD_PENDING_MAX];
static lv_timer_t *s_pending_timer;
static uint32_t s_cmd_sequence;

static uint32_t cmd_pending_sequence(const char* widget_id) {
    for (const auto& pending : s_cmd_pending) {
        if (pending.active && strcmp(pending.widget_id, widget_id) == 0)
            return pending.sequence;
    }
    return 0;
}

bool nl_ov_widget_is_cmd_pending(const nl_widget_t *w)
{
    if (!w || !w->id[0]) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        if (s_cmd_pending[i].active && strcmp(s_cmd_pending[i].widget_id, w->id) == 0) {
            /* 仅在点击后 350ms 内防抖（防电容屏微抖连点）；超过 350ms 允许用户继续操作 */
            if (now - s_cmd_pending[i].start_us < 350000) {
                return true;
            }
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
    p->hold = false;
    p->widget_id[0] = '\0';
    p->device_id[0] = '\0';
    p->from[0] = '\0';
    p->want[0] = '\0';
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

static void cmd_pending_begin(const nl_widget_t* w) {
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
    cmd_pending_t* p = &s_cmd_pending[slot];
    p->active = true;
    if (++s_cmd_sequence == 0)
        ++s_cmd_sequence;
    p->sequence = s_cmd_sequence;
    std::snprintf(p->widget_id, sizeof(p->widget_id), "%s", w->id);
    std::snprintf(p->device_id, sizeof(p->device_id), "%s", w->bind.device_id);
    p->channel = (int16_t)bind_channel_index(w);
    const char* cur = widget_cache_state(w);
    if (cur && cur[0]) {
        std::snprintf(p->from, sizeof(p->from), "%s", cur);
    } else {
        std::snprintf(p->from, sizeof(p->from), "%s", "UNKNOWN");
    }
    p->start_us = esp_timer_get_time();
    p->deadline_us = p->start_us + (int64_t)NL_CMD_ROLLBACK_MS * 1000;
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
        if (p->hold) {
            /* 口令已成功，超时后改信后续 snapshot，不把灯刷回旧态 */
            cmd_pending_clear_slot(p);
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
    uint32_t sequence;
};

static void async_cmd_worker(void* arg) {
    const auto item = *static_cast<async_cmd_item_t*>(arg);
    free(arg);
    int http = 0;
    nodelio_nvs_config_t cmd_cfg{};
    nl_ov_cfg_for_http(&cmd_cfg);
    /* 点按已写缓存；HTTP 成功不再二次乐观（DIOL 成功仍会落真态） */
    esp_err_t err = nodelio_panel_command_ex(&cmd_cfg, item.widget_id, "set", &item.bind,
                                             item.value, &http, !item.rollback_on_fail);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "async cmd %s http=%d err=%s", item.widget_id, http, esp_err_to_name(err));
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            const char* msg = "操作未响应";
            if (http == 503) {
                msg = "设备未在线";
            } else if (http == 401) {
                msg = "需口令";
            }
            nl_ov_toast(msg);
            if (item.rollback_on_fail && item.sequence == cmd_pending_sequence(item.widget_id)) {
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
            if (item.rollback_on_fail && item.sequence == cmd_pending_sequence(item.widget_id)) {
                cmd_pending_clear_widget(item.widget_id);
            }
        }
    }
}

void nl_ov_dispatch_async_cmd(const nl_widget_t* w, const char* value, bool rollback_on_fail) {
    if (!w)
        return;
    auto* item = static_cast<async_cmd_item_t*>(calloc(1, sizeof(async_cmd_item_t)));
    if (item) {
        std::snprintf(item->widget_id, sizeof(item->widget_id), "%s", w->id);
        item->bind = w->bind;
        std::snprintf(item->value, sizeof(item->value), "%s", value ? value : "TOGGLE");
        item->rollback_on_fail = rollback_on_fail;
        item->sequence = cmd_pending_sequence(w->id);
        if (nodelio::SubmitPanelJob(nodelio::PanelJobLane::Control, async_cmd_worker, item,
                                    [](void* p) { free(p); }))
            return;
        free(item);
    }
    // Never evict an accepted control command to make room for a newer one.
    if (rollback_on_fail) {
        cmd_pending_rollback_widget(w->id);
        nl_ov_refresh_home_tiles();
    }
    nl_ov_toast("操作繁忙，请稍后重试");
}

/** 首页开关：记 from → 乐观写缓存 → 下发；失败回滚。 */
void nl_ov_home_switch_commit(nl_widget_t *w, const char *want)
{
    if (!w || !want || !want[0]) {
        return;
    }
    cmd_pending_begin(w);
    nl_ov_set_widget_cache_state(w, want);
    nl_ov_refresh_home_tiles();
    nl_ov_dispatch_async_cmd(w, want, true);
}

void nl_ov_widget_cmd(const nl_widget_t *w, const char *value)
{
    if (!w) {
        return;
    }
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.kind != NL_BIND_ACTION_BUTTON &&
        w->bind.device_id[0] && !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    nl_ov_dispatch_async_cmd(w, value, false);
}

/** 已收到过至少一次 snapshot；之前未登记设备按离线画，避免亮→暗集体闪。 */
static bool s_have_device_snapshot = false;

extern "C" bool nodelio_panel_ui_device_online(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return true;
    }
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        /* snapshot 前未登记 → 离线（暗）；snapshot 后未列 → 不拦（兼容旧逻辑） */
        return s_have_device_snapshot;
    }
    return d->online;
}

/* 局域网设备：云端 online 恒 false / 通道 stub OFF，跨 snapshot 保留上次探测结果防闪烁；放 PSRAM 省 ~28.8KB 片内 BSS */
static nl_dev_cache_t *s_lan_sticky;
static int s_lan_sticky_n = 0;
/** snapshot_begin 前视觉签名；end 时相同则跳过整页刷磁贴 */
static uint32_t s_snap_vis_before;
static uint32_t s_lan_pack_sig_saved;
static int64_t s_lan_nvs_backoff_until_ms;
#define LAN_NVS_BACKOFF_MS (10 * 60 * 1000)

static bool ensure_lan_sticky(void)
{
    if (s_lan_sticky) {
        return true;
    }
    s_lan_sticky = static_cast<nl_dev_cache_t *>(
        heap_caps_calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_lan_sticky) {
        s_lan_sticky = static_cast<nl_dev_cache_t *>(calloc(NL_DEV_CACHE_MAX, sizeof(nl_dev_cache_t)));
    }
    return s_lan_sticky != nullptr;
}

static const nl_dev_cache_t *lan_sticky_find(const char *device_id)
{
    if (!s_lan_sticky || !device_id || !device_id[0]) {
        return nullptr;
    }
    for (int i = 0; i < s_lan_sticky_n; i++) {
        if (strcmp(s_lan_sticky[i].id, device_id) == 0) {
            return &s_lan_sticky[i];
        }
    }
    return nullptr;
}

static uint32_t mix_u32(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 16777619u;
    return h;
}

static uint32_t mix_str(uint32_t h, const char *s)
{
    if (!s) {
        return mix_u32(h, 0);
    }
    while (*s) {
        h = mix_u32(h, (uint8_t)*s++);
    }
    return h;
}

extern "C" uint32_t nodelio_panel_ui_dev_visual_sig(void)
{
    nl_ov_ensure_devs_cache();
    uint32_t h = 2166136261u;
    h = mix_u32(h, (uint32_t)s_dev_n);
    if (!s_devs) {
        return h;
    }
    for (int i = 0; i < s_dev_n; i++) {
        const nl_dev_cache_t *d = &s_devs[i];
        h = mix_str(h, d->id);
        h = mix_u32(h, d->online ? 1u : 0u);
        h = mix_u32(h, d->pass_required ? 1u : 0u);
        h = mix_str(h, d->power);
        h = mix_u32(h, (uint32_t)(uint16_t)d->countdown);
        h = mix_u32(h, d->ch_n);
        for (uint8_t c = 0; c < d->ch_n; c++) {
            h = mix_u32(h, (uint32_t)(uint16_t)d->ch_idx[c]);
            h = mix_str(h, d->ch_st[c]);
            h = mix_u32(h, (uint32_t)(uint16_t)d->ch_cd[c]);
        }
    }
    return h;
}

static uint32_t lan_pack_sig_now(void)
{
    nl_ov_ensure_devs_cache();
    uint32_t h = 2166136261u;
    if (!s_devs) {
        return h;
    }
    for (int i = 0; i < s_dev_n; i++) {
        if (!s_devs[i].lan_control || !s_devs[i].id[0] || !s_devs[i].ip[0]) {
            continue;
        }
        h = mix_str(h, s_devs[i].id);
        h = mix_str(h, s_devs[i].ip);
        h = mix_u32(h, s_devs[i].pass_required ? 1u : 0u);
    }
    return h;
}

extern "C" void nodelio_panel_ui_snapshot_begin(void)
{
    s_snap_vis_before = nodelio_panel_ui_dev_visual_sig();
    s_lan_sticky_n = 0;
    if (s_devs && ensure_lan_sticky()) {
        for (int i = 0; i < s_dev_n && s_lan_sticky_n < NL_DEV_CACHE_MAX; i++) {
            if (!s_devs[i].lan_control || !s_devs[i].id[0]) {
                continue;
            }
            s_lan_sticky[s_lan_sticky_n++] = s_devs[i];
        }
    }
    s_dev_n = 0;
}

extern "C" void nodelio_panel_ui_snapshot_add(const char *device_id, const char *name, bool online,
                                              const char *power_state)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs || !device_id || !device_id[0] || s_dev_n >= NL_DEV_CACHE_MAX) {
        return;
    }
    nl_dev_cache_t *d = &s_devs[s_dev_n++];
    std::memset(d, 0, sizeof(*d));
    std::snprintf(d->id, sizeof(d->id), "%s", device_id);
    if (name && name[0]) {
        std::snprintf(d->name, sizeof(d->name), "%s", name);
    }
    d->online = online;
    if (power_state && power_state[0]) {
        std::snprintf(d->power, sizeof(d->power), "%s", power_state);
    }
}

extern "C" void nodelio_panel_ui_snapshot_set_lan(const char *device_id, const char *ip, bool lan_control,
                                                  bool has_pass)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->lan_control = lan_control;
    d->pass_required = has_pass;
    if (ip && ip[0]) {
        std::snprintf(d->ip, sizeof(d->ip), "%s", ip);
    } else {
        d->ip[0] = '\0';
    }
    if (!lan_control) {
        return;
    }
    /* 忽略云端 online=false / stub 通道：恢复上次探测态。
     * 首次见默认离线（黑）：勿乐观在线，否则重刷/冷启动死设备会先黄再黑。 */
    const nl_dev_cache_t *prev = lan_sticky_find(device_id);
    if (prev) {
        d->online = prev->online;
        if (prev->power[0]) {
            std::snprintf(d->power, sizeof(d->power), "%s", prev->power);
        }
        d->ch_n = prev->ch_n;
        std::memcpy(d->ch_idx, prev->ch_idx, sizeof(d->ch_idx));
        std::memcpy(d->ch_name, prev->ch_name, sizeof(d->ch_name));
        std::memcpy(d->ch_st, prev->ch_st, sizeof(d->ch_st));
        std::memcpy(d->ch_cd, prev->ch_cd, sizeof(d->ch_cd));
        d->countdown = prev->countdown;
    } else {
        d->online = false;
        d->ch_n = 0;
        d->power[0] = '\0';
    }
}

extern "C" void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index,
                                                      const char *ch_name, const char *state)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d || !state || channel_index < 0 || d->ch_n >= NL_CH_PER_DEV) {
        return;
    }
    /* 同 index 已存在则覆盖（LAN 探测刷新） */
    for (uint8_t i = 0; i < d->ch_n; i++) {
        if (d->ch_idx[i] == (int16_t)channel_index) {
            if (ch_name && ch_name[0]) {
                std::snprintf(d->ch_name[i], sizeof(d->ch_name[0]), "%s", ch_name);
            }
            std::snprintf(d->ch_st[i], sizeof(d->ch_st[0]), "%s", state);
            return;
        }
    }
    d->ch_idx[d->ch_n] = (int16_t)channel_index;
    if (ch_name && ch_name[0]) {
        std::snprintf(d->ch_name[d->ch_n], sizeof(d->ch_name[0]), "%s", ch_name);
    }
    std::snprintf(d->ch_st[d->ch_n], sizeof(d->ch_st[0]), "%s", state);
    d->ch_cd[d->ch_n] = 0;
    d->ch_n++;
}

extern "C" void nodelio_panel_ui_set_lan_online(const char *device_id, bool online, const char *power_state)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->online = online;
    if (power_state && power_state[0]) {
        std::snprintf(d->power, sizeof(d->power), "%s", power_state);
    }
}

extern "C" void nodelio_panel_ui_set_lan_pass_required(const char *device_id, bool need)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->pass_required = need;
}

extern "C" void nodelio_panel_ui_clear_lan_channels(const char *device_id)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return;
    }
    d->ch_n = 0;
    std::memset(d->ch_idx, 0, sizeof(d->ch_idx));
    std::memset(d->ch_name, 0, sizeof(d->ch_name));
    std::memset(d->ch_st, 0, sizeof(d->ch_st));
    std::memset(d->ch_cd, 0, sizeof(d->ch_cd));
}

extern "C" void nodelio_panel_ui_refresh_tiles(void)
{
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

extern "C" void nodelio_panel_ui_note_countdown(const char *device_id, int channel_index, int seconds)
{
    nl_ov_set_countdown(device_id, channel_index, seconds);
}

extern "C" void nodelio_panel_ui_set_countdown(const char *device_id, int channel_index, int seconds)
{
    nl_ov_set_countdown(device_id, channel_index, seconds);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

#define LAN_NVS_NS "nodelio"
#define LAN_NVS_KEY "lan_devs"
#define LAN_PACK_MAX 8

typedef struct {
    char id[NL_LAYOUT_STR];
    char ip[48];
    uint8_t pass_required;
} nl_lan_pack_t;

typedef struct {
    uint8_t n;
    nl_lan_pack_t items[LAN_PACK_MAX];
} nl_lan_blob_t;

typedef struct {
    uint32_t pack_sig;
    nl_lan_blob_t blob;
} nl_lan_save_job_t;

extern "C" void nodelio_run_on_app_task(void (*fn)(void *), void *arg)
{
    if (!fn) {
        return;
    }
    Application::GetInstance().Schedule([fn, arg]() { fn(arg); });
}

static void lan_cache_save_flash(void *arg)
{
    nl_lan_save_job_t *job = static_cast<nl_lan_save_job_t *>(arg);
    if (!job) {
        return;
    }
    nl_lan_blob_t *blob = &job->blob;
    nvs_handle_t h;
    if (nvs_open(LAN_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        heap_caps_free(job);
        return;
    }
    esp_err_t err = nvs_set_blob(h, LAN_NVS_KEY, blob, sizeof(*blob));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        (void)nvs_erase_key(h, LAN_NVS_KEY);
        (void)nvs_erase_key(h, "apps_list"); /* 可重建；勿挤掉 server_url */
        err = nvs_set_blob(h, LAN_NVS_KEY, blob, sizeof(*blob));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_lan_nvs_backoff_until_ms = 0;
        s_lan_pack_sig_saved = job->pack_sig;
        ESP_LOGI(TAG, "lan cache saved n=%d", blob->n);
    } else {
        /* 满盘时退避，避免每次 snapshot 都打 WARN + 擦写 flash；成功前不记 sig */
        s_lan_nvs_backoff_until_ms = esp_timer_get_time() / 1000 + LAN_NVS_BACKOFF_MS;
        ESP_LOGW(TAG, "lan cache save: %s (backoff %ds)", esp_err_to_name(err),
                 LAN_NVS_BACKOFF_MS / 1000);
    }
    heap_caps_free(job);
}

static void lan_cache_save(void)
{
    /* layout_sync 在 PSRAM 栈：禁止本任务写 NVS（会 assert cache_disabled） */
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_lan_nvs_backoff_until_ms > 0 && now_ms < s_lan_nvs_backoff_until_ms) {
        return;
    }
    const uint32_t pack_sig = lan_pack_sig_now();
    if (pack_sig == s_lan_pack_sig_saved && s_lan_pack_sig_saved != 0) {
        return; /* IP/口令清单未变，勿反复写 NVS */
    }
    nl_ov_ensure_devs_cache();
    if (!s_devs) {
        return;
    }
    nl_lan_save_job_t *job = static_cast<nl_lan_save_job_t *>(
        heap_caps_calloc(1, sizeof(*job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job) {
        job = static_cast<nl_lan_save_job_t *>(calloc(1, sizeof(*job)));
    }
    if (!job) {
        return;
    }
    job->pack_sig = pack_sig;
    for (int i = 0; i < s_dev_n && job->blob.n < LAN_PACK_MAX; i++) {
        if (!s_devs[i].lan_control || !s_devs[i].id[0] || !s_devs[i].ip[0]) {
            continue;
        }
        nl_lan_pack_t *p = &job->blob.items[job->blob.n++];
        std::snprintf(p->id, sizeof(p->id), "%s", s_devs[i].id);
        std::snprintf(p->ip, sizeof(p->ip), "%s", s_devs[i].ip);
        p->pass_required = s_devs[i].pass_required ? 1 : 0;
    }
    if (job->blob.n == 0) {
        heap_caps_free(job);
        s_lan_pack_sig_saved = pack_sig;
        return;
    }
    nodelio_run_on_app_task(lan_cache_save_flash, job);
}

static esp_err_t lan_cache_load_nvs_impl(void *arg)
{
    nl_lan_blob_t *blob = static_cast<nl_lan_blob_t *>(arg);
    nvs_handle_t h;
    esp_err_t err = nvs_open(LAN_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = sizeof(*blob);
    err = nvs_get_blob(h, LAN_NVS_KEY, blob, &len);
    nvs_close(h);
    return err;
}

extern "C" void nodelio_panel_ui_lan_cache_load(void)
{
    nl_lan_blob_t *blob = static_cast<nl_lan_blob_t *>(
        heap_caps_calloc(1, sizeof(*blob), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!blob) {
        blob = static_cast<nl_lan_blob_t *>(calloc(1, sizeof(*blob)));
    }
    if (!blob) {
        return;
    }
    esp_err_t err = nodelio_flash_safe_run(lan_cache_load_nvs_impl, blob);
    if (err != ESP_OK || blob->n == 0) {
        heap_caps_free(blob);
        return;
    }
    if (blob->n > LAN_PACK_MAX) {
        blob->n = LAN_PACK_MAX;
    }
    nl_ov_ensure_devs_cache();
    if (!s_devs) {
        heap_caps_free(blob);
        return;
    }
    for (int i = 0; i < blob->n; i++) {
        const nl_lan_pack_t *p = &blob->items[i];
        if (!p->id[0] || !p->ip[0]) {
            continue;
        }
        nl_dev_cache_t *d = nl_ov_find_dev(p->id);
        if (!d) {
            if (s_dev_n >= NL_DEV_CACHE_MAX) {
                continue;
            }
            d = &s_devs[s_dev_n++];
            std::memset(d, 0, sizeof(*d));
            std::snprintf(d->id, sizeof(d->id), "%s", p->id);
        }
        std::snprintf(d->ip, sizeof(d->ip), "%s", p->ip);
        d->lan_control = true;
        d->pass_required = p->pass_required != 0;
        /* 只恢复 IP；在线态等 DIOL 探测，避免重刷后死设备先亮黄 */
        d->online = false;
    }
    s_lan_pack_sig_saved = lan_pack_sig_now();
    ESP_LOGI(TAG, "lan cache loaded n=%d devs=%d", blob->n, s_dev_n);
    heap_caps_free(blob);
}

static void apply_held_state(const char *device_id, int channel, const char *st)
{
    nl_dev_cache_t *d = ensure_dev(device_id);
    if (!d || !st || !st[0]) {
        return;
    }
    if (channel >= 0) {
        set_channel_state(d, channel, st);
        /* 单路继电器 snapshot 往往没有 channels，卡片回退读整机 power。 */
        if (channel == 0) {
            bool other = false;
            for (uint8_t i = 0; i < d->ch_n; i++) {
                if (d->ch_idx[i] != 0) {
                    other = true;
                    break;
                }
            }
            if (!other) {
                std::snprintf(d->power, sizeof(d->power), "%s", st);
            }
        }
        return;
    }
    std::snprintf(d->power, sizeof(d->power), "%s", st);
}

static const char *cached_state_at(const char *device_id, int channel)
{
    nl_dev_cache_t *d = nl_ov_find_dev(device_id);
    if (!d) {
        return nullptr;
    }
    if (channel >= 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            if (d->ch_idx[i] == channel) {
                return d->ch_st[i];
            }
        }
        if (channel == 0 && d->ch_n == 0 && d->power[0]) {
            return d->power;
        }
        return nullptr;
    }
    return d->power[0] ? d->power : nullptr;
}

static bool held_state_matches(const char *st, const char *want)
{
    if (!st || !want || state_is_unknown(st)) {
        return false;
    }
    return state_is_on(st) == state_is_on(want);
}

/** 快照（含局域网 sticky）写完后：已对上的口令放开，没对上的把卡片按口令保持。 */
static void cmd_pending_reconcile_after_snapshot(void)
{
    for (int i = 0; i < NL_CMD_PENDING_MAX; i++) {
        cmd_pending_t *p = &s_cmd_pending[i];
        if (!p->active || !p->hold || !p->want[0] || !p->device_id[0]) {
            continue;
        }
        const char *st = cached_state_at(p->device_id, p->channel);
        if (held_state_matches(st, p->want)) {
            cmd_pending_clear_slot(p);
            continue;
        }
        apply_held_state(p->device_id, p->channel, p->want);
    }
}

extern "C" void nodelio_panel_ui_snapshot_end(void)
{
    cmd_pending_reconcile_after_snapshot();
    lan_cache_save();
    s_have_device_snapshot = true;
    const uint32_t after = nodelio_panel_ui_dev_visual_sig();
    if (after == s_snap_vis_before) {
        ESP_LOGD(TAG, "snapshot end: visual unchanged — skip tile refresh");
        return;
    }
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

extern "C" void nodelio_panel_ui_hold_switch(const nl_widget_t *w, const char *want)
{
    if (!w || !want || !want[0]) {
        return;
    }
    cmd_pending_begin(w);
    for (auto &p : s_cmd_pending) {
        if (p.active && std::strcmp(p.widget_id, w->id) == 0) {
            p.hold = true;
            std::snprintf(p.want, sizeof(p.want), "%s", want);
            p.deadline_us = esp_timer_get_time() + 20000000LL;
            break;
        }
    }
    apply_held_state(w->bind.device_id, bind_channel_index(w), want);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

extern "C" void nodelio_panel_ui_hold_switch_fail(const char *widget_id)
{
    if (!widget_id || !widget_id[0]) {
        return;
    }
    cmd_pending_rollback_widget(widget_id);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

extern "C" void nodelio_panel_ui_apply_bind_state(const nl_bind_t *bind, const char *value)
{
    if (!bind || !value) {
        return;
    }
    nl_ov_apply_bind_state(bind, value);
    if (!s_home) {
        return;
    }
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    nl_ov_request_home_state_refresh();
}

extern "C" void nodelio_panel_ui_apply_reachability(const char *const *device_ids, const bool *online,
                                                    int n)
{
    nodelio_panel_ui_snapshot_begin();
    if (device_ids && online && n > 0) {
        for (int i = 0; i < n; i++) {
            nodelio_panel_ui_snapshot_add(device_ids[i], "", online[i], "");
        }
    }
    nodelio_panel_ui_snapshot_end();
}

extern "C" int nodelio_panel_ui_get_dev_count(void)
{
    return s_dev_n;
}

extern "C" const nl_dev_cache_t *nodelio_panel_ui_get_dev(int index)
{
    nl_ov_ensure_devs_cache();
    if (!s_devs || index < 0 || index >= s_dev_n) {
        return nullptr;
    }
    return &s_devs[index];
}
