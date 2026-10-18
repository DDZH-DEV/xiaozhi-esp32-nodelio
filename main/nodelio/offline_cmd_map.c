#include "offline_cmd_map.h"

#include "media_player.h"
#include "skill_result_ui.h"

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
    if (strcmp(w->skin, "dimmer") == 0) {
        return true;
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

bool nodelio_offline_cmd_is_local(int command_id)
{
    return command_id == NL_OC_REBOOT || command_id == NL_OC_GOODBYE ||
           command_id == NL_OC_MEDIA_PREV || command_id == NL_OC_MEDIA_NEXT ||
           command_id == NL_OC_MEDIA_PAUSE || command_id == NL_OC_MEDIA_RESUME ||
           command_id == NL_OC_CTRL_OFF || command_id == NL_OC_WIFI_CONFIG ||
           (command_id >= NL_OC_GOTO_HOME && command_id <= NL_OC_GOTO_SYS) ||
           (command_id >= NL_OC_POWER_SAVE_ON && command_id <= NL_OC_GOTO_RESET) ||
           command_id == NL_OC_PAGE_NEXT || command_id == NL_OC_PAGE_PREV ||
           command_id == NL_OC_SCROLL_DOWN || command_id == NL_OC_SCROLL_UP;
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
    case NL_OC_REBOOT:
        return "重启";
    case NL_OC_GOODBYE:
        return "退下";
    case NL_OC_MEDIA_PREV:
        return "上一首";
    case NL_OC_MEDIA_NEXT:
        return "下一首";
    case NL_OC_MEDIA_PAUSE:
        return "暂停";
    case NL_OC_MEDIA_RESUME:
        return "继续播放";
    case NL_OC_CTRL_OFF:
        return "控家未启用 · 设置→控家 或装应用";
    case NL_OC_WIFI_CONFIG:
        return "进入配网";
    case NL_OC_GOTO_HOME:
        return "打开首页";
    case NL_OC_GOTO_APPS:
        return "打开应用";
    case NL_OC_GOTO_SETTINGS:
        return "打开设置";
    case NL_OC_GOTO_CONNECT:
        return "打开连接";
    case NL_OC_GOTO_DISP:
        return "显示与声音";
    case NL_OC_GOTO_POWER:
        return "打开省电";
    case NL_OC_GOTO_VOICE:
        return "语音助手";
    case NL_OC_GOTO_MEDIA:
        return "播放器设置";
    case NL_OC_GOTO_CTRL:
        return "控家设置";
    case NL_OC_GOTO_SYS:
        return "设备信息";
    case NL_OC_POWER_SAVE_ON:
        return "开启省电";
    case NL_OC_POWER_SAVE_OFF:
        return "关闭省电";
    case NL_OC_CONTROLS_ON:
        return "打开控家";
    case NL_OC_CONTROLS_OFF:
        return "关闭控家";
    case NL_OC_XIAOZHI_ON:
        return "打开小智";
    case NL_OC_XIAOZHI_OFF:
        return "关闭小智";
    case NL_OC_LAYOUT_SYNC:
        return "刷新布局";
    case NL_OC_FW_CHECK:
        return "检查更新";
    case NL_OC_GOTO_RESET:
        return "恢复出厂";
    case NL_OC_PAGE_NEXT:
        return "下一屏";
    case NL_OC_PAGE_PREV:
        return "上一屏";
    case NL_OC_SCROLL_DOWN:
        return "往下翻";
    case NL_OC_SCROLL_UP:
        return "往上翻";
    default:
        return "未知口令";
    }
}

esp_err_t nodelio_offline_cmd_dispatch(const nodelio_nvs_config_t *cfg, int command_id,
                                       const nodelio_panel_layout_t *layout)
{
    /* 本机动作：不依赖 cfg / layout / WiFi */
    if (command_id == NL_OC_REBOOT) {
        nodelio_panel_ui_set_voice_hint("正在重启…");
        nodelio_panel_ui_local_reboot();
        return ESP_OK;
    }
    if (command_id == NL_OC_WIFI_CONFIG) {
        nodelio_panel_ui_set_voice_hint("进入配网热点");
        nodelio_panel_ui_local_wifi_config();
        return ESP_OK;
    }
    if (command_id >= NL_OC_GOTO_HOME && command_id <= NL_OC_GOTO_SYS) {
        static const int k_map[] = {
            NL_UI_HOME, NL_UI_APPS, NL_UI_SETTINGS, NL_UI_CONNECT, NL_UI_DISP,
            NL_UI_POWER, NL_UI_VOICE, NL_UI_MEDIA, NL_UI_CTRL, NL_UI_SYS,
        };
        int idx = command_id - NL_OC_GOTO_HOME;
        if (idx >= 0 && idx < (int)(sizeof(k_map) / sizeof(k_map[0]))) {
            const char *zh = nodelio_offline_cmd_zh(command_id);
            nodelio_panel_ui_set_voice_hint(zh ? zh : "已切换");
            nodelio_panel_ui_goto(k_map[idx]);
            return ESP_OK;
        }
    }
    if (command_id == NL_OC_POWER_SAVE_ON || command_id == NL_OC_POWER_SAVE_OFF) {
        const char *val = (command_id == NL_OC_POWER_SAVE_ON) ? "on" : "off";
        char err[64];
        esp_err_t e = nodelio_panel_ui_set_pref("power_save", val, err, sizeof(err));
        nodelio_panel_ui_set_voice_hint(e == ESP_OK ? nodelio_offline_cmd_zh(command_id)
                                                    : (err[0] ? err : "设置失败"));
        return e;
    }
    if (command_id == NL_OC_CONTROLS_ON || command_id == NL_OC_CONTROLS_OFF) {
        const char *val = (command_id == NL_OC_CONTROLS_ON) ? "on" : "off";
        char err[64];
        esp_err_t e = nodelio_panel_ui_set_pref("controls", val, err, sizeof(err));
        nodelio_panel_ui_set_voice_hint(e == ESP_OK ? nodelio_offline_cmd_zh(command_id)
                                                    : (err[0] ? err : "设置失败"));
        return e;
    }
    if (command_id == NL_OC_XIAOZHI_ON || command_id == NL_OC_XIAOZHI_OFF) {
        const char *val = (command_id == NL_OC_XIAOZHI_ON) ? "on" : "off";
        char err[64];
        esp_err_t e = nodelio_panel_ui_set_pref("xiaozhi", val, err, sizeof(err));
        nodelio_panel_ui_set_voice_hint(e == ESP_OK ? nodelio_offline_cmd_zh(command_id)
                                                    : (err[0] ? err : "设置失败"));
        return e;
    }
    if (command_id == NL_OC_LAYOUT_SYNC) {
        esp_err_t e = nodelio_panel_ui_set_pref("layout_sync", "", NULL, 0);
        nodelio_panel_ui_set_voice_hint("刷新布局");
        return e;
    }
    if (command_id == NL_OC_FW_CHECK) {
        char err[64];
        esp_err_t e = nodelio_panel_ui_set_pref("fw_check", "", err, sizeof(err));
        nodelio_panel_ui_set_voice_hint(e == ESP_OK ? "检查更新" : (err[0] ? err : "检查失败"));
        return e;
    }
    if (command_id == NL_OC_GOTO_RESET) {
        nodelio_panel_ui_set_voice_hint("请在屏幕确认恢复出厂");
        nodelio_panel_ui_goto(NL_UI_RESET);
        return ESP_OK;
    }
    if (command_id == NL_OC_PAGE_NEXT || command_id == NL_OC_PAGE_PREV) {
        int delta = (command_id == NL_OC_PAGE_NEXT) ? 1 : -1;
        esp_err_t e = nodelio_panel_ui_home_page(delta);
        const char *hint = (delta > 0) ? "下一屏" : "上一屏";
        if (e == ESP_ERR_NOT_FOUND) {
            hint = "只有一屏";
        } else if (e == ESP_ERR_INVALID_STATE) {
            hint = (delta < 0) ? "已经是第一屏" : "已经是最后一屏";
        } else if (e != ESP_OK) {
            nodelio_panel_ui_set_voice_hint("翻页失败");
            return e;
        }
        nodelio_panel_ui_set_voice_hint(hint);
        return ESP_OK;
    }
    if (command_id == NL_OC_SCROLL_DOWN || command_id == NL_OC_SCROLL_UP) {
        int dir = (command_id == NL_OC_SCROLL_DOWN) ? 1 : -1;
        esp_err_t e = nodelio_panel_ui_scroll(dir);
        const char *hint = (dir > 0) ? "往下翻" : "往上翻";
        if (e == ESP_ERR_NOT_FOUND) {
            hint = "当前页不能滚动";
        } else if (e == ESP_ERR_INVALID_STATE) {
            hint = (dir < 0) ? "已经到顶了" : "已经到底了";
        } else if (e != ESP_OK) {
            nodelio_panel_ui_set_voice_hint("滚动失败");
            return e;
        }
        nodelio_panel_ui_set_voice_hint(hint);
        return ESP_OK;
    }
    if (command_id == NL_OC_GOODBYE) {
        nodelio_panel_ui_set_voice_hint("已退下");
        nodelio_panel_ui_local_goodbye();
        return ESP_OK;
    }
    if (command_id == NL_OC_CTRL_OFF) {
        /* G1：关控家时「开灯」类口令只提示，不执行；引导去设置或装应用 */
        nodelio_panel_ui_set_voice_hint("控家未启用 · 设置→控家 或装应用");
        return ESP_OK;
    }
    if (command_id == NL_OC_MEDIA_PREV) {
        if (!nodelio_skill_media_prev()) {
            nodelio_panel_ui_set_voice_hint("没有上一首");
            return ESP_ERR_NOT_FOUND;
        }
        nodelio_panel_ui_set_voice_hint("上一首");
        return ESP_OK;
    }
    if (command_id == NL_OC_MEDIA_NEXT) {
        if (!nodelio_skill_media_next()) {
            nodelio_panel_ui_set_voice_hint("没有下一首");
            return ESP_ERR_NOT_FOUND;
        }
        nodelio_panel_ui_set_voice_hint("下一首");
        return ESP_OK;
    }
    if (command_id == NL_OC_MEDIA_PAUSE) {
        if (nodelio_media_is_paused()) {
            nodelio_panel_ui_set_voice_hint("已暂停");
            return ESP_OK;
        }
        if (!nodelio_media_is_playing()) {
            nodelio_panel_ui_set_voice_hint("没有在播");
            return ESP_ERR_INVALID_STATE;
        }
        nodelio_media_pause();
        nodelio_panel_ui_set_voice_hint("已暂停");
        return ESP_OK;
    }
    if (command_id == NL_OC_MEDIA_RESUME) {
        if (nodelio_media_is_playing()) {
            nodelio_panel_ui_set_voice_hint("正在播放");
            return ESP_OK;
        }
        if (!nodelio_media_is_paused()) {
            nodelio_panel_ui_set_voice_hint("没有可继续的曲");
            return ESP_ERR_INVALID_STATE;
        }
        nodelio_media_resume();
        nodelio_panel_ui_set_voice_hint("继续播放");
        return ESP_OK;
    }

    if (!nodelio_controls_enabled()) {
        nodelio_panel_ui_set_voice_hint("控家未启用");
        return ESP_ERR_INVALID_STATE;
    }

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
    if (strstr(phrase, "重启") || strstr(phrase, "重新启动") || strstr(phrase, "重新开机")) {
        return NL_OC_REBOOT;
    }
    /* SoftAP 配网（≠ 屏上「网络与连接」表单） */
    if (strstr(phrase, "进入配网") || strstr(phrase, "打开热点") || strstr(phrase, "配网模式") ||
        strstr(phrase, "WiFi配置") || strstr(phrase, "wifi配置") || strstr(phrase, "WIFI配置") ||
        (strstr(phrase, "配网") && !strstr(phrase, "网络设置"))) {
        return NL_OC_WIFI_CONFIG;
    }
    /* 竖滚：须在「往后翻/往前翻」换屏之前，避免「往」字串误匹配 */
    if (strstr(phrase, "往下翻") || strstr(phrase, "向下翻") || strstr(phrase, "向下滑") ||
        strstr(phrase, "往下滑") || strstr(phrase, "再往下")) {
        return NL_OC_SCROLL_DOWN;
    }
    if (strstr(phrase, "往上翻") || strstr(phrase, "向上翻") || strstr(phrase, "向上滑") ||
        strstr(phrase, "往上滑") || strstr(phrase, "再往上")) {
        return NL_OC_SCROLL_UP;
    }
    /* 须在「下一首/上一首」之前；「屏」与「首」不同字 */
    if (strstr(phrase, "下一屏") || strstr(phrase, "往后翻") || strstr(phrase, "向后翻")) {
        return NL_OC_PAGE_NEXT;
    }
    if (strstr(phrase, "上一屏") || strstr(phrase, "往前翻") || strstr(phrase, "向前翻")) {
        return NL_OC_PAGE_PREV;
    }
    /* UI 导航：须在裸「打开」控家口令之前 */
    if (strstr(phrase, "回到首页") || strstr(phrase, "打开首页") || strstr(phrase, "返回首页") ||
        (strstr(phrase, "首页") && !strstr(phrase, "回家"))) {
        return NL_OC_GOTO_HOME;
    }
    if (strstr(phrase, "打开应用") || strstr(phrase, "应用列表") || strstr(phrase, "技能列表") ||
        strstr(phrase, "应用页")) {
        return NL_OC_GOTO_APPS;
    }
    if (strstr(phrase, "打开设置") || strstr(phrase, "进入设置") || strstr(phrase, "设置页")) {
        return NL_OC_GOTO_SETTINGS;
    }
    if (strstr(phrase, "打开连接") || strstr(phrase, "网络与连接") || strstr(phrase, "网络设置") ||
        strstr(phrase, "服务器设置") || strstr(phrase, "绑定设置")) {
        return NL_OC_GOTO_CONNECT;
    }
    if (strstr(phrase, "显示与声音") || strstr(phrase, "亮度设置") || strstr(phrase, "音量设置") ||
        strstr(phrase, "打开亮度") || strstr(phrase, "调节亮度") || strstr(phrase, "调节音量")) {
        return NL_OC_GOTO_DISP;
    }
    /* 开关用「开启/关闭」；页用「打开/进入」——勿与 GOTO 混淆 */
    if (strstr(phrase, "开启省电") || strstr(phrase, "打开省电开关")) {
        return NL_OC_POWER_SAVE_ON;
    }
    if (strstr(phrase, "关闭省电") || strstr(phrase, "关掉省电")) {
        return NL_OC_POWER_SAVE_OFF;
    }
    if (strstr(phrase, "打开省电") || strstr(phrase, "省电设置") || strstr(phrase, "省电页") ||
        strstr(phrase, "进入省电")) {
        return NL_OC_GOTO_POWER;
    }
    if (strstr(phrase, "打开小智") || strstr(phrase, "开启小智") || strstr(phrase, "启用小智")) {
        return NL_OC_XIAOZHI_ON;
    }
    if (strstr(phrase, "关闭小智") || strstr(phrase, "关掉小智")) {
        return NL_OC_XIAOZHI_OFF;
    }
    if (strstr(phrase, "刷新布局") || strstr(phrase, "同步布局") || strstr(phrase, "更新布局")) {
        return NL_OC_LAYOUT_SYNC;
    }
    if (strstr(phrase, "检查更新") || strstr(phrase, "检查固件") || strstr(phrase, "固件更新")) {
        return NL_OC_FW_CHECK;
    }
    if (strstr(phrase, "恢复出厂") || strstr(phrase, "出厂重置") || strstr(phrase, "恢复出厂设置")) {
        return NL_OC_GOTO_RESET;
    }
    if (strstr(phrase, "语音助手") || strstr(phrase, "语音设置") || strstr(phrase, "打开语音")) {
        return NL_OC_GOTO_VOICE;
    }
    if (strstr(phrase, "播放器设置") || strstr(phrase, "播放器页")) {
        return NL_OC_GOTO_MEDIA;
    }
    if (strstr(phrase, "打开控家") || strstr(phrase, "开启控家")) {
        return NL_OC_CONTROLS_ON;
    }
    if (strstr(phrase, "关闭控家") || strstr(phrase, "关掉控家")) {
        return NL_OC_CONTROLS_OFF;
    }
    if (strstr(phrase, "控家设置") || strstr(phrase, "打开控家设置") ||
        strstr(phrase, "进入控家")) {
        return NL_OC_GOTO_CTRL;
    }
    if (strstr(phrase, "设备信息") || strstr(phrase, "系统信息") || strstr(phrase, "关于本机")) {
        return NL_OC_GOTO_SYS;
    }
    if (strstr(phrase, "退下") || strstr(phrase, "再见") || strstr(phrase, "拜拜") ||
        strstr(phrase, "息屏") || strstr(phrase, "睡觉")) {
        return NL_OC_GOODBYE;
    }
    if (strstr(phrase, "上一首") || strstr(phrase, "上一曲")) {
        return NL_OC_MEDIA_PREV;
    }
    if (strstr(phrase, "下一首") || strstr(phrase, "下一曲") || strstr(phrase, "切歌")) {
        return NL_OC_MEDIA_NEXT;
    }
    if (strstr(phrase, "暂停")) {
        return NL_OC_MEDIA_PAUSE;
    }
    if (strstr(phrase, "继续播放") || strstr(phrase, "继续播") || strstr(phrase, "恢复播放")) {
        return NL_OC_MEDIA_RESUME;
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

/** 抽取到的应用名是否像口令（拒「失败了，你再试一下」这类整句） */
static bool app_query_plausible(const char *q)
{
    if (!q || !q[0]) {
        return false;
    }
    if (strstr(q, "失败") || strstr(q, "再试") || strstr(q, "试试") || strstr(q, "一下") ||
        strstr(q, "看看") || strstr(q, "等会") || strstr(q, "一会儿") || strstr(q, "怎么") ||
        strstr(q, "为什么") || strstr(q, "卡住") || strstr(q, "不行")) {
        return false;
    }
    if (strstr(q, "，") || strstr(q, "。") || strstr(q, "？") || strstr(q, "！") ||
        strstr(q, ",") || strstr(q, ".") || strstr(q, "?") || strstr(q, "!") ||
        strchr(q, ' ') || strchr(q, '\t')) {
        return false;
    }
    /* 应用名通常很短；过长当整句抱怨 */
    size_t n = strlen(q);
    if (n > 24) {
        return false;
    }
    return true;
}

/** 「打开曲库/天气…」→ 应用 invoke；控家口令不抢 */
static bool try_open_named_app(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return false;
    }
    if (!strstr(phrase, "打开") && !strstr(phrase, "开启")) {
        return false;
    }
    /* 「好像打开失败了，你再试一下」等：勿当开应用 */
    if (strstr(phrase, "打开失败") || strstr(phrase, "开启失败") || strstr(phrase, "打不开") ||
        strstr(phrase, "没打开") || strstr(phrase, "没有打开") || strstr(phrase, "未打开") ||
        strstr(phrase, "再试") || strstr(phrase, "重试") || strstr(phrase, "又开")) {
        return false;
    }
    /* 控家对象留给 control / MultiNet 动态词 */
    if (strstr(phrase, "灯") || strstr(phrase, "帘") || strstr(phrase, "继电器") ||
        strstr(phrase, "插座") || strstr(phrase, "开关") || strstr(phrase, "情景")) {
        return false;
    }
    const char *p = strstr(phrase, "打开一下");
    if (p) {
        p += 12; /* UTF-8: 打开一下 = 4*3 */
    } else if ((p = strstr(phrase, "打开")) != NULL) {
        p += 6;
    } else if ((p = strstr(phrase, "开启")) != NULL) {
        p += 6;
    } else {
        return false;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    /* 跳过「的」（UTF-8 三字节），勿写 *p == '的'（multi-char） */
    if ((unsigned char)p[0] == 0xE7 && (unsigned char)p[1] == 0x9A &&
        (unsigned char)p[2] == 0x84) {
        p += 3;
    }
    if (!p[0]) {
        return false;
    }
    /* 纯导航词已由 GOTO 处理 */
    if (strncmp(p, "首页", 6) == 0 || strncmp(p, "设置", 6) == 0 || strncmp(p, "应用", 6) == 0 ||
        strncmp(p, "连接", 6) == 0 || strncmp(p, "热点", 6) == 0 || strncmp(p, "配网", 6) == 0 ||
        strncmp(p, "省电", 6) == 0 || strncmp(p, "亮度", 6) == 0 || strncmp(p, "音量", 6) == 0 ||
        strncmp(p, "控家", 6) == 0 || strncmp(p, "语音", 6) == 0 || strncmp(p, "小智", 6) == 0) {
        return false;
    }
    if (!app_query_plausible(p)) {
        ESP_LOGI(TAG, "skip open-app (not a name): '%s'", p);
        return false;
    }
    ESP_LOGI(TAG, "open app query='%s'", p);
    return nodelio_panel_ui_open_app(p);
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

bool nodelio_offline_try_system_phrase(const char *phrase, bool *end_session)
{
    if (end_session) {
        *end_session = false;
    }
    int cmd = phrase_to_cmd_id(phrase);
    if (cmd == NL_OC_REBOOT || cmd == NL_OC_WIFI_CONFIG ||
        (cmd >= NL_OC_GOTO_HOME && cmd <= NL_OC_GOTO_SYS) ||
        (cmd >= NL_OC_POWER_SAVE_ON && cmd <= NL_OC_GOTO_RESET) ||
        cmd == NL_OC_PAGE_NEXT || cmd == NL_OC_PAGE_PREV ||
        cmd == NL_OC_SCROLL_DOWN || cmd == NL_OC_SCROLL_UP) {
        ESP_LOGI(TAG, "system phrase → %s", nodelio_offline_cmd_zh(cmd));
        if (end_session && (cmd == NL_OC_REBOOT || cmd == NL_OC_WIFI_CONFIG)) {
            *end_session = true;
        }
        /* 开启/关闭省电等 set_pref：end_session 保持 false，会话不退下 */
        return nodelio_offline_cmd_dispatch(NULL, cmd, NULL) == ESP_OK;
    }
    /* 「打开曲库」等：phrase_to_cmd_id 会落到 LIGHT_ON，这里再抢一次应用 */
    if (try_open_named_app(phrase)) {
        return true;
    }
    return false;
}
