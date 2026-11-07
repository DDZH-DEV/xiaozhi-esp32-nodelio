#include "overlay_priv.h"
#include "panel_jobs.h"
#include "ui_work_stats.h"

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

static bool s_home_structure_dirty;
static bool s_home_skills_dirty;
static bool s_home_state_dirty;
static bool s_home_cacheable;
static void build_home_view(void);
static void refresh_skill_widget(lv_obj_t* tile, nl_widget_t* widget);

/** fill_home 世代：异步相册图回写时丢弃过期结果 */
uint32_t s_home_gen;

bool nl_ov_home_is_visible_and_clean(void)
{
    return s_home && !s_home_structure_dirty && !lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN);
}

void nl_ov_home_mark_state_dirty(void)
{
    s_home_state_dirty = true;
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

/** DualLine 一律互斥双段；禁止整卡翻转（窄卡竖排双段）。 */
static bool dual_line_use_segments(const nl_widget_t *w)
{
    return widget_is_dual_line(w);
}

/**
 * DualLine 排向（与 APP dualVerticalSegs 对齐）：
 * - 竖排：瘦高 1×N，或高≥3（如 2×4）——上下两路
 * - 横排：扁横条、以及 2×2 等近方卡——左右两路
 */
static bool dual_line_vertical_segs(const nl_widget_t *w)
{
    if (!widget_is_dual_line(w)) {
        return false;
    }
    const int gw = w->gw > 0 ? w->gw : 1;
    const int gh = w->gh > 0 ? w->gh : 1;
    if (gw <= 1) {
        return true;
    }
    return gh >= 3;
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
        w->bind.kind == NL_BIND_HOME_SCENARIO || w->bind.kind == NL_BIND_ACTION_BUTTON) {
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
            /* 当前字库无 MODE_FAN；用旋转感字形占位 */
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
        if (strcmp(w->skin, "gauge") == 0) {
            return MATERIAL_SYMBOLS_BRIGHTNESS_6;
        }
        if (strcmp(w->skin, "tank") == 0) {
            /* 当前字库无 WATER_DROP */
            return MATERIAL_SYMBOLS_CLOUD_DOWNLOAD;
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
#if CONFIG_IDF_TARGET_ESP32S3
    /* S3 无 PPA：跳过磁贴整树逐帧颜色插值，单帧即时切换状态色 */
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TRANSITION, 0);
    return;
#endif
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
#if CONFIG_IDF_TARGET_ESP32S3
    /* S3 无 PPA：避免 transform_scale 触发 LVGL 9 分配 ARGB8888 离屏图层与软件双线性缩放 */
    return;
#endif
    const int32_t w = lv_obj_get_width(cap);
    const int32_t h = lv_obj_get_height(cap);
    lv_obj_set_style_transform_pivot_x(cap, w > 0 ? w / 2 : 16, 0);
    lv_obj_set_style_transform_pivot_y(cap, h > 0 ? h / 2 : 16, 0);
    lv_anim_delete(cap, anim_obj_scale);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cap);
    lv_anim_set_values(&a, 244, LV_SCALE_NONE);
    lv_anim_set_time(&a, UI_TILE_ICON_POP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_obj_scale);
    lv_anim_start(&a);
}

static uint32_t color_ambient_bg(uint32_t c)
{
    // Custom layout colors become a light tint so titles retain their contrast.
    uint32_t r = (((c >> 16) & 0xff) + ((UI_C_SURFACE >> 16) & 0xff) * 9) / 10;
    uint32_t g = (((c >> 8) & 0xff) + ((UI_C_SURFACE >> 8) & 0xff) * 9) / 10;
    uint32_t b = ((c & 0xff) + (UI_C_SURFACE & 0xff) * 9) / 10;
    return (r << 16) | (g << 8) | b;
}

static uint32_t widget_fill_for_state(const nl_widget_t *w, tile_kind_t kind, bool online, bool on,
                                      bool unknown)
{
    if (!online || unknown) {
        return UI_C_OFFLINE;
    }
    /* DualLine：整卡中性石板，选中态只体现在支路胶囊上（勿整卡变「开」） */
    if (widget_is_dual_line(w)) {
        return kind_fill_off(kind);
    }
    if (!on) {
        return kind_fill_off(kind);
    }
    /* 开态：layout 自定义浅色底，否则使用语义浅色底。 */
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
    /* 终端卡：通道路高亮描边 + 底；断道路暗底，贴近 APP 双端卡片 */
    lv_obj_set_style_bg_color(seg, active ? active_c : UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(seg, online ? (active ? LV_OPA_COVER : LV_OPA_70) : LV_OPA_40, 0);
    lv_obj_set_style_border_width(seg, active ? 2 : 1, 0);
    lv_obj_set_style_border_color(seg, active ? active_c : UI_BORDER, 0);
    lv_obj_set_style_border_opa(seg, active ? LV_OPA_80 : LV_OPA_50, 0);
    ui_set_enabled(seg, online);
    const uint32_t n = lv_obj_get_child_count(seg);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(seg, i);
        if (!ch || !lv_obj_check_type(ch, &lv_label_class)) {
            continue;
        }
        tile_apply_trans(ch);
        intptr_t role = (intptr_t)lv_obj_get_user_data(ch);
        if (role == TILE_CHILD_DL_SUB) {
            lv_obj_set_style_text_color(
                ch, active ? ui_color_on(active_c) : (online ? UI_TEXT_TIP : UI_DISABLED_FG), 0);
            lv_obj_set_style_text_opa(ch, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(
                ch, active ? ui_color_on(active_c) : (online ? UI_TEXT_SUB : UI_DISABLED_FG), 0);
        }
    }
}

struct tile_visual_state_t {
    bool online, unknown, on;
    int duty, countdown;
    const lv_font_t* font;
    bool operator==(const tile_visual_state_t& rhs) const {
        return online == rhs.online && unknown == rhs.unknown && on == rhs.on && duty == rhs.duty &&
               countdown == rhs.countdown && font == rhs.font;
    }
};

// At most two cached pages of 32 tiles. Overflow tiles still render normally.
// Delete callbacks invalidate pointer keys before LVGL can reuse their addresses.
static bool tile_visual_unchanged(lv_obj_t* tile, const tile_visual_state_t& state) {
    struct entry {
        lv_obj_t* object;
        tile_visual_state_t state;
    };
    static entry entries[64]{};
    entry* empty = nullptr;
    for (auto& e : entries) {
        if (e.object == tile) {
            if (e.state == state)
                return true;
            e.state = state;
            return false;
        }
        if (!e.object && !empty)
            empty = &e;
    }
    if (empty) {
        empty->object = tile;
        empty->state = state;
        lv_obj_add_event_cb(
            tile,
            [](lv_event_t* event) {
                auto* e = static_cast<entry*>(lv_event_get_user_data(event));
                e->object = nullptr;
            },
            LV_EVENT_DELETE, empty);
    }
    return false;
}

void nl_ov_style_tile_visual(lv_obj_t *tile, const nl_widget_t *w)
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
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.kind != NL_BIND_ACTION_BUTTON &&
        w->bind.device_id[0]) {
        online = nodelio_panel_ui_device_online(w->bind.device_id);
    }
    const bool unknown = nl_ov_widget_state_unknown(w, kind);
    /* 点按已乐观写缓存；失败/超时回滚。不转圈等真态 */
    bool on = !unknown && nl_ov_widget_is_on(w, kind);
    const tile_visual_state_t state{
        online, unknown, on, nl_ov_widget_pwm_duty(w), nl_ov_widget_countdown(w), nl_ov_ui_font()};
    if (tile_visual_unchanged(tile, state))
        return;
    const bool on_rising = tile_vis_on_rising(w->id, on && online);
    uint32_t fill = widget_fill_for_state(w, kind, online, on, unknown);
    /* 开态图标使用高饱和品牌/语义亮色，关态暗白，离线灰暗 */
    lv_color_t active_c = UI_SUCCESS;
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
    tile_apply_trans(tile);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
    /* 首页磁贴微光边框：开态微光外描边，关态暗质感描边 */
    if (on && online) {
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, active_c, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_80, 0);
    } else {
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
        lv_obj_set_style_border_opa(tile, LV_OPA_80, 0);
    }
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
        ui_set_enabled(tile, online || kind == TILE_SCENE || w->bind.kind == NL_BIND_HOME_SCENARIO ||
                                   w->bind.kind == NL_BIND_ACTION_BUTTON);
        if (online) {
            lv_obj_set_style_opa(tile, LV_OPA_COVER, 0);
        }
    }

    const bool dim = !online || unknown;
    const bool dl = widget_is_dual_line(w);
    /* DualLine 整卡中性：标题/图标不跟 ON 变白 */
    lv_color_t icon_c = dim ? UI_DISABLED_FG : (dl ? UI_TEXT_DIM : (on ? active_c : UI_TEXT_DIM));
    lv_color_t title_c = dim ? UI_DISABLED_FG : UI_TEXT;
    lv_color_t hint_c = dim ? UI_DISABLED_FG : UI_TEXT_MUTED;
    lv_color_t badge_bg =
        dim ? UI_TILE_BADGE_OFFLINE : (on ? UI_TILE_BADGE_ON : UI_TILE_BADGE_OFF);
    if (!dim && on && !dl) {
        badge_bg = active_c;
    }
    if (!dim && dl) {
        badge_bg = active_c; /* 角标标出当前支路色 */
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
            lv_obj_set_style_bg_color(ch, on && online ? active_c : UI_CAPSULE_OFF, 0);
            lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(ch, UI_RADIUS_SM, 0);
            lv_obj_set_style_border_width(ch, 0, 0);
            if (lv_obj_get_child_count(ch) > 0) {
                lv_obj_t *icon_lbl = lv_obj_get_child(ch, 0);
                tile_apply_trans(icon_lbl);
                lv_label_set_text(icon_lbl, kind_symbol(w, kind, on && online));
                lv_obj_set_style_text_font(icon_lbl, nl_ov_tile_icon_font(is_row), 0);
                lv_obj_set_style_text_color(icon_lbl, on && online ? ui_color_on(active_c) : icon_c,
                                            0);
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
                        nl_ov_format_countdown(cd, hb, sizeof(hb));
                        lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                        lv_label_set_text(ch, hb);
                        nl_ov_set_font_compact(ch);
                        nl_ov_style_hint_pill(ch, true);
                        lv_obj_set_style_text_color(ch, UI_WARN, 0);
                        lv_obj_set_style_bg_color(ch, UI_SURFACE_PRESSED, 0);
                        lv_obj_set_style_bg_opa(ch, LV_OPA_70, 0);
                        lv_obj_align(ch, LV_ALIGN_TOP_RIGHT, -2, 2);
                    } else if (!online) {
                        lv_obj_clear_flag(ch, LV_OBJ_FLAG_HIDDEN);
                        lv_label_set_text(ch, "离");
                        nl_ov_set_font_compact(ch);
                        nl_ov_style_hint_pill(ch, false);
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
                    nl_ov_format_countdown(cd, hb, sizeof(hb));
                    lv_label_set_text(ch, hb);
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, true);
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
                    std::snprintf(hb, sizeof(hb), "%s%d%%", prefix, nl_ov_widget_pwm_duty(w));
                    lv_label_set_text(ch, hb);
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, hint_c, 0);
                    lv_obj_set_style_bg_color(ch, UI_FIELD, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
                } else if (kind == TILE_SCENE) {
                    lv_label_set_text(ch, "情景");
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, UI_TEXT_MUTED, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_TRANSP, 0);
                } else if (w && strcmp(w->skin, "momentary") == 0) {
                    /* 点动角标 */
                    lv_label_set_text(ch, (!online) ? "离" : "点动");
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, on && online ? ui_color_on(badge_bg) : hint_c,
                                                0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
                } else if (widget_is_dual_line(w)) {
                    /* 双段卡通常藏角标；若露出则显示当前支路短名 */
                    const char *act = on ? dl_on : dl_off;
                    lv_label_set_text(ch, (!online) ? "离" : (act ? act : (on ? "NO" : "NC")));
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, online ? ui_color_on(badge_bg) : hint_c, 0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
                } else {
                    /* 开/关/离：右上角/右侧小角标，不占主列 */
                    lv_label_set_text(ch, (!online) ? "离" : (on ? "开" : "关"));
                    nl_ov_set_font_compact(ch);
                    nl_ov_style_hint_pill(ch, false);
                    lv_obj_set_style_text_color(ch, on && online ? ui_color_on(badge_bg) : hint_c,
                                                0);
                    lv_obj_set_style_bg_color(ch, badge_bg, 0);
                    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
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
            int duty = nl_ov_widget_pwm_duty(w);
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

void nl_ov_home_flush_pending_state(void)
{
    if (!s_home_state_dirty) {
        return;
    }
    nl_ov_refresh_home_tiles();
}

void nl_ov_refresh_home_tiles(void)
{
    if (!s_home) {
        return;
    }
    if (s_home_structure_dirty || lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN)) {
        s_home_state_dirty = true;
        return;
    }
    /* 设置详情盖住首页时不要跟着重绘磁贴，否则滚动和快照抢同一把 LVGL 锁。 */
    if (s_settings_detail && !lv_obj_has_flag(s_settings_detail, LV_OBJ_FLAG_HIDDEN)) {
        s_home_state_dirty = true;
        return;
    }
    s_home_state_dirty = false;
    /* 批量刷态关掉色过渡，否则 bootstrap 一次改十几颗磁贴像整屏闪 */
    s_tile_bulk_refresh = true;
    uint32_t cnt = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(s_home, i);
        auto *w = static_cast<nl_widget_t *>(lv_obj_get_user_data(child));
        if (!w) {
            continue;
        }
        nl_ov_style_tile_visual(child, w);
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

void nl_ov_place_grid_obj(lv_obj_t *obj, const nl_widget_t *w, int cell_w, int cell_h)
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
    if (nl_ov_widget_is_cmd_pending(w)) {
        return;
    }
    intptr_t role = (intptr_t)lv_obj_get_user_data(seg);
    const bool want_on = (role == TILE_CHILD_DL_ON);
    const char *want = want_on ? "ON" : "OFF";
    tile_kind_t kind = tile_kind_of(w);
    if (nl_ov_widget_is_on(w, kind) == want_on) {
        return; /* 已在目标支路 */
    }
    if (w->bind.device_id[0] && !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    const char *off_n = nullptr;
    const char *on_n = nullptr;
    dual_branch_names(w, &off_n, &on_n);
    const char *branch = want_on ? on_n : off_n;
    char toast[40];
    std::snprintf(toast, sizeof(toast), "已切到 %s",
                  branch && branch[0] ? branch : (want_on ? "NO" : "NC"));
    nl_ov_toast(toast);
    if (seg) {
        tile_icon_pop(seg);
    }
    nl_ov_home_switch_commit(w, want);
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
        w->bind.kind == NL_BIND_HOME_SCENARIO || w->bind.kind == NL_BIND_ACTION_BUTTON) {
        nl_ov_widget_cmd(w, w->bind.name);
        /* 轻反馈：边框闪 + 短弹 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
            tile_icon_pop(tgt);
        }
        return;
    }
    if (w && strcmp(w->skin, "momentary") == 0) {
        if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.kind != NL_BIND_ACTION_BUTTON &&
            w->bind.device_id[0] && !nodelio_panel_ui_device_online(w->bind.device_id)) {
            nl_ov_toast("设备未在线");
            return;
        }
        /* 点动：下发触发脉冲，边框 + 弹跳反馈，不把本地缓存翻成开态 */
        if (tgt) {
            lv_obj_set_style_border_width(tgt, 2, 0);
            lv_obj_set_style_border_color(tgt, UI_ACCENT_SOFT, 0);
            tile_icon_pop(tgt);
        }
        nl_ov_dispatch_async_cmd(w, "TOGGLE", false);
        return;
    }
    if (nl_ov_widget_is_cmd_pending(w)) {
        return;
    }
    tile_kind_t kind = tile_kind_of(w);
    const char *want = nl_ov_widget_is_on(w, kind) ? "OFF" : "ON";
    if (w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.kind != NL_BIND_ACTION_BUTTON &&
        w->bind.device_id[0] && !nodelio_panel_ui_device_online(w->bind.device_id)) {
        nl_ov_toast("设备未在线");
        return;
    }
    if (tgt) {
        tile_icon_pop(tgt);
    }
    nl_ov_home_switch_commit(w, want);
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
    nl_ov_set_widget_cache_state(w, val);
    nl_ov_refresh_home_tiles();
    nl_ov_widget_cmd(w, val);
}

lv_obj_t *nl_ov_make_tile_shell(const nl_widget_t *w, tile_kind_t kind, int cell_w, int cell_h)
{
    uint32_t fill = widget_fill_for_state(w, kind, true, false, false);
    lv_obj_t *tile = lv_obj_create(s_home);
    lv_obj_set_user_data(tile, (void *)w);
    lv_obj_set_style_bg_color(tile, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_80, 0);
    lv_obj_set_style_radius(tile, UI_RADIUS_MD, 0);
    lv_obj_set_style_pad_all(tile, UI_PAD_SM, 0);
    ui_style_card(tile);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    nl_ov_place_grid_obj(tile, w, cell_w, cell_h);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tile, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color_pressed(fill)), LV_STATE_PRESSED);
#if !CONFIG_IDF_TARGET_ESP32S3
    lv_obj_set_style_translate_y(tile, 1, LV_STATE_PRESSED);
    ensure_tile_trans();
    lv_obj_set_style_transition(tile, &s_tile_trans, 0);
#endif
    return tile;
}

/** 支路终端卡：主名（厨房）+ 副标（NC·常闭），对齐 APP DualLine 两端盒子。 */
static lv_obj_t *make_dual_seg(lv_obj_t *parent, nl_widget_t *w, intptr_t role, const char *text,
                               bool compact, bool vertical)
{
    const bool is_on_branch = (role == TILE_CHILD_DL_ON);
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_set_user_data(seg, (void *)role);
    lv_obj_set_flex_grow(seg, 1);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(seg, compact ? 0 : 2, 0);
    if (vertical) {
        lv_obj_set_width(seg, lv_pct(100));
        lv_obj_set_height(seg, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(seg, compact ? 32 : 44, 0);
    } else {
        lv_obj_set_height(seg, compact ? 36 : 48);
    }
    lv_obj_set_style_radius(seg, UI_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(seg, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(seg, 1, 0);
    lv_obj_set_style_border_color(seg, UI_BORDER, 0);
    lv_obj_set_style_border_opa(seg, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(seg, 6, 0);
    lv_obj_set_style_pad_right(seg, 6, 0);
    lv_obj_set_style_pad_top(seg, compact ? 4 : 6, 0);
    lv_obj_set_style_pad_bottom(seg, compact ? 4 : 6, 0);
    lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(seg, UI_EXT_CLICK);
    lv_obj_set_style_bg_color(seg, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
#if !CONFIG_IDF_TARGET_ESP32S3
    lv_obj_set_style_translate_y(seg, 1, LV_STATE_PRESSED);
#endif
    lv_obj_add_event_cb(seg, on_dual_seg, LV_EVENT_CLICKED, w);

    char short_n[24];
    short_label(text, short_n, sizeof(short_n), compact ? 5 : 6);
    lv_obj_t *lb = lv_label_create(seg);
    lv_label_set_text(lb, short_n[0] ? short_n : text);
    nl_ov_set_font(lb);
    lv_obj_set_style_text_color(lb, UI_TEXT_SUB, 0);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lb, lv_pct(100));

    /* 副标：NC·常闭 / NO·常开 — 说明这是两路选一，不是开/关按钮 */
    lv_obj_t *sub = lv_label_create(seg);
    lv_obj_set_user_data(sub, (void *)(intptr_t)TILE_CHILD_DL_SUB);
    lv_label_set_text(sub, is_on_branch ? "NO · 常开" : "NC · 常闭");
    nl_ov_set_font(sub);
    lv_obj_set_style_text_color(sub, UI_TEXT_TIP, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(sub, lv_pct(100));
    if (compact && !vertical) {
        /* 扁横条挤：副标可藏，主名仍是两路名 */
        lv_obj_add_flag(sub, LV_OBJ_FLAG_HIDDEN);
    }
    return seg;
}

static void make_dual_div(lv_obj_t *parent, bool vertical, bool compact)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_remove_style_all(div);
    lv_obj_set_user_data(div, (void *)(intptr_t)TILE_CHILD_DL_DIV);
    if (vertical) {
        lv_obj_set_size(div, lv_pct(56), 1);
    } else {
        lv_obj_set_size(div, 1, compact ? 20 : 28);
    }
    lv_obj_set_style_bg_color(div, UI_BORDER, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_60, 0);
    lv_obj_clear_flag(div, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
}

static void add_tile_labels(lv_obj_t *tile, nl_widget_t *w, tile_kind_t kind, const char *title)
{
    bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
    bool is_row = (w && w->gw >= 2 && w->gh <= 1);
    bool is_large = (w && (w->gw >= 2 && w->gh >= 2));
    bool is_col_slim = (w && w->gw <= 1 && w->gh >= 2);
    bool dl_segs = dual_line_use_segments(w);
    bool dl_vert = dual_line_vertical_segs(w);

    char short_t[48];
    short_label(title, short_t, sizeof(short_t), title_max_runes(w));

    if (dl_segs && dl_vert) {
        /* DualLine：上下两路终端卡（厨房 / 热水器），贴近 APP */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, is_1x1 ? 4 : 8, 0);
        lv_obj_set_style_pad_row(tile, is_1x1 ? 4 : 6, 0);
    } else if (dl_segs && is_row) {
        /* DualLine 扁横条：左设备名 + 右两路名 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(tile, 8, 0);
        lv_obj_set_style_pad_left(tile, 10, 0);
        lv_obj_set_style_pad_right(tile, 8, 0);
        lv_obj_set_style_pad_top(tile, 4, 0);
        lv_obj_set_style_pad_bottom(tile, 4, 0);
    } else if (dl_segs) {
        /* DualLine 2×2 等：上标题 + 下左右两路 */
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_style_pad_row(tile, 6, 0);
    } else if (is_1x1) {
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(tile, 4, 0);
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

    /* 1. 图标：DualLine 一律省略，把空间留给支路名 */
    lv_coord_t cap_size = is_row ? 28 : (is_large ? 46 : (is_col_slim ? 36 : 42));
    if (dl_segs) {
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
    if (dl_segs && dl_vert) {
        /* 竖排两路已自带名称；设备标题仅在较高卡保留一行 */
        if (w && w->gh >= 3) {
            lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lb, lv_pct(94));
            lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
        } else {
            lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (dl_segs && is_row) {
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_width(lb, 72);
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    } else if (dl_segs) {
        /* 2×2 横排：顶部留短标题 */
        lv_obj_clear_flag(lb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lb, lv_pct(94));
        lv_obj_set_style_text_color(lb, UI_TEXT_MUTED, 0);
    } else if (is_1x1) {
        lv_obj_add_flag(lb, LV_OBJ_FLAG_HIDDEN);
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

    /* 2b. DualLine：两路终端卡（名称+NC/NO），点选切换当前通路 */
    if (dl_segs) {
        const char *off_n = nullptr;
        const char *on_n = nullptr;
        dual_branch_names(w, &off_n, &on_n);
        lv_obj_t *row = lv_obj_create(tile);
        lv_obj_remove_style_all(row);
        lv_obj_set_user_data(row, (void *)(intptr_t)TILE_CHILD_DL_ROW);
        lv_obj_set_width(row, lv_pct(is_row ? 70 : 100));
        if (dl_vert) {
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_height(row, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(row, 6, 0);
        } else {
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_flex_grow(row, 1);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 6, 0);
        }
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        const bool compact = is_row || is_1x1;
        make_dual_seg(row, w, TILE_CHILD_DL_OFF, off_n, compact, dl_vert);
        make_dual_div(row, dl_vert, compact);
        make_dual_seg(row, w, TILE_CHILD_DL_ON, on_n, compact, dl_vert);
    }

    /* 3. 药丸浮动角标（DualLine 藏起，选中态看双段高亮） */
    lv_obj_t *hint = lv_label_create(tile);
    lv_obj_set_user_data(hint, (void *)(intptr_t)TILE_CHILD_HINT);
    lv_obj_add_flag(hint, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
    lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(hint, UI_RADIUS_XS, 0);
    lv_obj_set_style_bg_color(hint, UI_TILE_BADGE_OFF, 0);
    lv_obj_set_style_bg_opa(hint, LV_OPA_70, 0);
    nl_ov_style_hint_pill(hint, false);
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
        lv_label_set_text(hint, "双路");
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
static void home_collect_user_pages(void);
static void home_resolve_current(void);

struct home_cached_view_t {
    lv_obj_t* root = nullptr;
    char page_id[NL_LAYOUT_STR]{};
    int grid_x = 0, grid_y = 0;
};
static home_cached_view_t s_cached_home;

static void drop_home_cache(void) {
    if (s_cached_home.root) {
        lv_obj_delete(s_cached_home.root);
        s_cached_home = {};
    }
}

void nl_ov_home_refresh_skill(const char* skill_id) {
    if (!s_home) {
        return;
    }
    if (s_home_structure_dirty || lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN)) {
        s_home_skills_dirty = true;
        return;
    }
    NlUiWorkScope work("home.skill");
    const uint32_t count = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* tile = lv_obj_get_child(s_home, i);
        auto* widget = static_cast<nl_widget_t*>(lv_obj_get_user_data(tile));
        if (widget && widget->type == NL_WIDGET_SKILL_WIDGET &&
            (!skill_id || strcmp(widget->skill_id, skill_id) == 0)) {
            refresh_skill_widget(tile, widget);
        }
    }
    if (!skill_id) {
        s_home_skills_dirty = false;
    }
}

void nl_ov_home_prepare(void) {
    if (!s_home || lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (s_home_structure_dirty) {
        build_home_view();
        return;
    }
    if (s_home_state_dirty) {
        nl_ov_refresh_home_tiles();
    }
    if (s_home_skills_dirty) {
        nl_ov_home_refresh_skill(nullptr);
    }
    // Page entry may use a retained tree, so it must still check cache expiry.
    // This does not add periodic fetching; completion only refreshes its widget.
    const uint32_t count = lv_obj_get_child_count(s_home);
    for (uint32_t i = 0; i < count; ++i) {
        auto* widget = static_cast<nl_widget_t*>(lv_obj_get_user_data(lv_obj_get_child(s_home, i)));
        if (widget && widget->type == NL_WIDGET_SKILL_WIDGET && widget->skill_id[0]) {
            nl_ov_skill_widget_prefetch(widget->skill_id,
                                        widget->default_invoke[0] ? widget->default_invoke : "{}",
                                        widget->refresh_sec > 0 ? widget->refresh_sec : 600);
        }
    }
}

void nl_ov_fill_home(void) {
    drop_home_cache();
    // Navigation queries must see the new layout even if its object tree is hidden.
    home_collect_user_pages();
    home_resolve_current();
    s_home_structure_dirty = true;
    s_home_cacheable = false;
    // Invalidates outstanding photo results even while the page stays hidden.
    ++s_home_gen;
    if (s_home && (!s_ready || !lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN))) {
        build_home_view();
    }
}

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
    if (strcmp(w->skill_id, "builtin.daily") == 0 ||
        strcmp(w->skill_id, "weather.today") == 0 ||
        strstr(w->skill_id, "weather") != nullptr ||
        strstr(w->title, "天气") != nullptr) {
        (void)nl_ov_apps_open_by_query("日历与天气");
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
    lv_obj_t *tile = nl_ov_make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
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
    if (strcmp(w->skill_id, "builtin.daily") == 0 ||
        strcmp(w->skill_id, "weather.today") == 0 ||
        strstr(w->skill_id, "weather") != nullptr ||
        strstr(w->title, "天气") != nullptr) {
        (void)nl_ov_apps_open_by_query("日历与天气");
        return;
    }
    const char *body = w->default_invoke[0] ? w->default_invoke : "{}";
    nl_ov_invoke_skill_async(w->skill_id, body);
}

lv_obj_t *nl_ov_widget_icon_pod(lv_obj_t *parent, const char *glyph, lv_color_t bg, int sz)
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

static void fill_skill_widget(nl_widget_t *w, int cell_w, int cell_h)
{
    if (!w || !w->skill_id[0]) {
        return;
    }
    lv_obj_t *tile = nl_ov_make_tile_shell(w, TILE_SCENE, cell_w, cell_h);
    lv_obj_set_style_bg_color(tile, UI_TILE_OFF, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(tile, on_skill_widget_click, LV_EVENT_CLICKED, w);

    refresh_skill_widget(tile, w);
    const char* invoke = w->default_invoke[0] ? w->default_invoke : "{}";
    nl_ov_skill_widget_prefetch(w->skill_id, invoke, w->refresh_sec > 0 ? w->refresh_sec : 600);
}

static void refresh_skill_widget(lv_obj_t* tile, nl_widget_t* w) {
    const char* fallback = w->title[0] ? w->title : w->skill_id;
    lv_obj_clean(tile);
    nl_widget_preview pv{};
    const bool have = nl_ov_skill_widget_preview(w->skill_id, &pv);
    const bool one_row = w->gh <= 1;

    const char *widget_glyph = MATERIAL_SYMBOLS_BRIGHTNESS_6;
    if (w->icon[0]) {
        const char *named = material_symbols_get_utf8(w->icon);
        if (named != NULL) {
            widget_glyph = named;
        }
    } else if (strstr(w->skill_id, "weather") != nullptr || strstr(fallback, "天气") != nullptr) {
        widget_glyph = MATERIAL_SYMBOLS_BRIGHTNESS_6;
    } else {
        widget_glyph = MATERIAL_SYMBOLS_CHAT_BUBBLE;
    }

    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(tile, 12, 0);
    lv_obj_set_style_pad_right(tile, 12, 0);
    lv_obj_set_style_pad_column(tile, 10, 0);

    const int pod_sz = one_row ? 30 : 38;
    nl_ov_widget_icon_pod(tile, widget_glyph, UI_ACCENT, pod_sz);

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *ht = lv_label_create(col);
    nl_ov_set_font(ht);
    lv_label_set_text(ht, fallback[0] ? fallback : (pv.head[0] ? pv.head : "日历与天气"));
    lv_obj_set_style_text_color(ht, UI_TEXT, 0);
    lv_label_set_long_mode(ht, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ht, lv_pct(100));

    lv_obj_t *sub = lv_label_create(col);
    nl_ov_set_font(sub);
    const char *sub_txt = (have && pv.title[0][0]) ? pv.title[0] : "点按打开应用";
    lv_label_set_text(sub, sub_txt);
    lv_obj_set_style_text_color(sub, UI_TEXT_MUTED, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(sub, lv_pct(100));
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
    NlUiWorkScope work("home.switch");
    const char* next_id = s_layout->pages[s_home_user_idxs[next]].id;
    const bool hit = s_cached_home.root && strcmp(s_cached_home.page_id, next_id) == 0;
    home_cached_view_t target;
    if (hit) {
        target = s_cached_home;
        s_cached_home = {};
    }
    lv_obj_t* old = s_home;
    const bool hidden = lv_obj_has_flag(old, LV_OBJ_FLAG_HIDDEN);
    if (s_home_cacheable && !s_home_structure_dirty) {
        drop_home_cache();
        s_cached_home.root = old;
        snprintf(s_cached_home.page_id, sizeof(s_cached_home.page_id), "%s", s_home_page_id);
        s_cached_home.grid_x = s_grid_ox;
        s_cached_home.grid_y = s_grid_oy;
        if (!hit) {
            target.root = lv_obj_create(s_body);
            if (target.root) {
                lv_obj_remove_style_all(target.root);
                lv_obj_set_size(target.root, lv_obj_get_width(old), lv_obj_get_height(old));
            } else {
                // Allocation pressure: rebuild the existing page, retaining no spare.
                s_cached_home = {};
                target.root = old;
            }
        }
        lv_obj_add_flag(old, LV_OBJ_FLAG_HIDDEN);
    } else if (hit) {
        lv_obj_delete(old);
    } else {
        target.root = old;
    }
    s_home = target.root;
    s_home_user_cur = next;
    home_remember_current_id();
    ++s_home_gen;
    nl_ov_photo_reset_rt();
    if (!hidden) {
        lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
    }
    if (hit) {
        s_grid_ox = target.grid_x;
        s_grid_oy = target.grid_y;
        s_home_cacheable = true;
        s_home_state_dirty = true;
        s_home_skills_dirty = true;
        nl_ov_home_prepare();
        ESP_LOGD(TAG, "home cache hit %s", next_id);
    } else {
        build_home_view();
    }
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

static lv_obj_t *s_home_dots;

void nl_ov_home_dots_set_visible(bool on)
{
    if (!s_home_dots) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_home_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_home_dots);
    } else {
        lv_obj_add_flag(s_home_dots, LV_OBJ_FLAG_HIDDEN);
    }
}

static void append_home_page_dots(lv_obj_t *parent, int strip_h)
{
    s_home_dots = nullptr;
    if (!parent || s_home_user_n <= 1) {
        return;
    }
    lv_obj_t *dots = lv_obj_create(parent);
    s_home_dots = dots;
    lv_obj_remove_style_all(dots);
    lv_obj_add_flag(dots, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(dots, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(dots, lv_pct(100), k_home_dots_h);
    lv_obj_set_style_pad_column(dots, 7, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(dots, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    /* 浮在分页底栏之上；随菜单一起自动隐藏 */
    const int yoff = strip_h > 0 ? -(strip_h + 2) : -(NL_UI_TAB_H + 16);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_MID, 0, yoff);
    nl_ov_nav_chrome_sync();

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

/** 宽屏打开应用后首页被收成左栏，启动台改成侧栏排版。 */
static bool launcher_is_side_column(void)
{
    const auto *p = nodelio_display_profile();
    if (!p || !p->nav_width || !s_home) {
        return false;
    }
    const int full = (int)p->width - (int)p->nav_width;
    const int w = (int)lv_obj_get_width(s_home);
    return w > 0 && w < full - 40;
}

static lv_obj_t *make_launcher_card(lv_obj_t *parent, const char *icon, const char *title,
                                    const char *subtitle, lv_event_cb_t on_click, bool side,
                                    bool half)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    if (half) {
        /* 基宽要小，剩下的宽度才由 flex 平分；按内容算会被长副标题撑满一行 */
        lv_obj_set_width(card, 10);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_min_width(card, 0, 0);
        lv_obj_set_height(card, 88);
    } else {
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, side ? 72 : 88);
    }
    lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(card, ui_palette_is_dark() ? 1 : 0, 0);
    lv_obj_set_style_border_color(card, UI_TILE_OFF_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_80, 0);
    ui_style_card(card);
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
    lv_obj_set_size(pod, side ? 40 : 48, side ? 40 : 48);
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
    if (side) {
        lv_obj_set_height(sub, nl_ov_ui_font_line_h());
    }
    return card;
}

/** 空控家启动台：已钉 home_tile + 全部应用入口 + 进入对话（不拉市场） */
static void fill_home_launcher(void)
{
    const bool side = launcher_is_side_column();
    const int pins = nl_ov_home_skill_pinned_count();
    lv_obj_set_layout(s_home, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_home, LV_FLEX_FLOW_COLUMN);
    /* 侧栏条目少时整组垂直居中，避免两张卡贴在顶上、下面一大块空 */
    lv_obj_set_flex_align(s_home,
                          (side && pins <= 2) ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_home, side ? 16 : 20, 0);
    lv_obj_set_style_pad_ver(s_home, side ? 16 : 12, 0);
    lv_obj_set_style_pad_row(s_home, side ? 12 : 10, 0);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_home, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_home, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLL_ELASTIC);

    lv_obj_t *hdr = lv_obj_create(s_home);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hdr, side ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hdr, side ? 4 : 6, 0);
    lv_obj_set_style_pad_bottom(hdr, side ? 6 : 0, 0);
    lv_obj_clear_flag(hdr, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    lv_obj_t *title = lv_label_create(hdr);
    nl_ov_set_font(title);
    lv_label_set_text(title, "启动台");
    /* 标题落在 36px 黑底顶栏里，浅色字才读得清 */
    lv_obj_set_style_text_color(title, UI_ON_TILE, 0);

    lv_obj_t *hint = lv_label_create(hdr);
    nl_ov_set_font(hint);
    /* P3b：多屏全空引导去 APP；有钉选则提示底入口 */
    if (s_home_user_n > 1) {
        lv_label_set_text(hint, "多屏均未编排 · 请用手机 APP「编辑大屏布局」");
    } else if (pins > 0) {
        lv_label_set_text(hint, side ? "未配置控家 · 已钉应用" : "未配置控家 · 下方为已钉应用");
    } else {
        lv_label_set_text(hint, side ? "未配置控家 · 先用应用或对话"
                                     : "未配置控家 · 可先用应用与对话，或去 APP 编排");
    }
    lv_obj_set_style_text_color(hint, UI_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(hint, side ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(hint, side ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    if (side) {
        lv_obj_set_height(hint, nl_ov_ui_font_line_h());
    }

    /* C3：已钉技能磁贴（技能通道，非 panel/command） */
    nl_ov_home_skills_append_launcher(s_home);

    lv_obj_t *card_parent = s_home;
    if (!side) {
        card_parent = lv_obj_create(s_home);
        lv_obj_remove_style_all(card_parent);
        lv_obj_set_width(card_parent, lv_pct(100));
        lv_obj_set_height(card_parent, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card_parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card_parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(card_parent, 12, 0);
        lv_obj_clear_flag(card_parent, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    }
    make_launcher_card(
        card_parent, MATERIAL_SYMBOLS_EXPLORE, "全部应用",
        side ? "技能列表 · 可钉选" : "打开技能列表 · 手机可钉选或挂画布",
        [](lv_event_t *) { nl_ov_show_page(NL_TAB_APPS); }, side, !side);

    make_launcher_card(card_parent, MATERIAL_SYMBOLS_MIC, "进入对话",
                       side ? "唤醒助手" : "唤醒助手 · 语音控家或提问",
                       [](lv_event_t *) { nodelio_overlay_show_chat(); }, side, !side);
    if (s_have_layout && s_layout && s_layout->layout_version > 0) {
        nodelio_panel_ui_mark_home_filled(s_layout->layout_version);
    }
}

static void build_home_view(void) {
    if (!s_home) {
        return;
    }
    NlUiWorkScope work("home.build");
    s_home_structure_dirty = false;
    s_home_skills_dirty = false;
    s_home_state_dirty = false;
    s_home_cacheable = false;
    s_home_gen++;
    nl_ov_photo_reset_rt();
    nl_ov_photo_cache_prune_layout();
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
        ui_style_card(card);
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

    int cols = s_layout->grid_cols > 0 ? s_layout->grid_cols : nodelio_display_profile()->grid_cols;
    int rows = s_layout->grid_rows > 0 ? s_layout->grid_rows : nodelio_display_profile()->grid_rows;
    lv_obj_update_layout(s_home);
    lv_coord_t area_w = lv_obj_get_content_width(s_home);
    lv_coord_t area_h = lv_obj_get_content_height(s_home);
    if (area_w < 40) {
        area_w = nodelio_display_profile()->width - nodelio_display_profile()->nav_width -
                 2 * NL_UI_HOME_PAD_H;
    }
    if (area_h < 40) {
        area_h = nodelio_display_profile()->height - 2 * NL_UI_HOME_PAD_V;
    }
    /* C4：为底部应用条预留高度，控家网格与 home_tile 同屏不互踩 */
    const int strip_h = nl_ov_home_skill_strip_h();
    if (strip_h > 0 && area_h > strip_h + 80) {
        area_h -= strip_h;
    }
    /* 多屏指示点 */
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
                nl_ov_fill_photo_frame(w, cell_w, cell_h);
                continue;
            }
            if (!nodelio_controls_enabled()) {
                continue;
            }
            tile_kind_t kind = tile_kind_of(w);
            if (kind == TILE_PWM) {
                bool is_1x1 = (w && w->gw <= 1 && w->gh <= 1);
                bool is_row = (w && w->gw >= 2 && w->gh <= 1);
                lv_obj_t *row = nl_ov_make_tile_shell(w, kind, cell_w, cell_h);
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
                nl_ov_style_hint_pill(hint, false);
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
                nl_ov_style_tile_visual(row, w);
                continue;
            }
            lv_obj_t *tile = nl_ov_make_tile_shell(w, kind, cell_w, cell_h);
            {
                const bool dl = widget_is_dual_line(w);
                ESP_LOGD(TAG, "draw switch '%s' skin='%s' dual=%d segs=%d vert=%d %dx%d", title,
                         w->skin[0] ? w->skin : "(empty)", dl ? 1 : 0,
                         dual_line_use_segments(w) ? 1 : 0, dual_line_vertical_segs(w) ? 1 : 0,
                         (int)w->gw, (int)w->gh);
            }
            if (dual_line_use_segments(w)) {
                /* 双段按钮各自响应；整卡不再翻转，避免误触 */
                lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_add_event_cb(tile, on_btn, LV_EVENT_CLICKED, w);
            }
            add_tile_labels(tile, w, kind, title);
            nl_ov_style_tile_visual(tile, w);
        }
    }

    s_home_cacheable = count_home_content_tiles(home) <= 32;
    for (int si = 0; si < home->section_count; ++si) {
        for (int wi = 0; wi < home->sections[si].widget_count; ++wi) {
            if (home->sections[si].widgets[wi].type == NL_WIDGET_PHOTO_FRAME) {
                s_home_cacheable = false;
            }
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

    /* 多屏：内容区左右滑换屏（Tabbar 不跟滑）；最多保留一棵普通页面树 */
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

// Caller holds the display lock. One pending callback regardless of network burst size.
void nl_ov_request_home_state_refresh(void) {
    static bool queued = false;
    if (queued)
        return;
    queued = true;
    if (lv_async_call(
            [](void*) {
                queued = false;
                nl_ov_refresh_home_tiles();
            },
            nullptr) != LV_RESULT_OK) {
        queued = false;
        nl_ov_refresh_home_tiles();
    }
}
