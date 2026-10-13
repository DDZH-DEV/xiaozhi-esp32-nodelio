#include "offline_cmd_map.h"

#include "panel_layout.h"
#include "panel_ui.h"

#include <esp_log.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "nl_oc_map";

typedef enum {
    OC_PREF_ANY = 0,
    OC_PREF_LIGHT,
    OC_PREF_CURTAIN,
} oc_pref_t;

static bool title_has(const char *title, const char *kw)
{
    return title && kw && kw[0] && strstr(title, kw) != NULL;
}

static bool widget_is_onoff(const nl_widget_t *w)
{
    if (!w || w->bind.kind == NL_BIND_NONE || w->bind.device_id[0] == '\0') {
        return false;
    }
    if (w->type == NL_WIDGET_SWITCH_TILE || w->type == NL_WIDGET_PIN_TOGGLE) {
        return true;
    }
    /* 通道/引脚绑定也认（类型字段偶发未知时） */
    if (w->bind.kind == NL_BIND_DEVICE_CHANNEL || w->bind.kind == NL_BIND_DEVICE_PIN) {
        if (w->type != NL_WIDGET_SLIDER_TILE) {
            return true;
        }
    }
    return false;
}

static bool looks_like_light(const nl_widget_t *w)
{
    if (!w) {
        return false;
    }
    if (title_has(w->title, "灯") || title_has(w->title, "光") || title_has(w->title, "照明") ||
        title_has(w->title, "继电器") || title_has(w->title, "开关") || title_has(w->title, "插座")) {
        return true;
    }
    if (title_has(w->bind.name, "灯") || title_has(w->bind.name, "light") ||
        title_has(w->bind.name, "relay") || title_has(w->bind.name, "开关")) {
        return true;
    }
    return false;
}

static bool looks_like_curtain(const nl_widget_t *w)
{
    if (!w) {
        return false;
    }
    if (title_has(w->title, "帘") || title_has(w->title, "窗") || title_has(w->title, "幕")) {
        return true;
    }
    if (title_has(w->bind.name, "帘") || title_has(w->bind.name, "curtain") ||
        title_has(w->bind.name, "shade")) {
        return true;
    }
    return false;
}

/** 分数越高越优先：在线 + 偏好关键字 + 有标题 */
static int score_onoff(const nl_widget_t *w, oc_pref_t pref)
{
    int s = 10;
    if (nodelio_panel_ui_device_online(w->bind.device_id)) {
        s += 100;
    }
    if (pref == OC_PREF_LIGHT && looks_like_light(w)) {
        s += 50;
    }
    if (pref == OC_PREF_CURTAIN && looks_like_curtain(w)) {
        s += 60;
    }
    if (pref == OC_PREF_CURTAIN && looks_like_light(w)) {
        s -= 30; /* 说窗帘时勿误开灯 */
    }
    if (pref == OC_PREF_LIGHT && looks_like_curtain(w)) {
        s -= 20;
    }
    if (w->type == NL_WIDGET_PIN_TOGGLE || w->bind.kind == NL_BIND_DEVICE_PIN) {
        s += 5;
    }
    if (w->title[0]) {
        s += 2;
    }
    return s;
}

static const nl_widget_t *find_best_onoff(const nodelio_panel_layout_t *layout, oc_pref_t pref)
{
    const nl_widget_t *best_on = NULL;
    const nl_widget_t *best_any = NULL;
    int best_on_sc = -1;
    int best_any_sc = -1;
    if (!layout) {
        return NULL;
    }
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (!widget_is_onoff(w)) {
                    continue;
                }
                if (pref == OC_PREF_CURTAIN && !looks_like_curtain(w)) {
                    continue; /* 窗帘口令必须命中帘/窗类控件 */
                }
                int sc = score_onoff(w, pref);
                if (sc > best_any_sc) {
                    best_any_sc = sc;
                    best_any = w;
                }
                if (nodelio_panel_ui_device_online(w->bind.device_id) && sc > best_on_sc) {
                    best_on_sc = sc;
                    best_on = w;
                }
            }
        }
    }
    /* 有在线目标绝不过离线；全离线时交给 dispatch 报离线 */
    return best_on ? best_on : best_any;
}

static const nl_widget_t *find_scene(const nodelio_panel_layout_t *layout, const char *kw_a, const char *kw_b)
{
    const nl_widget_t *first = NULL;
    const nl_widget_t *named = NULL;
    if (!layout) {
        return NULL;
    }
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->bind.kind != NL_BIND_HOME_SCENARIO) {
                    continue;
                }
                if (!first) {
                    first = w;
                }
                if (title_has(w->title, kw_a) || title_has(w->title, kw_b) || title_has(w->bind.name, kw_a) ||
                    title_has(w->bind.name, kw_b)) {
                    named = w;
                }
            }
        }
    }
    return named ? named : first;
}

static const char *widget_label(const nl_widget_t *w)
{
    if (!w) {
        return "?";
    }
    if (w->title[0]) {
        return w->title;
    }
    if (w->bind.name[0]) {
        return w->bind.name;
    }
    return w->id;
}

const char *nodelio_offline_cmd_zh(int command_id)
{
    switch (command_id) {
    case NL_OC_LIGHT_ON:
        return "开灯";
    case NL_OC_LIGHT_OFF:
        return "关灯";
    case NL_OC_SCENE_HOME:
        return "回家";
    case NL_OC_SCENE_AWAY:
        return "出门";
    case NL_OC_CURTAIN_OPEN:
        return "打开窗帘";
    case NL_OC_CURTAIN_CLOSE:
        return "关闭窗帘";
    default:
        return "未知口令";
    }
}

esp_err_t nodelio_offline_cmd_dispatch(const nodelio_nvs_config_t *cfg, int command_id,
                                       const nodelio_panel_layout_t *layout)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!layout) {
        layout = nodelio_panel_ui_get_layout();
    }
    if (!layout) {
        ESP_LOGW(TAG, "no layout — cannot map cmd %d", command_id);
        nodelio_panel_ui_set_voice_hint("无布局，请先拉布局");
        return ESP_ERR_INVALID_STATE;
    }

    const nl_widget_t *w = NULL;
    const char *value = NULL;
    const char *verb = "";
    const char *miss = "布局无开关/引脚";

    switch (command_id) {
    case NL_OC_LIGHT_ON:
        w = find_best_onoff(layout, OC_PREF_LIGHT);
        value = "ON";
        verb = "开";
        break;
    case NL_OC_LIGHT_OFF:
        w = find_best_onoff(layout, OC_PREF_LIGHT);
        value = "OFF";
        verb = "关";
        break;
    case NL_OC_SCENE_HOME:
        w = find_scene(layout, "回家", "到家");
        value = NULL;
        verb = "情景";
        miss = "布局无情景";
        break;
    case NL_OC_SCENE_AWAY:
        w = find_scene(layout, "出门", "离开");
        value = NULL;
        verb = "情景";
        miss = "布局无情景";
        break;
    case NL_OC_CURTAIN_OPEN:
        w = find_best_onoff(layout, OC_PREF_CURTAIN);
        value = "ON";
        verb = "开";
        miss = "布局无窗帘控件";
        break;
    case NL_OC_CURTAIN_CLOSE:
        w = find_best_onoff(layout, OC_PREF_CURTAIN);
        value = "OFF";
        verb = "关";
        miss = "布局无窗帘控件";
        break;
    default:
        ESP_LOGW(TAG, "unknown command_id=%d", command_id);
        nodelio_panel_ui_set_voice_hint("未知命令");
        return ESP_ERR_NOT_FOUND;
    }

    if (!w) {
        int n_sw = 0, n_sc = 0;
        if (layout) {
            for (int pi = 0; pi < layout->page_count; pi++) {
                const nl_page_t *pg = &layout->pages[pi];
                for (int si = 0; si < pg->section_count; si++) {
                    const nl_section_t *sec = &pg->sections[si];
                    for (int wi = 0; wi < sec->widget_count; wi++) {
                        const nl_widget_t *x = &sec->widgets[wi];
                        if (x->bind.kind == NL_BIND_HOME_SCENARIO) {
                            n_sc++;
                        } else if (widget_is_onoff(x)) {
                            n_sw++;
                        }
                    }
                }
            }
        }
        ESP_LOGW(TAG, "no widget for cmd=%d (%s); layout switches=%d scenes=%d", command_id, miss, n_sw,
                 n_sc);
        nodelio_panel_ui_set_voice_hint(miss);
        return ESP_ERR_NOT_FOUND;
    }

    const char *name = widget_label(w);
    bool online = true;
    if (w->bind.kind != NL_BIND_HOME_SCENARIO) {
        online = nodelio_panel_ui_device_online(w->bind.device_id);
    }
    if (!online) {
        char t[48];
        snprintf(t, sizeof(t), "%s离线", name);
        nodelio_panel_ui_set_voice_hint(t);
        ESP_LOGW(TAG, "skip offline target %s device=%s", w->id, w->bind.device_id);
        return ESP_ERR_INVALID_STATE;
    }

    {
        char t[48];
        snprintf(t, sizeof(t), "%s %s…", verb, name);
        nodelio_panel_ui_set_voice_hint(t);
    }

    int http = 0;
    esp_err_t err = nodelio_panel_command(cfg, w->id, "set", &w->bind, value, &http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel_command id=%d widget=%s http=%d: %s", command_id, w->id, http,
                 esp_err_to_name(err));
        nodelio_panel_ui_set_voice_hint(err == ESP_ERR_INVALID_STATE ? "设备离线" : "执行失败");
        return err;
    }
    {
        char t[48];
        snprintf(t, sizeof(t), "已%s %s", verb, name);
        nodelio_panel_ui_set_voice_hint(t);
    }
    ESP_LOGI(TAG, "ok cmd=%d → %s (%s)", command_id, w->id, name);
    return ESP_OK;
}

static bool phrase_has_off(const char *phrase)
{
    if (!phrase) {
        return false;
    }
    /* 「开关」含「关」字，不能单靠 strstr("关") */
    if (strstr(phrase, "关掉") || strstr(phrase, "关闭") || strstr(phrase, "关上") ||
        strstr(phrase, "关灯") || strstr(phrase, "关继电器") || strstr(phrase, "关插座") ||
        strstr(phrase, "关一下") || strstr(phrase, "关窗帘") || strstr(phrase, "关卷帘")) {
        return true;
    }
    if (strstr(phrase, "打开") || strstr(phrase, "开启") || strstr(phrase, "开灯") ||
        strstr(phrase, "开继电器") || strstr(phrase, "开插座") || strstr(phrase, "开一下") ||
        strstr(phrase, "开窗帘") || strstr(phrase, "开卷帘")) {
        return false;
    }
    /* 「关xxx」且不是名词「开关」 */
    if (strstr(phrase, "关") && !strstr(phrase, "开关")) {
        return true;
    }
    return false;
}

static int phrase_to_cmd_id(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return 0;
    }
    if (strstr(phrase, "回家") || strstr(phrase, "到家") || strstr(phrase, "我回来了")) {
        return NL_OC_SCENE_HOME;
    }
    if (strstr(phrase, "出门") || strstr(phrase, "离开") || strstr(phrase, "外出") ||
        strstr(phrase, "我走了")) {
        return NL_OC_SCENE_AWAY;
    }
    if (strstr(phrase, "窗帘") || strstr(phrase, "卷帘")) {
        return phrase_has_off(phrase) ? NL_OC_CURTAIN_CLOSE : NL_OC_CURTAIN_OPEN;
    }
    /* 灯 / 继电器 / 开关 / 插座 → 通用通断（find_best_onoff） */
    if (strstr(phrase, "灯") || strstr(phrase, "继电器") || strstr(phrase, "开关") ||
        strstr(phrase, "插座") || strstr(phrase, "通断")) {
        return phrase_has_off(phrase) ? NL_OC_LIGHT_OFF : NL_OC_LIGHT_ON;
    }
    /* 无对象：「打开 / 关闭」→ 优选通断控件 */
    if (strstr(phrase, "关掉") || strstr(phrase, "关闭") || strstr(phrase, "关上")) {
        return NL_OC_LIGHT_OFF;
    }
    if (strstr(phrase, "打开") || strstr(phrase, "开启")) {
        return NL_OC_LIGHT_ON;
    }
    return 0;
}

esp_err_t nodelio_offline_try_phrase(const nodelio_nvs_config_t *cfg, const char *phrase,
                                     const nodelio_panel_layout_t *layout)
{
    int cmd = phrase_to_cmd_id(phrase);
    if (cmd == 0) {
        /* 口令本身无法识别（与「布局无控件」区分） */
        return ESP_ERR_INVALID_ARG;
    }
    return nodelio_offline_cmd_dispatch(cfg, cmd, layout);
}
