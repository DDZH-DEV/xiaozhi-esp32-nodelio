#include "offline_cmd_map.h"

#include "media_player.h"
#include "offline_cmd.h"
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
           command_id == NL_OC_SCROLL_DOWN || command_id == NL_OC_SCROLL_UP ||
           (command_id >= NL_OC_APP_MUSIC && command_id <= NL_OC_APP_QUOTE) ||
           command_id == NL_OC_SKILL_CLOSE || command_id == NL_OC_RADIO_SWAP ||
           (command_id >= NL_OC_BRIGHT_UP && command_id <= NL_OC_VOL_MIN);
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
    case NL_OC_APP_MUSIC:
        return "打开曲库";
    case NL_OC_APP_NEWS:
        return "打开新闻";
    case NL_OC_APP_WEATHER:
        return "打开天气";
    case NL_OC_APP_RADIO:
        return "打开电台";
    case NL_OC_APP_PODCAST:
        return "打开播客";
    case NL_OC_APP_AIR:
        return "打开空气";
    case NL_OC_APP_QUOTE:
        return "打开每日一句";
    case NL_OC_SKILL_CLOSE:
        return "关闭播放器";
    case NL_OC_RADIO_SWAP:
        return "换台";
    case NL_OC_BRIGHT_UP:
        return "亮一点";
    case NL_OC_BRIGHT_DOWN:
        return "暗一点";
    case NL_OC_VOL_UP:
        return "声音大一点";
    case NL_OC_VOL_DOWN:
        return "声音小一点";
    case NL_OC_BRIGHT_MAX:
        return "亮度最大";
    case NL_OC_BRIGHT_MIN:
        return "亮度最小";
    case NL_OC_VOL_MAX:
        return "声音最大";
    case NL_OC_VOL_MIN:
        return "声音最小";
    default:
        return "未知口令";
    }
}

/* 与 offline_cmd.c 里 MultiNet 固定拼音一一对应，供设置页一览。 */
static const nodelio_oc_catalog_entry_t k_oc_catalog[] = {
    {"本机", "重启 / 重新启动"},
    {"本机", "进入配网 / 配网 / 配网模式 / 打开热点"},
    {"本机", "退下 / 再见 / 拜拜 / 息屏"},
    {"本机", "刷新布局"},
    {"本机", "检查更新"},
    {"本机", "恢复出厂"},
    {"本机", "开启省电 / 开省电"},
    {"本机", "关闭省电 / 关省电"},
    {"本机", "打开小智 / 开启小智"},
    {"本机", "关闭小智"},
    {"本机", "打开控家"},
    {"本机", "关闭控家"},
    {"导航", "打开首页 / 回到首页"},
    {"导航", "打开应用"},
    {"导航", "打开设置"},
    {"导航", "打开连接 / 网络设置"},
    {"导航", "显示与声音"},
    {"设置", "亮一点 / 暗一点 / 亮度高一点 / 亮度最大"},
    {"设置", "声音大一点 / 声音小一点 / 声音最大 / 声音最小"},
    {"导航", "打开省电"},
    {"导航", "语音助手"},
    {"导航", "播放器设置"},
    {"导航", "控家设置"},
    {"导航", "设备信息"},
    {"导航", "下一屏 / 往后翻"},
    {"导航", "上一屏 / 往前翻"},
    {"导航", "往下翻 / 向下滑"},
    {"导航", "往上翻 / 向上滑"},
    {"播放", "上一首"},
    {"播放", "下一首 / 切歌 / 换一首"},
    {"播放", "暂停"},
    {"播放", "继续播放"},
    {"播放", "换台 / 换个台（电台回列表）"},
    {"应用", "打开曲库 / 打开音乐"},
    {"应用", "打开新闻"},
    {"应用", "打开天气 / 再问天气 / 刷新天气"},
    {"应用", "打开电台"},
    {"应用", "打开播客"},
    {"应用", "打开空气 / 再问空气 / 刷新空气"},
    {"应用", "打开每日一句 / 换一句 / 再来一句"},
    {"应用", "下一篇 / 上一篇 / 读这篇 / 继续读（新闻卡打开时）"},
    {"应用", "第 N 条 / 打开第一条 / 读第二条（新闻卡打开时）"},
    {"播放", "关闭播放器 / 关闭曲库 / 关掉音乐 / 退出播放器"},
    {"控家关", "开灯 / 关灯 / 回家 / 出门（仅控家关闭时提示）"},
};

const nodelio_oc_catalog_entry_t *nodelio_offline_cmd_catalog(size_t *count)
{
    if (count) {
        *count = sizeof(k_oc_catalog) / sizeof(k_oc_catalog[0]);
    }
    return k_oc_catalog;
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
    if (command_id >= NL_OC_APP_MUSIC && command_id <= NL_OC_APP_QUOTE) {
        const char *q = "曲库";
        switch (command_id) {
        case NL_OC_APP_NEWS:
            q = "新闻";
            break;
        case NL_OC_APP_WEATHER:
            q = "天气";
            break;
        case NL_OC_APP_RADIO:
            q = "电台";
            break;
        case NL_OC_APP_PODCAST:
            q = "播客";
            break;
        case NL_OC_APP_AIR:
            q = "空气";
            break;
        case NL_OC_APP_QUOTE:
            q = "每日一句";
            break;
        default:
            break;
        }
        nodelio_panel_ui_set_voice_hint(nodelio_offline_cmd_zh(command_id));
        return nodelio_panel_ui_open_app(q) ? ESP_OK : ESP_FAIL;
    }
    if (command_id == NL_OC_RADIO_SWAP) {
        if (nodelio_skill_is_live_radio() && nodelio_skill_reopen_media_list()) {
            nodelio_panel_ui_set_voice_hint("请点选电台");
            return ESP_OK;
        }
        nodelio_panel_ui_set_voice_hint("打开电台");
        return nodelio_panel_ui_open_app("电台") ? ESP_OK : ESP_FAIL;
    }
    if (command_id >= NL_OC_BRIGHT_UP && command_id <= NL_OC_VOL_MIN) {
        const bool is_bright = (command_id == NL_OC_BRIGHT_UP || command_id == NL_OC_BRIGHT_DOWN ||
                                command_id == NL_OC_BRIGHT_MAX || command_id == NL_OC_BRIGHT_MIN);
        const char *key = is_bright ? "brightness" : "volume";
        int v;
        if (command_id == NL_OC_BRIGHT_MAX || command_id == NL_OC_VOL_MAX) {
            v = 100;
        } else if (command_id == NL_OC_BRIGHT_MIN) {
            v = 5;
        } else if (command_id == NL_OC_VOL_MIN) {
            v = 0;
        } else {
            const int delta =
                (command_id == NL_OC_BRIGHT_UP || command_id == NL_OC_VOL_UP) ? 10 : -10;
            char cur[16] = {0};
            if (nodelio_panel_ui_get_pref(key, cur, sizeof(cur)) != ESP_OK || !cur[0]) {
                snprintf(cur, sizeof(cur), "%s", is_bright ? "80" : "70");
            }
            v = atoi(cur) + delta;
            if (is_bright) {
                if (v < 5) {
                    v = 5;
                }
            } else if (v < 0) {
                v = 0;
            }
            if (v > 100) {
                v = 100;
            }
        }
        char val[8];
        snprintf(val, sizeof(val), "%d", v);
        char err[64];
        esp_err_t e = nodelio_panel_ui_set_pref(key, val, err, sizeof(err));
        if (e != ESP_OK) {
            nodelio_panel_ui_set_voice_hint(err[0] ? err : "调节失败");
            return e;
        }
        nodelio_panel_ui_set_voice_hint(nodelio_offline_cmd_zh(command_id));
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
    if (command_id == NL_OC_SKILL_CLOSE) {
        nodelio_panel_ui_set_voice_hint("已关闭播放器");
        nodelio_skill_close_all();
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

    const nl_widget_t *w = NULL;
    const char *value = NULL;
    const char *verb = "";
    const char *miss = "布局无开关/引脚";

    /* 开机无云布局：用 NVS 动态词回落开灯/关灯（DIOL/云仍可发） */
    if (!layout && (command_id == NL_OC_LIGHT_ON || command_id == NL_OC_LIGHT_OFF ||
                    command_id == NL_OC_CURTAIN_OPEN || command_id == NL_OC_CURTAIN_CLOSE)) {
        const char *act =
            (command_id == NL_OC_LIGHT_ON || command_id == NL_OC_CURTAIN_OPEN) ? "ON" : "OFF";
        char did[32];
        char dname[32];
        int ch = 0;
        bool is_pin = false;
        if (!nodelio_offline_cmd_pick_dyn(act, did, sizeof(did), &ch, &is_pin, dname, sizeof(dname))) {
            nodelio_panel_ui_set_voice_hint("无布局，请先联网拉布局");
            return ESP_ERR_INVALID_STATE;
        }
        nl_bind_t bind;
        memset(&bind, 0, sizeof(bind));
        bind.kind = is_pin ? NL_BIND_DEVICE_PIN : NL_BIND_DEVICE_CHANNEL;
        snprintf(bind.device_id, sizeof(bind.device_id), "%s", did);
        if (is_pin) {
            bind.pin = ch;
            bind.channel = -1;
        } else {
            bind.channel = ch;
            bind.pin = -1;
        }
        {
            char t[48];
            snprintf(t, sizeof(t), "%s %s…", act[1] == 'N' ? "开" : "关", dname);
            nodelio_panel_ui_set_voice_hint(t);
        }
        int http = 0;
        esp_err_t err = nodelio_panel_command(cfg, "dyn_voice", "set", &bind, act, &http);
        if (err != ESP_OK) {
            nodelio_panel_ui_set_voice_hint(err == ESP_ERR_INVALID_STATE ? "设备离线" : "执行失败");
            return err;
        }
        {
            char t[48];
            snprintf(t, sizeof(t), "已%s %s", act[1] == 'N' ? "开" : "关", dname);
            nodelio_panel_ui_set_voice_hint(t);
        }
        return ESP_OK;
    }

    if (!layout) {
        ESP_LOGW(TAG, "no layout — cannot map cmd %d", command_id);
        nodelio_panel_ui_set_voice_hint("无布局，请先拉布局");
        return ESP_ERR_INVALID_STATE;
    }

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
    /* 「打开面板应用」是 MCP/口播常用说法；中间夹「面板」时 strstr「打开应用」匹配不到 */
    if (strstr(phrase, "打开应用") || strstr(phrase, "打开面板应用") ||
        strstr(phrase, "面板应用") || strstr(phrase, "应用列表") || strstr(phrase, "技能列表") ||
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
        strstr(phrase, "打开亮度") || strstr(phrase, "调节亮度") || strstr(phrase, "调节音量") ||
        strstr(phrase, "屏幕亮度设置")) {
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
    /* 亮度/音量：极限值优先；须在「打开亮度」GOTO 与泛化开灯之前 */
    if (strstr(phrase, "亮度最大") || strstr(phrase, "最亮") || strstr(phrase, "屏幕最亮") ||
        strstr(phrase, "亮度调到最大") || strstr(phrase, "亮度调至最大")) {
        return NL_OC_BRIGHT_MAX;
    }
    if (strstr(phrase, "亮度最小") || strstr(phrase, "最暗") || strstr(phrase, "屏幕最暗") ||
        strstr(phrase, "亮度调到最小") || strstr(phrase, "亮度调至最小")) {
        return NL_OC_BRIGHT_MIN;
    }
    if (strstr(phrase, "声音最大") || strstr(phrase, "音量最大") || strstr(phrase, "最大声") ||
        strstr(phrase, "音量调到最大") || strstr(phrase, "声音调到最大") ||
        strstr(phrase, "音量调至最大")) {
        return NL_OC_VOL_MAX;
    }
    if (strstr(phrase, "声音最小") || strstr(phrase, "音量最小") || strstr(phrase, "最小声") ||
        strstr(phrase, "音量调到最小") || strstr(phrase, "声音调到最小") ||
        strstr(phrase, "音量调至最小")) {
        return NL_OC_VOL_MIN;
    }
    if (strstr(phrase, "亮一点") || strstr(phrase, "调亮") || strstr(phrase, "亮度高一点") ||
        strstr(phrase, "屏幕亮一点") || strstr(phrase, "亮一些") || strstr(phrase, "提高亮度") ||
        strstr(phrase, "加大亮度") || strstr(phrase, "亮度调高") || strstr(phrase, "调高亮度") ||
        /* ASR：「屏幕亮度调」常听成「屏幕亮度对」 */
        strstr(phrase, "屏幕亮度对") || strstr(phrase, "屏幕亮度调")) {
        return NL_OC_BRIGHT_UP;
    }
    if (strstr(phrase, "暗一点") || strstr(phrase, "调暗") || strstr(phrase, "亮度低一点") ||
        strstr(phrase, "屏幕暗一点") || strstr(phrase, "暗一些") || strstr(phrase, "降低亮度") ||
        strstr(phrase, "亮度调低") || strstr(phrase, "调低亮度")) {
        return NL_OC_BRIGHT_DOWN;
    }
    if (strstr(phrase, "声音大一点") || strstr(phrase, "音量大一点") || strstr(phrase, "大声点") ||
        strstr(phrase, "音量高一点") || strstr(phrase, "大一点声") || strstr(phrase, "大声一点") ||
        strstr(phrase, "音量调大") || strstr(phrase, "声音调大") || strstr(phrase, "大点声")) {
        return NL_OC_VOL_UP;
    }
    if (strstr(phrase, "声音小一点") || strstr(phrase, "音量小一点") || strstr(phrase, "小声点") ||
        strstr(phrase, "音量低一点") || strstr(phrase, "小一点声") || strstr(phrase, "小声一点") ||
        strstr(phrase, "音量调小") || strstr(phrase, "声音调小") || strstr(phrase, "小点声")) {
        return NL_OC_VOL_DOWN;
    }
    /* 换台 ≠ 换一首；须在切歌之前 */
    if (strstr(phrase, "换台") || strstr(phrase, "换个台") || strstr(phrase, "换个电台")) {
        return NL_OC_RADIO_SWAP;
    }
    /* 信息类再 invoke：勿与「打开…」控家冲突 */
    if (strstr(phrase, "换一句") || strstr(phrase, "再来一句") || strstr(phrase, "换一句名言")) {
        return NL_OC_APP_QUOTE;
    }
    if (strstr(phrase, "再问天气") || strstr(phrase, "刷新天气") || strstr(phrase, "更新天气")) {
        return NL_OC_APP_WEATHER;
    }
    if (strstr(phrase, "再问空气") || strstr(phrase, "刷新空气") || strstr(phrase, "更新空气")) {
        return NL_OC_APP_AIR;
    }
    if (strstr(phrase, "上一首") || strstr(phrase, "上一曲")) {
        return NL_OC_MEDIA_PREV;
    }
    if (strstr(phrase, "下一首") || strstr(phrase, "下一曲") || strstr(phrase, "切歌") ||
        strstr(phrase, "换一首")) {
        return NL_OC_MEDIA_NEXT;
    }
    if (strstr(phrase, "暂停")) {
        return NL_OC_MEDIA_PAUSE;
    }
    if (strstr(phrase, "继续播放") || strstr(phrase, "继续播") || strstr(phrase, "恢复播放")) {
        return NL_OC_MEDIA_RESUME;
    }
    /* 关曲库/播放器须在泛化「关闭」→关灯之前 */
    if ((strstr(phrase, "关闭") || strstr(phrase, "关掉") || strstr(phrase, "关上") ||
         strstr(phrase, "退出") || strstr(phrase, "收起")) &&
        (strstr(phrase, "播放器") || strstr(phrase, "曲库") || strstr(phrase, "音乐") ||
         strstr(phrase, "电台") || strstr(phrase, "播客") || strstr(phrase, "新闻") ||
         strstr(phrase, "技能") || strstr(phrase, "应用"))) {
        return NL_OC_SKILL_CLOSE;
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

/** 「打开面板曲库」口语里的「面板」不是应用名；「取库」是「曲库」ASR 近音。 */
static void normalize_open_app_name(char *name, size_t name_sz)
{
    if (!name || !name[0] || name_sz < 8) {
        return;
    }
    if (strncmp(name, "面板", 6) == 0 && name[6] != '\0') {
        memmove(name, name + 6, strlen(name + 6) + 1);
    }
    if (strcmp(name, "取库") == 0 || strcmp(name, "去库") == 0 || strcmp(name, "趣库") == 0) {
        snprintf(name, name_sz, "%s", "曲库");
    }
}

/** 去掉首尾空白与句末标点（STT 常带「音乐。」）；中间标点仍留给 plausible 拒绝。 */
static void strip_app_query(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src || !src[0]) {
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
    /* 去头空白 */
    size_t lead = 0;
    while (dst[lead] == ' ' || dst[lead] == '\t') {
        lead++;
    }
    if (lead > 0) {
        memmove(dst, dst + lead, strlen(dst + lead) + 1);
    }
    for (;;) {
        size_t n = strlen(dst);
        if (n == 0) {
            break;
        }
        unsigned char c = (unsigned char)dst[n - 1];
        if (c == ' ' || c == '\t' || c == '.' || c == ',' || c == '?' || c == '!' || c == ';' ||
            c == ':' || c == '~' || c == '"' || c == '\'') {
            dst[n - 1] = '\0';
            continue;
        }
        if (n >= 3) {
            unsigned char a = (unsigned char)dst[n - 3];
            unsigned char b = (unsigned char)dst[n - 2];
            unsigned char d = (unsigned char)dst[n - 1];
            /* 。U+3002 、U+3001 */
            if (a == 0xE3 && b == 0x80 && (d == 0x82 || d == 0x81)) {
                dst[n - 3] = '\0';
                continue;
            }
            /* ？！，；： U+FF1F/FF01/FF0C/FF1B/FF1A */
            if (a == 0xEF && b == 0xBC &&
                (d == 0x9F || d == 0x81 || d == 0x8C || d == 0x9B || d == 0x9A)) {
                dst[n - 3] = '\0';
                continue;
            }
            /* … U+2026 */
            if (a == 0xE2 && b == 0x80 && d == 0xA6) {
                dst[n - 3] = '\0';
                continue;
            }
        }
        break;
    }
    normalize_open_app_name(dst, dst_sz);
}

/** UTF-8 大致字数（CJK 按 3 字节计）；用于拒「电」单字误匹配电台。 */
static int utf8_approx_chars(const char *q)
{
    if (!q) {
        return 0;
    }
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)q; *p; ) {
        if (*p < 0x80) {
            p++;
            n++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            p += 2;
            n++;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            p += 3;
            n++;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4;
            n++;
        } else {
            p++;
            n++;
        }
    }
    return n;
}

/** 「第一个 / 第2个」是列表选条，不是应用名；open_app 异步恒 true 会误吞并 suppress TTS */
static bool looks_like_list_ordinal(const char *q)
{
    if (!q || !q[0]) {
        return false;
    }
    if (strstr(q, "第") == NULL) {
        return false;
    }
    return strstr(q, "个") != NULL || strstr(q, "条") != NULL || strstr(q, "项") != NULL ||
           strstr(q, "首") != NULL || strstr(q, "份") != NULL;
}

/** 抽取到的应用名是否像口令（拒「失败了，你再试一下」这类整句） */
static bool app_query_plausible(const char *q)
{
    if (!q || !q[0]) {
        return false;
    }
    if (looks_like_list_ordinal(q)) {
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
    /* 控家残片：「打开电。」≠ 打开电台；须「电台/电器」完整词 */
    if (strcmp(q, "电") == 0 || strcmp(q, "机") == 0 || strcmp(q, "器") == 0 ||
        strcmp(q, "灯") == 0 || strcmp(q, "扇") == 0) {
        return false;
    }
    /* 至少 2 个字；单字极易 fuzzy 进「网络电台」等 */
    if (utf8_approx_chars(q) < 2) {
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
    /* 控家对象留给 control / MultiNet 动态词；「打开电」勿当开应用 */
    if (strstr(phrase, "灯") || strstr(phrase, "帘") || strstr(phrase, "继电器") ||
        strstr(phrase, "插座") || strstr(phrase, "开关") || strstr(phrase, "情景") ||
        strstr(phrase, "电器") || strstr(phrase, "客厅") || strstr(phrase, "卧室") ||
        strstr(phrase, "空调") || strstr(phrase, "风扇")) {
        return false;
    }
    /* 「打开电。」残句：无「电台」则不当开应用（交云端澄清） */
    if (strstr(phrase, "电") && !strstr(phrase, "电台") && !strstr(phrase, "电视") &&
        !strstr(phrase, "电脑") && !strstr(phrase, "电器")) {
        const char *open = strstr(phrase, "打开");
        if (!open) {
            open = strstr(phrase, "开启");
        }
        if (open) {
            const char *rest = open + 6;
            while (*rest == ' ' || *rest == '\t') {
                rest++;
            }
            char tmp[48];
            strip_app_query(tmp, sizeof(tmp), rest);
            if (strcmp(tmp, "电") == 0 || utf8_approx_chars(tmp) < 2) {
                return false;
            }
        }
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
    /* 纯导航词已由 GOTO 处理（含「打开面板应用」剥前缀后剩「应用」） */
    if (strncmp(p, "首页", 6) == 0 || strncmp(p, "设置", 6) == 0 || strncmp(p, "应用", 6) == 0 ||
        strncmp(p, "连接", 6) == 0 || strncmp(p, "热点", 6) == 0 || strncmp(p, "配网", 6) == 0 ||
        strncmp(p, "省电", 6) == 0 || strncmp(p, "亮度", 6) == 0 || strncmp(p, "音量", 6) == 0 ||
        strncmp(p, "控家", 6) == 0 || strncmp(p, "语音", 6) == 0 || strncmp(p, "小智", 6) == 0 ||
        strncmp(p, "面板应用", 12) == 0) {
        return false;
    }
    char name[48];
    strip_app_query(name, sizeof(name), p);
    if (strcmp(name, "应用") == 0 || strcmp(name, "面板应用") == 0) {
        return false;
    }
    /* 「打开任务应用 / 打开DSH」：即使名不完全在白名单也开进度 */
    if (strstr(name, "任务") != NULL || strstr(name, "DSH") != NULL || strstr(name, "dsh") != NULL) {
        if (strstr(name, "派活") == NULL) {
            ESP_LOGI(TAG, "open app (task alias) query='%s' → 查看任务", name);
            return nodelio_panel_ui_open_app("查看任务");
        }
    }
    /* 「打开新闻阅读第一篇」→ 只开新闻（阅读留给卡内旁路） */
    {
        static const char *const kShort[] = {"新闻", "曲库", "取库", "音乐", "电台", "播客", "天气",
                                             "空气", "一句", "名言"};
        for (size_t i = 0; i < sizeof(kShort) / sizeof(kShort[0]); i++) {
            const size_t sl = strlen(kShort[i]);
            if (strncmp(name, kShort[i], sl) == 0 && name[sl] != '\0' &&
                (strstr(name + sl, "阅读") || strstr(name + sl, "朗读") || strstr(name + sl, "第一") ||
                 strstr(name + sl, "读一") || strstr(name + sl, "播报"))) {
                ESP_LOGI(TAG, "open app trim '%s' → '%s'", name, kShort[i]);
                snprintf(name, sizeof(name), "%s", kShort[i]);
                break;
            }
        }
    }
    if (!app_query_plausible(name)) {
        ESP_LOGI(TAG, "skip open-app (not a name): '%s'", name[0] ? name : p);
        return false;
    }
    ESP_LOGI(TAG, "open app query='%s'", name);
    return nodelio_panel_ui_open_app(name);
}

/** 「让它 / 让他 …」→ dsh.task；须带指令正文。ASR 常把「它」听成「他」。 */
static bool try_dsh_dispatch_phrase(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return false;
    }
    const char* p = phrase;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (strncmp(p, "帮我", strlen("帮我")) == 0) {
        p += strlen("帮我");
    }
    static const char* const prefixes[] = {"让助手", "让它去", "让他去", "让它", "让他",
                                           "叫它",   "叫他",   "请它",   "请他"};
    bool matched = false;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const size_t n = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], n) == 0) {
            p += n;
            matched = true;
            break;
        }
    }
    if (!matched) {
        return false;
    }
    for (;;) {
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == ':' || *p == ';' || *p == '-') {
            p++;
            continue;
        }
        /* ，：、 （UTF-8） */
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC &&
            ((unsigned char)p[2] == 0x8C || (unsigned char)p[2] == 0x9A)) {
            p += 3;
            continue;
        }
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80 &&
            (unsigned char)p[2] == 0x81) {
            p += 3;
            continue;
        }
        break;
    }
    /* 正文不走应用名清洗，也不截断 UTF-8、代码和路径。 */
    char probe[16];
    strip_app_query(probe, sizeof(probe), p);
    if (!probe[0]) {
        return nodelio_panel_ui_dsh_task(NULL);
    }
    if (nodelio_panel_ui_dsh_is_armed()) {
        return nodelio_panel_ui_dsh_confirm_send(p);
    }
    return nodelio_panel_ui_dsh_task(p);
}

int nodelio_offline_dsh_selection(const char* phrase, bool* resume) {
    if (resume) {
        *resume = false;
    }
    if (!phrase || strlen(phrase) >= 96) {
        return 0;
    }
    char text[96];
    strip_app_query(text, sizeof(text), phrase);
    const char* p = text;
    static const char* const prefixes[] = {"续聊", "打开", "查看", "选择", "选", "要"};
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const size_t n = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], n) == 0) {
            p += n;
            if (i == 0) {
                if (resume) {
                    *resume = true;
                }
                if (!*p) {
                    return -1;
                }
            }
            break;
        }
    }
    if (strncmp(p, "第", strlen("第")) == 0) {
        p += strlen("第");
    }
    int ordinal = 0;
    if (*p >= '1' && *p <= '9') {
        ordinal = *p++ - '0';
        if (*p >= '0' && *p <= '9') {
            ordinal = ordinal * 10 + *p++ - '0';
        }
    } else {
        static const char* const digits[] = {"零", "一", "二", "三", "四",
                                             "五", "六", "七", "八", "九"};
        for (int i = 1; i <= 9; ++i) {
            if (strncmp(p, digits[i], strlen(digits[i])) == 0) {
                ordinal = i;
                p += strlen(digits[i]);
                break;
            }
        }
        if (strncmp(p, "两", strlen("两")) == 0) {
            ordinal = 2;
            p += strlen("两");
        }
        if (strncmp(p, "十", strlen("十")) == 0) {
            ordinal = (ordinal ? ordinal : 1) * 10;
            p += strlen("十");
            for (int i = 1; i <= 9; ++i) {
                if (strncmp(p, digits[i], strlen(digits[i])) == 0) {
                    ordinal += i;
                    p += strlen(digits[i]);
                    break;
                }
            }
        }
    }
    if (ordinal <= 0) {
        return 0;
    }
    if (strncmp(p, "个", strlen("个")) == 0 || strncmp(p, "条", strlen("条")) == 0) {
        p += strlen("个");
    }
    return (!*p || strcmp(p, "工作区") == 0 || strcmp(p, "会话") == 0 || strcmp(p, "任务") == 0)
               ? ordinal
               : 0;
}

/* 明确的面板导航可以退出续聊输入；其余正文均先确认，不能触发面板重启等操作。 */
bool nodelio_offline_try_dsh_phrase(const char* phrase) {
    if (!phrase || !phrase[0]) {
        return false;
    }
    char short_phrase[96];
    strip_app_query(short_phrase, sizeof(short_phrase), phrase);
    if (strcmp(short_phrase, "取消续聊") == 0 || strcmp(short_phrase, "退出续聊") == 0) {
        if (!nodelio_panel_ui_dsh_is_armed()) {
            return false;
        }
        nodelio_panel_ui_clear_dsh_arm();
        nodelio_skill_dsh_on_arm_cleared();
        nodelio_panel_ui_toast_defer("已取消续聊");
        return true;
    }
    const bool navigation =
        strcmp(short_phrase, "查看任务") == 0 || strcmp(short_phrase, "任务列表") == 0 ||
        strcmp(short_phrase, "关闭面板") == 0 || strcmp(short_phrase, "关闭任务") == 0 ||
        strcmp(short_phrase, "关闭DSH") == 0 || strcmp(short_phrase, "返回首页") == 0;
    if (navigation) {
        nodelio_panel_ui_clear_dsh_arm();
        nodelio_skill_dsh_on_arm_cleared();
        return false;
    }
    if (nodelio_panel_ui_dsh_has_pending_send()) {
        return nodelio_panel_ui_dsh_try_confirm_send(phrase) != 0;
    }
    if (try_dsh_dispatch_phrase(phrase)) {
        return true;
    }
    /* 否定派活不是指令；同时防止正文中的「重启」落入本机系统口令。 */
    if (!nodelio_panel_ui_dsh_is_armed() &&
        (strstr(phrase, "不要让它") || strstr(phrase, "不要让他") || strstr(phrase, "别让它") ||
         strstr(phrase, "别让他") || strstr(phrase, "不用让它") || strstr(phrase, "不用让他"))) {
        nodelio_panel_ui_toast_defer("未发送任务");
        return true;
    }
    /* 序号导航留给技能卡；带「修改第一个文件」的正文不能被抢成选项。 */
    const bool selection = nodelio_offline_dsh_selection(phrase, NULL) != 0;
    if (nodelio_panel_ui_dsh_is_armed() && !selection) {
        return nodelio_panel_ui_dsh_confirm_send(phrase);
    }
    return false;
}

bool nodelio_offline_query_keeps_cloud_tts(const char *query)
{
    if (!query || !query[0]) {
        return false;
    }
    /* 仅「问内容」保留云端播报（天气怎么样）。纯开卡（打开新闻/看新闻）勿 keep：
     * 云端常说「找不到应用」而本地卡已弹出。 */
    if (strstr(query, "新闻") || strstr(query, "news") || strstr(query, "一句") ||
        strstr(query, "名言") || strstr(query, "quote") || strstr(query, "曲库") ||
        strstr(query, "音乐") || strstr(query, "听歌") || strstr(query, "放歌") ||
        strstr(query, "爱音乐") || strstr(query, "music") || strstr(query, "电台") ||
        strstr(query, "radio") || strstr(query, "播客") || strstr(query, "podcast")) {
        return false;
    }
    if (strstr(query, "天气") || strstr(query, "weather")) {
        return strstr(query, "怎么样") != NULL || strstr(query, "如何") != NULL ||
               strstr(query, "几度") != NULL || strstr(query, "冷") != NULL ||
               strstr(query, "热") != NULL || strstr(query, "下雨") != NULL ||
               strstr(query, "预报") != NULL || strstr(query, "明天") != NULL ||
               strstr(query, "后天") != NULL || strstr(query, "今天天气") != NULL;
    }
    if (strstr(query, "空气") || strstr(query, "air")) {
        return strstr(query, "质量") != NULL || strstr(query, "怎么样") != NULL ||
               strstr(query, "如何") != NULL || strstr(query, "AQI") != NULL ||
               strstr(query, "aqi") != NULL;
    }
    return false;
}

bool nodelio_offline_query_keeps_listening(const char *query)
{
    if (!query || !query[0]) {
        return false;
    }
    /* 新闻追问、DSH 序号导航和续聊都需要继续聆听。 */
    if (strstr(query, "新闻") || strstr(query, "news") || strstr(query, "任务") ||
        strstr(query, "DSH") || strstr(query, "dsh") || strstr(query, "派活")) {
        return true;
    }
    return nodelio_offline_query_keeps_cloud_tts(query);
}

/**
 * 听歌/看天气等口语：云端常抢 search_music 等自带工具，本地先开 NodeLio 应用。
 * 不要求「打开」前缀。
 */
static bool try_open_intent_app(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return false;
    }
    /* 音乐 */
    if (strstr(phrase, "听歌") || strstr(phrase, "放歌") || strstr(phrase, "来首歌") ||
        strstr(phrase, "播放音乐") || strstr(phrase, "放音乐") || strstr(phrase, "爱音乐") ||
        strstr(phrase, "想听歌") || strstr(phrase, "来点音乐") || strstr(phrase, "随便放一") ||
        strstr(phrase, "放一首") || strstr(phrase, "播一首")) {
        ESP_LOGI(TAG, "intent open → 曲库 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("曲库");
    }
    /* 电台 / 播客 */
    if (strstr(phrase, "听电台") || strstr(phrase, "放电台") || strstr(phrase, "开电台") ||
        (strstr(phrase, "电台") && (strstr(phrase, "听") || strstr(phrase, "放") || strstr(phrase, "播")))) {
        ESP_LOGI(TAG, "intent open → 电台 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("电台");
    }
    if (strstr(phrase, "听播客") || strstr(phrase, "放播客") || strstr(phrase, "开播客")) {
        ESP_LOGI(TAG, "intent open → 播客 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("播客");
    }
    /* 资讯类 */
    if (strstr(phrase, "看新闻") || strstr(phrase, "听新闻") || strstr(phrase, "今日新闻") ||
        strstr(phrase, "最新新闻")) {
        ESP_LOGI(TAG, "intent open → 新闻 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("新闻");
    }
    if (strstr(phrase, "看天气") || strstr(phrase, "今天天气") || strstr(phrase, "天气预报") ||
        (strstr(phrase, "天气") && (strstr(phrase, "怎么样") || strstr(phrase, "如何")))) {
        ESP_LOGI(TAG, "intent open → 天气 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("天气");
    }
    if (strstr(phrase, "空气质量") || strstr(phrase, "看空气")) {
        ESP_LOGI(TAG, "intent open → 空气 phrase='%s'", phrase);
        return nodelio_panel_ui_open_app("空气");
    }
    return false;
}

/** 整句就是应用短名（「电台。」），无「打开」前缀也走 open_app。 */
static bool try_open_bare_known_app(const char *phrase)
{
    if (!phrase || !phrase[0]) {
        return false;
    }
    char name[48];
    strip_app_query(name, sizeof(name), phrase);
    if (!name[0] || !app_query_plausible(name)) {
        return false;
    }
    static const char* const kKnown[] = {
        "电台",     "播客",     "曲库",     "取库",     "去库",     "趣库",     "音乐",
        "新闻",     "面板新闻", "新闻面板",
        "最新新闻", "财经新闻", "财新闻", /* 「看新闻」的常见 ASR 残句；只匹配整个短句。 */
        "天气",     "空气",     "一句",     "名言",     "网络电台", "radio",    "podcast",
        "music",    "news",     "weather",  "DSH",      "dsh",      "DSH任务",  "DSH进度",
        "查看任务", "任务情况", "任务列表", "任务详情", "任务应用", "任务表情", /* 表情=情况 ASR
                                                                                   误识 */
        "派活",
    };
    bool hit = false;
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); i++) {
        if (strcmp(name, kKnown[i]) == 0) {
            hit = true;
            break;
        }
    }
    /* 「帮我查看一下任务情况 / 任务详情 / 任务应用」等；「任务表情」当情况误识 */
    if (!hit) {
        if (strstr(name, "查看任务") || strstr(name, "任务情况") || strstr(name, "任务列表") ||
            strstr(name, "任务详情") || strstr(name, "任务应用") || strstr(name, "任务表情") ||
            strstr(phrase, "查看任务") || strstr(phrase, "任务情况") || strstr(phrase, "任务列表") ||
            strstr(phrase, "任务详情") || strstr(phrase, "任务应用") || strstr(phrase, "任务表情") ||
            strstr(phrase, "DSH") || strstr(phrase, "dsh")) {
            if (strstr(phrase, "派活") == NULL && strstr(name, "派活") == NULL) {
                hit = true;
                snprintf(name, sizeof(name), "%s", "查看任务");
            }
        }
    }
    if (!hit) {
        ESP_LOGI(TAG, "bare open miss name='%s' phrase='%s'", name, phrase);
        return false;
    }
    ESP_LOGI(TAG, "bare open app query='%s' from phrase='%s'", name, phrase);
    return nodelio_panel_ui_open_app(name);
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

bool nodelio_offline_try_system_phrase(const char *phrase, bool *end_session, bool *keep_cloud_tts)
{
    if (end_session) {
        *end_session = false;
    }
    if (keep_cloud_tts) {
        *keep_cloud_tts = false;
    }
    if (!phrase || !phrase[0]) {
        ESP_LOGI(TAG, "system phrase: empty");
        return false;
    }
    if (nodelio_offline_try_dsh_phrase(phrase)) {
        return true;
    }
    /* 新闻卡前台：读/续读走云 detect 播原文；翻篇本地切卡。未开卡不吞句 */
    if (nodelio_skill_news_ui_active()) {
        char news_phrase[96];
        strip_app_query(news_phrase, sizeof(news_phrase), phrase);
        const bool decline = strstr(news_phrase, "不要读") || strstr(news_phrase, "不用读") ||
                             strstr(news_phrase, "别读") || strstr(news_phrase, "不读") ||
                             strstr(news_phrase, "不朗读") || strstr(news_phrase, "不要朗读") ||
                             strstr(news_phrase, "不用朗读");
        if (decline) {
            return false;
        }
        const char *news_op = NULL;
        if (strstr(phrase, "继续读") || strstr(phrase, "接着念") || strstr(phrase, "往下读") ||
            strstr(phrase, "继续念") || strstr(phrase, "接着读")) {
            news_op = "continue";
        } else if (strstr(phrase, "下一篇") || strstr(phrase, "下一条新闻") || strstr(phrase, "下篇")) {
            news_op = "next";
        } else if (strstr(phrase, "上一篇") || strstr(phrase, "上一条新闻") || strstr(phrase, "上篇")) {
            news_op = "prev";
        } else if (strstr(phrase, "读这篇") || strstr(phrase, "朗读") || strstr(phrase, "念一下") ||
                   strstr(phrase, "读一下") || strstr(phrase, "念这篇") || strstr(phrase, "读吧") ||
                   strstr(phrase, "开始读") || strstr(phrase, "要读") ||
                   strcmp(news_phrase, "读") == 0) {
            news_op = "read";
        }
        if (news_op) {
            ESP_LOGI(TAG, "system phrase → news %s", news_op);
            if (strcmp(news_op, "read") == 0 || strcmp(news_op, "continue") == 0) {
                nodelio_panel_ui_news_read_local(news_op);
                if (keep_cloud_tts) {
                    *keep_cloud_tts = true; /* 云端 skill.news 长念，勿 Suppress */
                }
                if (end_session) {
                    *end_session = false;
                }
                return true;
            }
            char out[256];
            out[0] = '\0';
            if (nodelio_skill_news_action(news_op, out, sizeof(out))) {
                if (strcmp(news_op, "next") == 0) {
                    nodelio_panel_ui_set_voice_hint("下一篇");
                } else if (strcmp(news_op, "prev") == 0) {
                    nodelio_panel_ui_set_voice_hint("上一篇");
                } else if (out[0]) {
                    nodelio_panel_ui_set_voice_hint(out);
                }
                if (end_session) {
                    *end_session = false;
                }
                return true;
            }
            nodelio_panel_ui_set_voice_hint(out[0] ? out : "没有更多新闻");
            if (end_session) {
                *end_session = false;
            }
            return true;
        }
    }
    int cmd = phrase_to_cmd_id(phrase);
    ESP_LOGI(TAG, "system phrase try phrase='%s' cmd=%d(%s)", phrase, cmd,
             nodelio_offline_cmd_zh(cmd));
    if (cmd == NL_OC_REBOOT || cmd == NL_OC_WIFI_CONFIG ||
        (cmd >= NL_OC_GOTO_HOME && cmd <= NL_OC_GOTO_SYS) ||
        (cmd >= NL_OC_POWER_SAVE_ON && cmd <= NL_OC_GOTO_RESET) ||
        cmd == NL_OC_PAGE_NEXT || cmd == NL_OC_PAGE_PREV ||
        cmd == NL_OC_SCROLL_DOWN || cmd == NL_OC_SCROLL_UP ||
        cmd == NL_OC_MEDIA_PREV || cmd == NL_OC_MEDIA_NEXT ||
        cmd == NL_OC_MEDIA_PAUSE || cmd == NL_OC_MEDIA_RESUME ||
        cmd == NL_OC_SKILL_CLOSE || cmd == NL_OC_RADIO_SWAP ||
        cmd == NL_OC_APP_QUOTE || cmd == NL_OC_APP_WEATHER || cmd == NL_OC_APP_AIR ||
        (cmd >= NL_OC_BRIGHT_UP && cmd <= NL_OC_VOL_MIN)) {
        ESP_LOGI(TAG, "system phrase → %s", nodelio_offline_cmd_zh(cmd));
        if (end_session) {
            /* 默认保持会话：设置导航/开关/翻页可连说。
             * 切歌/继续播须退会话，否则 Listening 与 HTTP 播抢通道（re-arm 与 notifying 打架）。
             * 换台：已在播电台则只回列表；否则 open 电台须退会话。 */
            if (cmd == NL_OC_RADIO_SWAP) {
                *end_session = !nodelio_skill_is_live_radio();
            } else {
                *end_session =
                    (cmd == NL_OC_REBOOT || cmd == NL_OC_WIFI_CONFIG || cmd == NL_OC_SKILL_CLOSE ||
                     cmd == NL_OC_MEDIA_PREV || cmd == NL_OC_MEDIA_NEXT || cmd == NL_OC_MEDIA_RESUME);
            }
        }
        return nodelio_offline_cmd_dispatch(NULL, cmd, NULL) == ESP_OK;
    }
    /* 「打开曲库」等：phrase_to_cmd_id 会落到 LIGHT_ON，这里再抢一次应用 */
    if (try_open_named_app(phrase)) {
        const bool keep_cloud = nodelio_offline_query_keeps_cloud_tts(phrase);
        const bool keep_listen = nodelio_offline_query_keeps_listening(phrase);
        ESP_LOGI(TAG, "system phrase: named open hit keep_cloud=%d keep_listen=%d", keep_cloud ? 1 : 0,
                 keep_listen ? 1 : 0);
        /* 媒体须退会话才能 HTTP 播；天气问内容 keep_cloud；新闻 keep_listen 追问要不要读 */
        if (end_session) {
            *end_session = !keep_listen;
        }
        if (keep_cloud_tts) {
            *keep_cloud_tts = keep_cloud;
        }
        if (keep_listen && !keep_cloud &&
            (strstr(phrase, "新闻") != NULL || strstr(phrase, "news") != NULL)) {
            nodelio_panel_ui_arm_news_read_offer();
        }
        return true;
    }
    /* 「听歌/看天气」等：抢在云端 search_music 之前 */
    if (try_open_intent_app(phrase)) {
        const bool keep_cloud = nodelio_offline_query_keeps_cloud_tts(phrase);
        const bool keep_listen = nodelio_offline_query_keeps_listening(phrase);
        ESP_LOGI(TAG, "system phrase: intent open hit keep_cloud=%d keep_listen=%d",
                 keep_cloud ? 1 : 0, keep_listen ? 1 : 0);
        if (end_session) {
            *end_session = !keep_listen;
        }
        if (keep_cloud_tts) {
            *keep_cloud_tts = keep_cloud;
        }
        if (keep_listen && !keep_cloud &&
            (strstr(phrase, "新闻") != NULL || strstr(phrase, "news") != NULL)) {
            nodelio_panel_ui_arm_news_read_offer();
        }
        return true;
    }
    /* 裸说「电台。」/「播客」：无「打开」也开对应应用（剥句末标点） */
    if (try_open_bare_known_app(phrase)) {
        const bool keep_cloud = nodelio_offline_query_keeps_cloud_tts(phrase);
        const bool keep_listen = nodelio_offline_query_keeps_listening(phrase);
        ESP_LOGI(TAG, "system phrase: bare open hit keep_cloud=%d keep_listen=%d", keep_cloud ? 1 : 0,
                 keep_listen ? 1 : 0);
        if (end_session) {
            *end_session = !keep_listen;
        }
        if (keep_cloud_tts) {
            *keep_cloud_tts = keep_cloud;
        }
        if (keep_listen && !keep_cloud &&
            (strstr(phrase, "新闻") != NULL || strstr(phrase, "news") != NULL)) {
            nodelio_panel_ui_arm_news_read_offer();
        }
        return true;
    }
    ESP_LOGI(TAG, "system phrase: no local hit");
    return false;
}
