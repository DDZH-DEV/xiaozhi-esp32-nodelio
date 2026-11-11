#include "nodelio_mcp.h"
#include "application.h"
#include "workbench.h"

#include "content_envelope.h"
#include "device_caps.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "overlay_priv.h"
#include "media_player.h"
#include "skill_result_ui.h"
#include "skill_runtime.h"

#include "mcp_server.h"
#include "mcp_payload_budget.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <unordered_set>
#include <vector>

#define TAG "nl_mcp"

/** 只读 RAM 镜像。MCP 技能任务栈在 PSRAM，禁止 nodelio_nvs_load（关 cache 读 flash 会崩）。 */
static bool load_cfg(nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    nodelio_nvs_cache_get(cfg);
    return cfg->server_url[0] != '\0' && cfg->device_id[0] != '\0' && cfg->device_key[0] != '\0';
}

static const nl_widget_t *find_widget_by_title(const nodelio_panel_layout_t *layout, const char *title)
{
    if (!layout || !title || !title[0]) {
        return nullptr;
    }
    // 1. 优先完全一致匹配
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if ((w->title[0] && strcmp(w->title, title) == 0) ||
                    (w->bind.name[0] && strcmp(w->bind.name, title) == 0) ||
                    (w->id[0] && strcmp(w->id, title) == 0)) {
                    return w;
                }
            }
        }
    }
    // 2. 双向子串模糊匹配（如“主卧”匹配“主卧筒灯”或“主卧灯”匹配“主卧”）
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->title[0] && (strstr(w->title, title) || strstr(title, w->title))) {
                    return w;
                }
                if (w->bind.name[0] && (strstr(w->bind.name, title) || strstr(title, w->bind.name))) {
                    return w;
                }
            }
        }
    }
    return nullptr;
}

struct UnassignedTarget {
    std::string device_id;
    int channel = -1;
    std::string title;
};

static bool find_unassigned_target(const char *title, UnassignedTarget &out)
{
    if (!title || !title[0]) {
        return false;
    }
    int dev_cnt = nodelio_panel_ui_get_dev_count();
    // 1. 完全匹配
    for (int i = 0; i < dev_cnt; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->id[0]) {
            continue;
        }
        if (d->ch_n > 0) {
            for (int ci = 0; ci < d->ch_n; ci++) {
                if (d->ch_name[ci][0] && strcmp(d->ch_name[ci], title) == 0) {
                    out.device_id = d->id;
                    out.channel = d->ch_idx[ci];
                    out.title = d->ch_name[ci];
                    return true;
                }
            }
        } else {
            if ((d->name[0] && strcmp(d->name, title) == 0) || strcmp(d->id, title) == 0) {
                out.device_id = d->id;
                out.channel = 0;
                out.title = d->name[0] ? d->name : d->id;
                return true;
            }
        }
    }
    // 2. 双向模糊包含匹配
    for (int i = 0; i < dev_cnt; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->id[0]) {
            continue;
        }
        if (d->ch_n > 0) {
            for (int ci = 0; ci < d->ch_n; ci++) {
                if (d->ch_name[ci][0] && (strstr(d->ch_name[ci], title) || strstr(title, d->ch_name[ci]))) {
                    out.device_id = d->id;
                    out.channel = d->ch_idx[ci];
                    out.title = d->ch_name[ci];
                    return true;
                }
            }
        } else {
            if (d->name[0] && (strstr(d->name, title) || strstr(title, d->name))) {
                out.device_id = d->id;
                out.channel = 0;
                out.title = d->name;
                return true;
            }
        }
    }
    return false;
}

/** 从「打开继电器」「把客厅灯关掉」里抽出标题关键字 */
static void extract_title_hint(const char *phrase, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!phrase || !phrase[0]) {
        return;
    }
    const char *p = phrase;
    const char *prefixes[] = {"请帮我", "帮我", "请", "把", "将", "打开", "开启", "关掉", "关闭",
                              "关上", "开一下", "关一下", nullptr};
    for (int i = 0; prefixes[i]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(p, prefixes[i], n) == 0) {
            p += n;
        }
    }
    /* UTF-8 标点不能写进 '…'（多字节 → -Werror=multichar） */
    auto starts_utf8 = [](const char *s, const char *tok) -> bool {
        size_t n = strlen(tok);
        return n > 0 && strncmp(s, tok, n) == 0;
    };
    while (*p == ' ' || *p == ',' || *p == '.' || starts_utf8(p, "，") || starts_utf8(p, "。")) {
        if (*p == ' ' || *p == ',' || *p == '.') {
            p++;
        } else if (starts_utf8(p, "，")) {
            p += strlen("，");
        } else {
            p += strlen("。");
        }
    }
    snprintf(out, out_len, "%s", p);
    size_t len = strlen(out);
    auto ends_utf8 = [](const char *s, size_t slen, const char *tok) -> bool {
        size_t n = strlen(tok);
        return slen >= n && strcmp(s + slen - n, tok) == 0;
    };
    while (len > 0) {
        if (out[len - 1] == '.' || out[len - 1] == '!' || out[len - 1] == ' ') {
            out[--len] = '\0';
            continue;
        }
        if (ends_utf8(out, len, "。") || ends_utf8(out, len, "！")) {
            size_t n = ends_utf8(out, len, "。") ? strlen("。") : strlen("！");
            len -= n;
            out[len] = '\0';
            continue;
        }
        break;
    }
    /* 去掉尾部「打开/关掉」等 */
    const char *suffixes[] = {"打开", "开启", "关掉", "关闭", "关上", nullptr};
    for (int i = 0; suffixes[i]; i++) {
        size_t n = strlen(suffixes[i]);
        if (len > n && strcmp(out + len - n, suffixes[i]) == 0) {
            out[len - n] = '\0';
            len -= n;
        }
    }
}

static bool phrase_implies_on(const char *phrase)
{
    if (!phrase) {
        return true;
    }
    if (strstr(phrase, "关掉") || strstr(phrase, "关闭") || strstr(phrase, "关上") ||
        strstr(phrase, "关灯") || strstr(phrase, "关继电器") || strstr(phrase, "关插座")) {
        return false;
    }
    if (strstr(phrase, "关") && !strstr(phrase, "开关") && !strstr(phrase, "打开") &&
        !strstr(phrase, "开启")) {
        return false;
    }
    return true;
}

static bool bind_target_offline(const nl_bind_t *bind)
{
    if (!bind || bind->kind == NL_BIND_HOME_SCENARIO || bind->kind == NL_BIND_ACTION_BUTTON ||
        !bind->device_id[0]) {
        return false;
    }
    return !nodelio_panel_ui_device_online(bind->device_id);
}

static ToolResult offline_result(const char *name)
{
    std::string msg = std::string("设备离线，无法控制");
    if (name && name[0]) {
        msg += "：";
        msg += name;
    }
    msg += "（请确认该设备已联网上线）";
    return std::unexpected(msg);
}

static bool title_worth_listing(const char *title, const char *device_id);

/** sila-86 短包上限约 1024；正文压到 900，留给 JSON-RPC 外壳。 */
static constexpr size_t kStatusReplyMax = 900;
static constexpr int kStatusOnMax = 12;

static const nl_dev_cache_t *mcp_find_dev(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return nullptr;
    }
    const int n = nodelio_panel_ui_get_dev_count();
    for (int i = 0; i < n; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (d && d->id[0] && strcmp(d->id, device_id) == 0) {
            return d;
        }
    }
    return nullptr;
}

static bool status_try_append(std::string &out, const char *name, const char *label_zh)
{
    if (!name || !name[0] || !label_zh || !label_zh[0]) {
        return false;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s %s", name, label_zh);
    const size_t need = (out.empty() ? 0 : 1) + std::strlen(line);
    if (out.size() + need > kStatusReplyMax) {
        return false;
    }
    if (!out.empty()) {
        out.push_back('\n');
    }
    out += line;
    return true;
}

/** 点名：该芯片各通道；无通道行则整机 power。 */
static std::string status_for_device(const nl_dev_cache_t *d, const char *fallback_title)
{
    if (!d) {
        return std::string("未找到设备状态");
    }
    const char *head = (fallback_title && fallback_title[0])
                            ? fallback_title
                            : (d->name[0] ? d->name : d->id);
    if (!d->online) {
        std::string msg = std::string(head) + " 离线";
        if (msg.size() > kStatusReplyMax) {
            msg.resize(kStatusReplyMax);
        }
        return msg;
    }
    std::string out;
    if (d->ch_n > 0) {
        for (uint8_t i = 0; i < d->ch_n; i++) {
            char ch_fallback[24];
            const char *nm = d->ch_name[i][0] ? d->ch_name[i] : nullptr;
            if (!nm) {
                std::snprintf(ch_fallback, sizeof(ch_fallback), "通道%d", (int)d->ch_idx[i]);
                nm = ch_fallback;
            }
            if (!status_try_append(out, nm, nl_ov_state_label_zh(d->ch_st[i]))) {
                break;
            }
        }
    } else {
        status_try_append(out, head, nl_ov_state_label_zh(d->power));
    }
    if (out.empty()) {
        return std::string("没有可读状态");
    }
    return out;
}

/** action=STATUS：短中文，不发控家命令。 */
static ToolResult control_status(const nodelio_panel_layout_t *layout, const std::string &title,
                                 const std::string &phrase)
{
    char hint[48] = {0};
    const char *name = nullptr;
    if (!title.empty()) {
        name = title.c_str();
    } else if (!phrase.empty()) {
        extract_title_hint(phrase.c_str(), hint, sizeof(hint));
        if (hint[0]) {
            name = hint;
        }
    }

    if (name && name[0]) {
        if (layout) {
            const nl_widget_t *w = find_widget_by_title(layout, name);
            if (w && w->bind.kind == NL_BIND_ACTION_BUTTON) {
                return std::string(w->title[0] ? w->title : (w->bind.name[0] ? w->bind.name : name)) +
                       " 是组合按钮，没有开关态";
            }
            if (w && w->bind.kind != NL_BIND_HOME_SCENARIO && w->bind.device_id[0]) {
                const char *shown = w->title[0] ? w->title : w->bind.name;
                const nl_dev_cache_t *d = mcp_find_dev(w->bind.device_id);
                if (!d) {
                    return std::string(shown[0] ? shown : name) + " 未知";
                }
                return status_for_device(d, shown[0] ? shown : name);
            }
        }
        UnassignedTarget u;
        if (find_unassigned_target(name, u)) {
            const nl_dev_cache_t *d = mcp_find_dev(u.device_id.c_str());
            if (!d) {
                return u.title + " 未知";
            }
            return status_for_device(d, u.title.c_str());
        }
        return std::unexpected(std::string("未找到设备，请用 list_controls 确认名称"));
    }

    /* 不点名：只列还开着的，关着的不写。 */
    std::string out;
    int listed = 0;
    int more = 0;
    std::unordered_set<std::string> seen;

    auto consider = [&](const char *nm, const char *device_id, const char *st) {
        if (!nm || !nm[0]) {
            return;
        }
        if (device_id && device_id[0] && !nodelio_panel_ui_device_online(device_id)) {
            return;
        }
        if (strcmp(nl_ov_state_label_zh(st), "开") != 0) {
            return;
        }
        std::string key(nm);
        if (seen.count(key)) {
            return;
        }
        if (listed >= kStatusOnMax) {
            more++;
            return;
        }
        if (!status_try_append(out, nm, "开")) {
            more++;
            return;
        }
        seen.insert(std::move(key));
        listed++;
    };

    std::unordered_set<std::string> bound_keys;
    if (layout) {
        for (int pi = 0; pi < layout->page_count; pi++) {
            const nl_page_t *pg = &layout->pages[pi];
            for (int si = 0; si < pg->section_count; si++) {
                const nl_section_t *sec = &pg->sections[si];
                for (int wi = 0; wi < sec->widget_count; wi++) {
                    const nl_widget_t *w = &sec->widgets[wi];
                    if (w->bind.kind == NL_BIND_NONE || w->bind.kind == NL_BIND_HOME_SCENARIO ||
                        w->bind.kind == NL_BIND_ACTION_BUTTON) {
                        continue;
                    }
                    if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER) {
                        continue;
                    }
                    const char *nm = w->title[0] ? w->title : w->bind.name;
                    if (!title_worth_listing(nm, w->bind.device_id)) {
                        continue;
                    }
                    if (w->bind.device_id[0]) {
                        if (w->bind.channel >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":" +
                                              std::to_string(w->bind.channel));
                        } else if (w->bind.pin >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":pin" +
                                              std::to_string(w->bind.pin));
                        } else {
                            bound_keys.insert(std::string(w->bind.device_id));
                        }
                    }
                    consider(nm, w->bind.device_id, nl_ov_widget_cache_state(w));
                }
            }
        }
    }

    const int dev_cnt = nodelio_panel_ui_get_dev_count();
    for (int i = 0; i < dev_cnt; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->id[0] || strncmp(d->id, "WP-", 3) == 0) {
            continue;
        }
        if (d->ch_n > 0) {
            for (int ci = 0; ci < d->ch_n; ci++) {
                std::string key = std::string(d->id) + ":" + std::to_string(d->ch_idx[ci]);
                if (bound_keys.count(key)) {
                    continue;
                }
                const char *ch_name = d->ch_name[ci];
                if (!ch_name[0] || !title_worth_listing(ch_name, d->id)) {
                    continue;
                }
                consider(ch_name, d->id, d->ch_st[ci]);
            }
        } else {
            if (bound_keys.count(d->id)) {
                continue;
            }
            const char *nm = d->name[0] ? d->name : nullptr;
            if (!nm || !title_worth_listing(nm, d->id) || strstr(nm, d->id)) {
                continue;
            }
            consider(nm, d->id, d->power);
        }
    }

    if (out.empty()) {
        return std::string("现在没有开着的设备");
    }
    if (more > 0) {
        char tail[40];
        std::snprintf(tail, sizeof(tail), "\n还有%d个开着", more);
        if (out.size() + std::strlen(tail) <= kStatusReplyMax) {
            out += tail;
        }
    }
    return out;
}

static ToolResult run_widget_set(const nodelio_nvs_config_t *cfg, const nl_widget_t *w, const char *val)
{
    if (!w) {
        return std::unexpected(std::string("布局中未找到控件"));
    }
    const char *name = w->title[0] ? w->title : w->bind.name;
    if (bind_target_offline(&w->bind)) {
        return offline_result(name[0] ? name : w->id);
    }
    if (!val) {
        if (w->bind.kind == NL_BIND_HOME_SCENARIO || w->bind.kind == NL_BIND_ACTION_BUTTON) {
            val = w->bind.name;
        } else {
            val = "TOGGLE";
        }
    }
    int http = 0;
    esp_err_t err = nodelio_panel_command(cfg, w->id, "set", &w->bind, val, &http);
    if (err != ESP_OK) {
        if (http == 503) {
            return offline_result(name[0] ? name : w->id);
        }
        return std::unexpected(std::string("执行失败"));
    }
    std::string ok = std::string("已控制：") + (name[0] ? name : w->id);
    nodelio_panel_ui_toast(ok.c_str());
    nodelio_panel_ui_set_voice_hint(ok.c_str());
    return ok;
}

static ToolResult run_unassigned_set(const nodelio_nvs_config_t *cfg, const UnassignedTarget &t, const char *val)
{
    if (!t.device_id.empty() && !nodelio_panel_ui_device_online(t.device_id.c_str())) {
        return offline_result(t.title.c_str());
    }
    nl_bind_t bind{};
    bind.kind = NL_BIND_DEVICE_CHANNEL;
    std::strncpy(bind.device_id, t.device_id.c_str(), sizeof(bind.device_id) - 1);
    bind.channel = t.channel >= 0 ? t.channel : 0;
    if (!val || !val[0]) {
        val = "TOGGLE";
    }
    int http = 0;
    esp_err_t err = nodelio_panel_command(cfg, "unassigned", "set", &bind, val, &http);
    if (err != ESP_OK) {
        if (http == 503) {
            return offline_result(t.title.c_str());
        }
        return std::unexpected(std::string("执行失败"));
    }
    std::string ok = std::string("已控制：") + t.title;
    nodelio_panel_ui_toast(ok.c_str());
    nodelio_panel_ui_set_voice_hint(ok.c_str());
    return ok;
}

/** 无名 Channel N / 墙板自身：语音列表里噪音大、易撑爆 MQTT */
static bool title_worth_listing(const char *title, const char *device_id)
{
    if (!title || !title[0]) {
        return false;
    }
    if (device_id && (strncmp(device_id, "WP-", 3) == 0)) {
        return false;
    }
    if (strncasecmp(title, "Channel", 7) == 0) {
        return false;
    }
    if (strncmp(title, "通道", 6) == 0) { /* UTF-8 通道 */
        return false;
    }
    return true;
}

/**
 * 语音用精简列表：MQTT MCP 约 8KB 上限。
 * 只含首页排布控件（按 title 去重）+ 少量已命名未排布；不含 actions/device_id/id。
 */
static const char *kControlsOff =
    "控家模块已关闭。请告诉用户到墙板「设置 → 控家」打开「允许控家模块」，不要假装已经开灯或执行成功。";

static const char *kControlsEmpty =
    "当前没有可控制的设备或情景。请告诉用户：可在手机 APP 绑定继电器/开关并排布到墙板，"
    "或打开「面板应用」安装市场应用；不要假装已经开灯或执行成功。";

static std::string list_controls_json()
{
    if (!nodelio_controls_enabled()) {
        return std::string(kControlsOff);
    }
    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return std::string(kControlsEmpty);
    }

    std::unordered_set<std::string> seen_title;
    std::unordered_set<std::string> bound_keys;
    constexpr int kMaxItems = 40;
    int n = 0;

    auto add_item = [&](const char *title, const char *room, const char *kind,
                        const char *device_id) -> bool {
        if (n >= kMaxItems || !title || !title[0]) {
            return false;
        }
        std::string tkey(title);
        if (seen_title.count(tkey)) {
            return false;
        }
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            return false;
        }
        cJSON_AddStringToObject(o, "title", title);
        if (room && room[0] && strcmp(room, "首页") != 0) {
            cJSON_AddStringToObject(o, "room", room);
        }
        if (kind && kind[0] && strcmp(kind, "switch") != 0) {
            cJSON_AddStringToObject(o, "kind", kind);
        }
        /* 仅离线带 online:false，省 MQTT 字节；缺省=在线/未知 */
        if (device_id && device_id[0] && !nodelio_panel_ui_device_online(device_id)) {
            cJSON_AddBoolToObject(o, "online", false);
        }
        cJSON_AddItemToArray(arr, o);
        seen_title.insert(std::move(tkey));
        n++;
        return true;
    };

    if (layout) {
        for (int pi = 0; pi < layout->page_count && n < kMaxItems; pi++) {
            const nl_page_t *pg = &layout->pages[pi];
            for (int si = 0; si < pg->section_count && n < kMaxItems; si++) {
                const nl_section_t *sec = &pg->sections[si];
                for (int wi = 0; wi < sec->widget_count && n < kMaxItems; wi++) {
                    const nl_widget_t *w = &sec->widgets[wi];
                    if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER ||
                        w->bind.kind == NL_BIND_NONE) {
                        continue;
                    }
                    const char *title = w->title[0] ? w->title : w->bind.name;
                    if (!title_worth_listing(title, w->bind.device_id)) {
                        continue;
                    }
                    if (w->bind.device_id[0]) {
                        if (w->bind.channel >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":" +
                                              std::to_string(w->bind.channel));
                        } else if (w->bind.pin >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":pin" +
                                              std::to_string(w->bind.pin));
                        } else {
                            bound_keys.insert(std::string(w->bind.device_id));
                        }
                    }
                    const char *kind = "switch";
                    if (w->bind.kind == NL_BIND_HOME_SCENARIO) {
                        kind = "scene";
                    } else if (w->bind.kind == NL_BIND_ACTION_BUTTON) {
                        kind = "action";
                    } else if (w->type == NL_WIDGET_SLIDER_TILE ||
                               strcmp(w->bind.capability, "pwm") == 0) {
                        kind = "pwm";
                    }
                    const char *room = sec->title[0] ? sec->title : pg->title;
                    const char *dev =
                        (w->bind.kind == NL_BIND_HOME_SCENARIO ||
                         w->bind.kind == NL_BIND_ACTION_BUTTON)
                            ? nullptr
                            : w->bind.device_id;
                    add_item(title, room, kind, dev);
                }
            }
        }
    }

    /* 未排布：仅已命名通道/设备，且跳过墙板、Channel N */
    int dev_cnt = nodelio_panel_ui_get_dev_count();
    for (int i = 0; i < dev_cnt && n < kMaxItems; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->id[0] || strncmp(d->id, "WP-", 3) == 0) {
            continue;
        }
        if (d->ch_n > 0) {
            for (int ci = 0; ci < d->ch_n && n < kMaxItems; ci++) {
                int ch_idx = d->ch_idx[ci];
                std::string key = std::string(d->id) + ":" + std::to_string(ch_idx);
                if (bound_keys.count(key)) {
                    continue;
                }
                const char *ch_name = d->ch_name[ci];
                if (!ch_name[0] || !title_worth_listing(ch_name, d->id)) {
                    continue;
                }
                add_item(ch_name, "未排布", "switch", d->id);
            }
        } else {
            if (bound_keys.count(d->id)) {
                continue;
            }
            const char *name = d->name[0] ? d->name : nullptr;
            if (!name || !title_worth_listing(name, d->id)) {
                continue;
            }
            /* 名称里带设备 id 后缀的默认名也跳过 */
            if (strstr(name, d->id)) {
                continue;
            }
            add_item(name, "未排布", "switch", d->id);
        }
    }

    if (n == 0) {
        cJSON_Delete(arr);
        ESP_LOGI(TAG, "list_controls empty (no bindings)");
        return std::string(kControlsEmpty);
    }

    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string out = printed ? printed : std::string(kControlsEmpty);
    free(printed);

    /* 硬顶：MQTT MCP 常见 ~8KB；留余量给 jsonrpc 外壳。
     * 八六云端回话时按当前可发线长再收一档，避免组好后被堆预算丢掉。 */
    size_t max_bytes = 3500;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    {
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const size_t wire = McpPayloadBudget::SendableWire(largest);
        constexpr size_t kMqttWrap = 320;
        if (wire <= kMqttWrap) {
            max_bytes = 0;
        } else if (wire - kMqttWrap < max_bytes) {
            max_bytes = wire - kMqttWrap;
        }
    }
#endif
    if (out.size() > max_bytes) {
        ESP_LOGW(TAG, "list_controls truncated %u→%u bytes (n=%d)", (unsigned)out.size(),
                 (unsigned)max_bytes, n);
        /* 粗暴截到合法 JSON 数组：找到最后一个完整对象 */
        size_t cut = out.rfind("},", max_bytes);
        if (cut != std::string::npos && cut + 1 < out.size()) {
            out.resize(cut + 1);
            out.push_back(']');
        } else {
            out = std::string(kControlsEmpty);
        }
    }
    ESP_LOGI(TAG, "list_controls n≈%d bytes=%u", n, (unsigned)out.size());
    return out;
}

/** 「播放第一首 / 放第三首 / 来第2个」→ 1-based 序号；不是选条返回 0。
 *  与 parse_ordinal_zh 不同，这里只认「在已有列表里选一条」的说法。 */
static int music_keyword_ordinal(const char *kw)
{
    if (!kw || !kw[0]) {
        return 0;
    }
    /* 「第」必须紧跟在播放动词之后或句首，避免把歌名里的「第一」当成序号 */
    const char *p = strstr(kw, "第");
    if (!p) {
        /* 「放一首 / 来首 / 播放第一个」等 ASR 变体 */
        if (strstr(kw, "第一个") || strstr(kw, "首个") || strstr(kw, "头一首")) {
            return 1;
        }
        return 0;
    }
    /* 序号后须是「首/曲/个/条」，否则是歌名（如「第一次爱的人」） */
    const char *q = p + strlen("第");
    int n = 0;
    if (*q >= '1' && *q <= '9') {
        n = *q - '0';
        q++;
        if (*q >= '0' && *q <= '9') {
            n = n * 10 + (*q - '0');
            q++;
        }
    } else {
        static const char *const kCn[] = {"一", "二", "两", "三", "四", "五",
                                          "六", "七", "八", "九", "十"};
        static const int kVal[] = {1, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        for (size_t i = 0; i < sizeof(kCn) / sizeof(kCn[0]); i++) {
            if (strncmp(q, kCn[i], strlen(kCn[i])) == 0) {
                n = kVal[i];
                q += strlen(kCn[i]);
                break;
            }
        }
    }
    if (n <= 0) {
        return 0;
    }
    return (strncmp(q, "首", strlen("首")) == 0 || strncmp(q, "曲", strlen("曲")) == 0 ||
            strncmp(q, "个", strlen("个")) == 0 || strncmp(q, "条", strlen("条")) == 0)
               ? n
               : 0;
}

/** 曲库 keyword：打开别名 → 空词点播；浏览口令保持原样。 */
static bool music_keyword_is_browse(const char *kw)
{
    if (!kw || !kw[0]) {
        return false;
    }
    return strstr(kw, "有什么") || strstr(kw, "什么歌") || strstr(kw, "列表") || strstr(kw, "全部") ||
           strstr(kw, "歌单") || strstr(kw, "换一批") || strstr(kw, "再来一批") || strstr(kw, "看看") ||
           strstr(kw, "试听") || strstr(kw, "测试") || strstr(kw, "本地");
}

static bool music_keyword_is_open_alias(const char *kw)
{
    if (!kw || !kw[0]) {
        return true; /* 空 = 打开曲库点播 */
    }
    if (music_keyword_is_browse(kw)) {
        return false;
    }
    /* 「打开音乐 / 曲库 / 放首歌」等：与服务端 isMusicPlayIntent 对齐 */
    if (strstr(kw, "曲库") || strstr(kw, "音乐") || strstr(kw, "放一") || strstr(kw, "来一") ||
        strstr(kw, "播一") || strstr(kw, "播放") || strstr(kw, "放歌") || strstr(kw, "听歌") ||
        strstr(kw, "点播") || strstr(kw, "开播") || strstr(kw, "随便放") || strstr(kw, "随便播") ||
        strstr(kw, "随便听") || strstr(kw, "随机") || strstr(kw, "来点歌") || strstr(kw, "放点歌") ||
        strstr(kw, "放首歌") || strstr(kw, "听首歌") || strstr(kw, "来首歌") || strcmp(kw, "随便") == 0) {
        return true;
    }
    return false;
}

static std::string music_normalize_invoke_body(const std::string &body_in, bool *force_play)
{
    if (force_play) {
        *force_play = false;
    }
    std::string body = body_in.empty() ? "{}" : body_in;
    if (body.empty() || body.front() != '{') {
        body = std::string("{\"keyword\":\"") + body_in + "\"}";
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (!root) {
        if (force_play) {
            *force_play = true; /* 无法解析时按空搜点播 */
        }
        return std::string("{\"keyword\":\"\"}");
    }
    const cJSON *kwj = cJSON_GetObjectItemCaseSensitive(root, "keyword");
    if (!kwj) {
        kwj = cJSON_GetObjectItemCaseSensitive(root, "query");
    }
    if (!kwj) {
        kwj = cJSON_GetObjectItemCaseSensitive(root, "q");
    }
    const char *kw = (cJSON_IsString(kwj) && kwj->valuestring) ? kwj->valuestring : "";
    if (music_keyword_is_open_alias(kw)) {
        if (force_play) {
            *force_play = true;
        }
        cJSON_Delete(root);
        if (kw[0]) {
            ESP_LOGI(TAG, "music open-alias keyword='%s' → empty+force", kw);
        }
        return std::string("{\"keyword\":\"\"}");
    }
    cJSON_Delete(root);
    return body;
}

/** LLM 常写短 id（radio）→ 市场真实 app_id（radio.stations）。保留 market:: 前缀。 */
static std::string normalize_skill_id_alias(const std::string &raw)
{
    if (raw.empty()) {
        return raw;
    }
    std::string prefix;
    std::string id = raw;
    const size_t sep = raw.rfind("::");
    if (sep != std::string::npos) {
        prefix = raw.substr(0, sep + 2);
        id = raw.substr(sep + 2);
    }
    std::string low = id;
    for (char &c : low) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const char *canon = nullptr;
    if (low == "radio" || low == "fm" || low == "netradio") {
        canon = "radio.stations";
    } else if (low == "podcast" || low == "pod") {
        canon = "podcast.latest";
    } else if (low == "music" || low == "music_search" || low == "library") {
        canon = "music.search";
    } else if (low == "news") {
        canon = "news.latest";
    } else if (low == "quote" || low == "daily" || low == "quote.daily") {
        canon = "article.daily";
    } else if (low == "dsh" || low == "dsh.status" || low == "dsh_status" || low == "tasks") {
        canon = "dsh.status";
    } else if (low == "codex" || low == "桌面工作台") {
        canon = "codex.status";
    } else if (low == "chatgpt" || low == "openai") {
        canon = "openai.status";
    } else if (low == "workbuddy") {
        canon = "workbuddy.status";
    } else if (low == "workbench") {
        canon = "workbench.status";
    } else if (low == "dsh.task" || low == "dsh_task") {
        canon = "dsh.task";
    }
    if (!canon) {
        return raw;
    }
    std::string out = prefix + canon;
    if (out != raw) {
        ESP_LOGI(TAG, "skill_id alias '%s' → '%s'", raw.c_str(), out.c_str());
    }
    return out;
}

static ToolResult invoke_skill(const nodelio_nvs_config_t* cfg, const std::string& skill_id_in,
                               const std::string& params_in) {
    std::string params = params_in;
    if (skill_id_in.empty()) {
        return std::unexpected(std::string("skill_id 不能为空"));
    }
    std::string skill_id = normalize_skill_id_alias(skill_id_in);
    bool weather_voice = false;
    {
        const char *bare = skill_id.c_str();
        const char *colon = strrchr(bare, ':');
        if (colon && colon > bare && colon[-1] == ':') {
            bare = colon + 1;
        }
        /* 天气/空气：走 builtin.daily invoke，返回 spoken 供小智播报；开卡放到拿到结果后再做。 */
        if (strcmp(bare, "weather.today") == 0 || strcmp(bare, "air.now") == 0 ||
            strcmp(bare, "builtin.daily") == 0) {
            weather_voice = true;
            skill_id = "builtin.daily";
        }
    }
    const bool dsh_app = nl_workbench_app(skill_id.c_str());
    if (dsh_app) {
        cJSON* body = cJSON_Parse(params.empty() ? "{}" : params.c_str());
        if (cJSON_IsObject(body) && !cJSON_HasObjectItem(body, "bridge_id") &&
            !cJSON_HasObjectItem(body, "computer") && !cJSON_HasObjectItem(body, "session") &&
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "select_computer"))) {
            char bridge[64] = {}, provider[16] = {};
            nodelio_panel_ui_workbench_context(skill_id.c_str(), bridge, sizeof(bridge), provider,
                                               sizeof(provider));
            if (bridge[0]) {
                bool valid = cJSON_AddStringToObject(body, "bridge_id", bridge) != nullptr;
                if (provider[0] && !cJSON_HasObjectItem(body, "provider"))
                    valid = valid && cJSON_AddStringToObject(body, "provider", provider);
                char* text = valid ? cJSON_PrintUnformatted(body) : nullptr;
                if (!text) {
                    cJSON_Delete(body);
                    return std::unexpected(std::string("内存不足，未发送"));
                }
                params = text;
                cJSON_free(text);
            }
        }
        cJSON_Delete(body);
    }
    /* 网站智能客服：kb_lookup 只读查 FAQ/KB，无需开卡；其它 op 须开卡，缺 op 时补 draft */
    if (strcmp(skill_id.c_str(), "support.inbox") == 0) {
        cJSON *body = cJSON_Parse(params.empty() ? "{}" : params.c_str());
        if (!cJSON_IsObject(body)) {
            cJSON_Delete(body);
            body = cJSON_CreateObject();
        }
        if (!body) {
            return std::unexpected(std::string("内存不足，未发送"));
        }
        const cJSON *jop = cJSON_GetObjectItemCaseSensitive(body, "op");
        const bool no_card_op =
            cJSON_IsString(jop) && jop->valuestring &&
            (strcmp(jop->valuestring, "kb_lookup") == 0 ||
             strcmp(jop->valuestring, "analytics_summary") == 0 ||
             strcmp(jop->valuestring, "site_stats") == 0);
        if (!no_card_op) {
            uint64_t cid = 0;
            if (!nodelio_skill_support_conversation_id(&cid) || cid == 0) {
                cJSON_Delete(body);
                return std::unexpected(std::string("请先打开网站智能客服卡片"));
            }
            if (!cJSON_HasObjectItem(body, "conversation_id")) {
                cJSON_AddNumberToObject(body, "conversation_id", (double)cid);
            }
            const cJSON *jtext = cJSON_GetObjectItemCaseSensitive(body, "text");
            if ((!cJSON_IsString(jop) || !jop->valuestring || !jop->valuestring[0]) &&
                cJSON_IsString(jtext) && jtext->valuestring && jtext->valuestring[0]) {
                cJSON_DeleteItemFromObject(body, "op");
                cJSON_AddStringToObject(body, "op", "draft");
            }
        }
        char *text = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (!text) {
            return std::unexpected(std::string("内存不足，未发送"));
        }
        params = text;
        cJSON_free(text);
    }
    const bool dsh_mutation = nl_workbench_mutation(skill_id.c_str(), params.c_str());
    const bool music_app = nodelio_skill_id_is_music(skill_id.c_str());
    if (nodelio_skill_id_preempts_media(skill_id.c_str()) && nodelio_media_preempt_for_heavy_app()) {
        nodelio_skill_clear_music_stash();
        ESP_LOGI(TAG, "preempt media before skill '%s'", skill_id.c_str());
    }

    /* 列表已在屏上时，「播放第一首」绝不能再搜一次歌。
     * 旧行为：模型每轮都重新 invoke music.search(keyword)，服务端换一批结果，
     * 于是「第一首」变成了另一批的第一首，用户听到的是别的歌。
     * 这里先按序号在现有列表里点播，命中就直接返回。 */
    if (music_app) {
        char kw_buf[NL_ENV_SPOKEN];
        const char *kw = nullptr;
        if (!params.empty() && params.front() != '{') {
            kw = params.c_str();
        } else if (!params.empty()) {
            cJSON *root = cJSON_Parse(params.c_str());
            if (root) {
                const cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "keyword");
                if (!cJSON_IsString(j)) {
                    j = cJSON_GetObjectItemCaseSensitive(root, "query");
                }
                if (!cJSON_IsString(j)) {
                    j = cJSON_GetObjectItemCaseSensitive(root, "q");
                }
                if (cJSON_IsString(j) && j->valuestring) {
                    snprintf(kw_buf, sizeof(kw_buf), "%s", j->valuestring);
                    kw = kw_buf;
                }
                cJSON_Delete(root);
            }
        }
        const int ord = kw ? music_keyword_ordinal(kw) : 0;
        if (ord > 0) {
            char title[NL_ENV_TITLE];
            title[0] = '\0';
            if (nodelio_skill_media_play_index(ord, title, sizeof(title))) {
                ESP_LOGI(TAG, "music ordinal %d → list play '%s' (no re-search)", ord, title);
                if (title[0]) {
                    return std::string("正在播放列表第") + std::to_string(ord) + "首《" + title +
                           "》，歌单保留可继续下一首";
                }
                return std::string("已从当前列表开播第") + std::to_string(ord) + "首";
            }
            /* 没有列表才回落到搜歌，并把序号原样交给服务端 */
        }
        /* 内存卡来源的「换一批/下一批」= 卡内下一批 24 首，不能落到云端重搜。 */
        if (kw && nodelio_skill_media_is_sd() &&
            (strstr(kw, "换一批") || strstr(kw, "下一批") || strstr(kw, "再来一批"))) {
            if (nodelio_skill_sd_next_batch()) {
                return std::string("已换到内存卡下一批曲目");
            }
            return std::unexpected(std::string("内存卡曲目不足一批，无法换批"));
        }
    }
    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, cfg->device_id,
             skill_id.c_str());

    std::string body = params.empty() ? "{}" : params;
    if (body.empty() || body.front() != '{') {
        body = std::string("{\"keyword\":\"") + params + "\"}";
    }
    if (music_app) {
        bool unused = false;
        body = music_normalize_invoke_body(body, &unused);
    }

    const int cap = nodelio_caps_max_content_bytes() + 512;
    char *resp = (char *)heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = (char *)malloc((size_t)cap);
    }
    if (!resp) {
        return std::unexpected(std::string("内存不足"));
    }
    resp[0] = '\0';
    int status = 0;
    const int http_ms = dsh_app ? 22000 : (weather_voice ? 16000 : 10000);
    esp_err_t err = nodelio_http_post_json_ms(url, cfg->device_key, body.c_str(), &status, resp,
                                              cap - 1, http_ms);
    const bool envelope_body = resp[0] && strstr(resp, "\"schema_version\"") != nullptr;
    const bool http_ok = (status == 200 || status == 201);
    const bool envelope_err = status >= 400 && envelope_body;
    if (err != ESP_OK || (!http_ok && !envelope_err)) {
        ESP_LOGW(TAG, "invoke skill '%s' fail: status=%d err=%d", skill_id.c_str(), status, err);
        free(resp);
        if (status == 404) {
            return std::unexpected(std::string("未安装该技能，请先在应用市场安装"));
        }
        if (status == 0 || (err != ESP_OK && status < 400)) {
            if (dsh_mutation) {
                return std::unexpected(std::string("结果未确认，请先查看任务，勿重复发送"));
            }
            if (nl_ov_skill_needs_outbound_bridge(skill_id.c_str())) {
                return std::unexpected(std::string("本地桥未连接，请在电脑上启动桥"));
            }
            return std::unexpected(std::string("服务器无响应，请稍后重试"));
        }
        return std::unexpected(std::string("调用技能失败：HTTP ") + std::to_string(status));
    }

    const size_t n = strlen(resp);
    /* http 层按 cap 截断仍返回 200；截断 JSON 必解析失败 */
    if (n >= (size_t)(cap - 2)) {
        ESP_LOGW(TAG, "invoke skill '%s' resp truncated bytes=%u cap=%d", skill_id.c_str(),
                 (unsigned)n, cap);
        free(resp);
        return std::string("曲库结果过大已截断，请直接说歌名搜索");
    }
    if (n > 0 && resp[n - 1] != '}' && resp[n - 1] != ']') {
        ESP_LOGW(TAG, "invoke skill '%s' resp looks incomplete bytes=%u tail=0x%02x", skill_id.c_str(),
                 (unsigned)n, (unsigned)(unsigned char)resp[n - 1]);
    }

    /* 天气：先拿到 spoken，再开卡（开卡二次 invoke 可打中服务端同日缓存）。 */
    if (weather_voice) {
        char spoken[NL_ENV_SPOKEN];
        spoken[0] = '\0';
        cJSON* root = cJSON_Parse(resp);
        const cJSON* sp = root ? cJSON_GetObjectItemCaseSensitive(root, "spoken") : nullptr;
        const cJSON* tx =
            cJSON_IsObject(sp) ? cJSON_GetObjectItemCaseSensitive(sp, "text") : nullptr;
        if (cJSON_IsString(tx) && tx->valuestring) {
            snprintf(spoken, sizeof(spoken), "%s", tx->valuestring);
        }
        cJSON_Delete(root);
        free(resp);
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                (void)nl_ov_apps_open_by_query("日历与天气");
            }
        }
        ESP_LOGI(TAG, "invoke weather spoken=%s", spoken[0] ? spoken : "(empty)");
        if (spoken[0]) {
            return std::string(spoken);
        }
        return std::string("已打开日历与天气");
    }

    /* 双投递：UI Envelope + 模型短摘要；开播只信 Host display.auto_play */
    char spoken[NL_ENV_SPOKEN];
    nodelio_skill_deliver_json(cfg, resp, false, spoken, sizeof(spoken));
    free(resp);
    ESP_LOGI(TAG, "invoke skill '%s' spoken=%s", skill_id.c_str(),
             spoken[0] ? spoken : "(empty)");
    if (spoken[0]) {
        return std::string(spoken);
    }
    return std::string("已展示技能结果");
}

static std::string list_skills_json(const nodelio_nvs_config_t *cfg)
{
    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);

    constexpr int kCap = 24576;
    char *resp = (char *)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) {
        resp = (char *)malloc(kCap);
    }
    if (!resp) {
        return "[]";
    }
    resp[0] = '\0';
    int status = 0;
    esp_err_t err = nodelio_http_get_ms(url, cfg->device_key, NULL, &status, resp, kCap - 1, 8000);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "list skills fail: status=%d err=%d", status, err);
        free(resp);
        return "[]";
    }

    /* C5：重写为短清单，区分「已钉首页」与「仅技能页」 */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    const cJSON *skills = root ? cJSON_GetObjectItemCaseSensitive(root, "skills") : nullptr;
    cJSON *out_arr = cJSON_CreateArray();
    if (cJSON_IsArray(skills) && out_arr) {
        const cJSON *it = nullptr;
        cJSON_ArrayForEach(it, skills) {
            cJSON *row = cJSON_CreateObject();
            if (!row) {
                continue;
            }
            const cJSON *sid = cJSON_GetObjectItemCaseSensitive(it, "skillId");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            const cJSON *pin = cJSON_GetObjectItemCaseSensitive(it, "pinned");
            if (cJSON_IsString(sid) && sid->valuestring) {
                cJSON_AddStringToObject(row, "skillId", sid->valuestring);
            }
            if (cJSON_IsString(name) && name->valuestring) {
                cJSON_AddStringToObject(row, "name", name->valuestring);
            }
            const bool enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
            const bool pinned = cJSON_IsTrue(pin);
            cJSON_AddBoolToObject(row, "enabled", enabled);
            cJSON_AddBoolToObject(row, "pinned", pinned);
            cJSON_AddStringToObject(row, "placement", pinned ? "已钉首页" : "仅技能页");
            cJSON_AddItemToArray(out_arr, row);
        }
    }
    if (root) {
        cJSON_Delete(root);
    }
    if (!out_arr) {
        return "[]";
    }
    char *printed = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    if (!printed) {
        return "[]";
    }
    std::string out(printed);
    cJSON_free(printed);
    return out;
}

/** 从 layout 标题解析 device_id + channel；失败返回 false */
static bool resolve_target_bind(const char *title, char *device_id, size_t device_id_len, int *channel)
{
    if (!title || !title[0] || !device_id || device_id_len == 0 || !channel) {
        return false;
    }
    device_id[0] = '\0';
    *channel = -1;
    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
    if (layout) {
        const nl_widget_t *w = find_widget_by_title(layout, title);
        if (w && w->bind.device_id[0]) {
            std::strncpy(device_id, w->bind.device_id, device_id_len - 1);
            device_id[device_id_len - 1] = '\0';
            if (w->bind.channel >= 0) {
                *channel = w->bind.channel;
            } else if (w->bind.pin >= 0) {
                *channel = w->bind.pin;
            }
            return true;
        }
    }
    UnassignedTarget u;
    if (find_unassigned_target(title, u) && !u.device_id.empty()) {
        std::strncpy(device_id, u.device_id.c_str(), device_id_len - 1);
        device_id[device_id_len - 1] = '\0';
        *channel = u.channel;
        return true;
    }
    return false;
}

static ToolResult schedule_tool(const nodelio_nvs_config_t *cfg, const std::string &op,
                                const std::string &device_id_in, const std::string &target_title,
                                int channel, const std::string &action, const std::string &trigger_time,
                                const std::string &days, const std::string &schedule_id,
                                const std::string &value, int scenario_id)
{
    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char device_id[64] = {0};
    int ch = channel;
    if (!device_id_in.empty()) {
        std::strncpy(device_id, device_id_in.c_str(), sizeof(device_id) - 1);
    } else if (!target_title.empty()) {
        if (!resolve_target_bind(target_title.c_str(), device_id, sizeof(device_id), &ch)) {
            return std::unexpected(std::string("未找到设备，请用 list_controls 确认名称"));
        }
    }

    if (op == "list") {
        if (!device_id[0]) {
            return std::unexpected(std::string("list 需要 device_id 或 target_title"));
        }
        char url[320];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/schedules?targetDeviceId=%s", base,
                 cfg->device_id, device_id);
        char resp[4096] = {0};
        int status = 0;
        esp_err_t err = nodelio_http_get(url, cfg->device_key, NULL, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || status != 200) {
            return std::unexpected(std::string("列出定时失败 HTTP ") + std::to_string(status));
        }
        return std::string(resp);
    }
    if (op == "delete") {
        if (schedule_id.empty()) {
            return std::unexpected(std::string("delete 需要 schedule_id"));
        }
        char url[320];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/schedules/%s", base, cfg->device_id,
                 schedule_id.c_str());
        char resp[512] = {0};
        int status = 0;
        esp_err_t err = nodelio_http_delete(url, cfg->device_key, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || status != 200) {
            return std::unexpected(std::string("删除定时失败 HTTP ") + std::to_string(status));
        }
        return std::string("已删除定时 ") + schedule_id;
    }
    if (op == "add") {
        if (!device_id[0]) {
            return std::unexpected(std::string("add 需要 device_id 或 target_title"));
        }
        std::string act = action.empty() ? "ON" : action;
        for (auto &c : act) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        /* SET_PWM / SET_PWM:75 / action=SET_PWM + value=75 */
        if (act == "SET_PWM" || strncmp(act.c_str(), "SET_PWM:", 8) == 0 ||
            strncmp(act.c_str(), "SET_PWM ", 8) == 0) {
            int duty = -1;
            if (act == "SET_PWM") {
                if (!value.empty()) {
                    duty = atoi(value.c_str());
                }
            } else {
                const char *p = act.c_str() + 7;
                while (*p == ':' || *p == ' ') {
                    p++;
                }
                duty = atoi(p);
            }
            if (duty < 0 || duty > 100) {
                return std::unexpected(std::string("SET_PWM 需要 value=0-100（或 action=SET_PWM:75）"));
            }
            if (ch < 0) {
                return std::unexpected(std::string("SET_PWM 定时需要通道号（target_title 或 channel）"));
            }
            char pwm_act[24];
            std::snprintf(pwm_act, sizeof(pwm_act), "SET_PWM:%d", duty);
            act = pwm_act;
        } else if (act != "ON" && act != "OFF") {
            return std::unexpected(std::string("action 须为 ON / OFF / SET_PWM（或 SET_PWM:N）"));
        }
        if (trigger_time.empty() || days.empty()) {
            return std::unexpected(std::string("需要 time(HH:MM) 与 days(如 mon,tue,wed,thu,fri,sat,sun)"));
        }
        char url[256];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/schedules", base, cfg->device_id);
        char body[640];
        if (scenario_id > 0) {
            snprintf(body, sizeof(body),
                     "{\"targetDeviceId\":\"%s\",\"channelIndex\":%d,\"action\":\"%s\","
                     "\"triggerTime\":\"%s\",\"daysOfWeek\":\"%s\",\"isEnabled\":true,"
                     "\"name\":\"%s %s\",\"scenarioId\":%d}",
                     device_id, ch, act.c_str(), trigger_time.c_str(), days.c_str(), act.c_str(),
                     trigger_time.c_str(), scenario_id);
        } else {
            snprintf(body, sizeof(body),
                     "{\"targetDeviceId\":\"%s\",\"channelIndex\":%d,\"action\":\"%s\","
                     "\"triggerTime\":\"%s\",\"daysOfWeek\":\"%s\",\"isEnabled\":true,\"name\":\"%s %s\"}",
                     device_id, ch, act.c_str(), trigger_time.c_str(), days.c_str(), act.c_str(),
                     trigger_time.c_str());
        }
        char resp[1024] = {0};
        int status = 0;
        esp_err_t err =
            nodelio_http_post_json(url, cfg->device_key, body, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || (status != 200 && status != 201)) {
            return std::unexpected(std::string("创建定时失败 HTTP ") + std::to_string(status));
        }
        std::string ok = std::string("已创建定时：") + act + " @ " + trigger_time;
        nodelio_panel_ui_toast(ok.c_str());
        return ok + (resp[0] ? std::string(" ") + resp : "");
    }
    return std::unexpected(std::string("op 须为 list / add / delete"));
}

static void json_escape_title(const std::string &in, char *out, size_t n)
{
    size_t j = 0;
    for (unsigned char c : in) {
        if (j + 2 >= n) {
            break;
        }
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

/** 日历提醒：op=add|list|done|cancel。时间含糊时须追问，勿默成现在。 */
static ToolResult reminder_tool(const nodelio_nvs_config_t *cfg, const std::string &op_in,
                                const std::string &title_in, const std::string &when,
                                const std::string &time_hm, const std::string &recur_in,
                                const std::string &days, int id, const std::string &query)
{
    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    std::string op = op_in;
    for (auto &c : op) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (op.empty() || op == "list") {
        char url[256];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/reminders", base, cfg->device_id);
        char resp[4096] = {0};
        int status = 0;
        esp_err_t err = nodelio_http_get(url, cfg->device_key, NULL, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || status != 200) {
            return std::unexpected(std::string("列出提醒失败 HTTP ") + std::to_string(status));
        }
        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            return std::string("当前没有提醒");
        }
        const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
        if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0) {
            cJSON_Delete(root);
            return std::string("当前没有未完成提醒");
        }
        std::string out = "未完成提醒：";
        int n = 0;
        const cJSON *it = nullptr;
        cJSON_ArrayForEach(it, items) {
            if (n >= 8) {
                break;
            }
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(it, "title");
            const cJSON *recur = cJSON_GetObjectItemCaseSensitive(it, "recur");
            const cJSON *tm = cJSON_GetObjectItemCaseSensitive(it, "time");
            const cJSON *nf = cJSON_GetObjectItemCaseSensitive(it, "nextFireAt");
            const cJSON *rid = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (!cJSON_IsString(title) || !title->valuestring) {
                continue;
            }
            if (n > 0) {
                out += "；";
            }
            out += title->valuestring;
            if (cJSON_IsNumber(rid)) {
                out += "(id=";
                out += std::to_string((int)rid->valuedouble);
                out += ")";
            }
            if (cJSON_IsString(recur) && recur->valuestring) {
                if (strcmp(recur->valuestring, "daily") == 0) {
                    out += " 每天";
                } else if (strcmp(recur->valuestring, "weekly") == 0) {
                    out += " 每周";
                }
            }
            if (cJSON_IsString(tm) && tm->valuestring && tm->valuestring[0]) {
                out += " ";
                out += tm->valuestring;
            } else if (cJSON_IsString(nf) && nf->valuestring && strlen(nf->valuestring) >= 16) {
                /* 2026-11-08T08:00:00+08:00 → 11-08 08:00 */
                out += " ";
                out.append(nf->valuestring + 5, 5);
                out += " ";
                out.append(nf->valuestring + 11, 5);
            }
            n++;
        }
        cJSON_Delete(root);
        nodelio_skill_daily_reminders_invalidate();
        return out;
    }
    if (op == "add") {
        if (title_in.empty()) {
            return std::unexpected(std::string("请说明提醒内容，例如：提醒我明天早上八点倒垃圾"));
        }
        std::string recur = recur_in.empty() ? "once" : recur_in;
        for (auto &c : recur) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (recur == "once" && when.empty()) {
            return std::unexpected(std::string(
                "时间不清楚。请问清具体日期和时间后再创建，例如明天早上八点，不要默认为现在。"));
        }
        if ((recur == "daily" || recur == "weekly") && time_hm.empty()) {
            return std::unexpected(std::string("请问清每天或每周的具体钟点，例如晚上十点。"));
        }
        if (recur == "weekly" && days.empty()) {
            return std::unexpected(std::string("请问清是星期几，或工作日 / 周末。"));
        }
        char esc[160];
        json_escape_title(title_in, esc, sizeof(esc));
        char body[512];
        if (recur == "once") {
            char wesc[80];
            json_escape_title(when, wesc, sizeof(wesc));
            snprintf(body, sizeof(body),
                     "{\"title\":\"%s\",\"recur\":\"once\",\"when\":\"%s\"}", esc, wesc);
        } else {
            char desc[48];
            json_escape_title(days, desc, sizeof(desc));
            char tesc[16];
            json_escape_title(time_hm, tesc, sizeof(tesc));
            snprintf(body, sizeof(body),
                     "{\"title\":\"%s\",\"recur\":\"%s\",\"time\":\"%s\",\"days\":\"%s\"}", esc,
                     recur.c_str(), tesc, desc);
        }
        char url[256];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/reminders", base, cfg->device_id);
        char resp[1024] = {0};
        int status = 0;
        esp_err_t err =
            nodelio_http_post_json(url, cfg->device_key, body, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || (status != 200 && status != 201)) {
            cJSON *er = cJSON_Parse(resp);
            const cJSON *msg = er ? cJSON_GetObjectItemCaseSensitive(er, "error") : nullptr;
            std::string detail = (msg && cJSON_IsString(msg) && msg->valuestring)
                                     ? msg->valuestring
                                     : (std::string("HTTP ") + std::to_string(status));
            if (er) {
                cJSON_Delete(er);
            }
            return std::unexpected(std::string("创建提醒失败：") + detail);
        }
        cJSON *root = cJSON_Parse(resp);
        const cJSON *spoken = root ? cJSON_GetObjectItemCaseSensitive(root, "spoken") : nullptr;
        std::string ok = (spoken && cJSON_IsString(spoken) && spoken->valuestring)
                             ? spoken->valuestring
                             : (std::string("已记下：") + title_in);
        if (root) {
            cJSON_Delete(root);
        }
        nodelio_panel_ui_toast(ok.c_str());
        nodelio_skill_daily_reminders_invalidate();
        return ok;
    }
    if (op == "done" || op == "cancel") {
        char url[288];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/reminders/action", base, cfg->device_id);
        char esc[160];
        const std::string q = !query.empty() ? query : title_in;
        json_escape_title(q, esc, sizeof(esc));
        char body[320];
        if (id > 0) {
            snprintf(body, sizeof(body), "{\"op\":\"%s\",\"id\":%d}", op.c_str(), id);
        } else if (esc[0]) {
            snprintf(body, sizeof(body), "{\"op\":\"%s\",\"title\":\"%s\"}", op.c_str(), esc);
        } else {
            return std::unexpected(std::string(op == "done" ? "请说明完成哪一条提醒"
                                                            : "请说明取消哪一条提醒"));
        }
        char resp[512] = {0};
        int status = 0;
        esp_err_t err =
            nodelio_http_post_json(url, cfg->device_key, body, &status, resp, sizeof(resp) - 1);
        if (err != ESP_OK || status != 200) {
            if (status == 404) {
                return std::unexpected(std::string("未找到对应提醒"));
            }
            return std::unexpected(std::string("操作失败 HTTP ") + std::to_string(status));
        }
        cJSON *root = cJSON_Parse(resp);
        const cJSON *spoken = root ? cJSON_GetObjectItemCaseSensitive(root, "spoken") : nullptr;
        std::string ok = (spoken && cJSON_IsString(spoken) && spoken->valuestring)
                             ? spoken->valuestring
                             : (op == "done" ? "已完成" : "已取消");
        if (root) {
            cJSON_Delete(root);
        }
        nodelio_skill_daily_reminders_invalidate();
        return ok;
    }
    return std::unexpected(std::string("op 须为 add / list / done / cancel"));
}

void nodelio_mcp_register_tools(void)
{
    auto &mcp = McpServer::GetInstance();

    // Keep this tool and open_app on the first tools/list page.
    mcp.AddTool(
        "self.skill.news",
        /* 首句点名：云端易把「读新闻」当成不存在的工具而空口否认 */
        "self.skill.news: 读新闻/读文章/每日一篇. "
        "「调用skill.news读」/读/读吧→op=read; 「调用skill.news续」/继续读→op=continue. "
        "Call ONLY ONCE per turn; read returned text verbatim in full; never ask to continue.",
        PropertyList({
            Property("op", kPropertyTypeString, "read"),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
            if (op.empty()) {
                op = "read";
            }
            if (op != "prev" && op != "next" && op != "read" && op != "continue" && op != "mark_read" &&
                op != "mark" && op != "skip" && op != "close" && op != "dismiss") {
                return std::unexpected(std::string("op 须为 prev/next/read/continue/mark_read/skip/close"));
            }
            auto& app = Application::GetInstance();
            /* 云端常忽略 continue / 同轮连调两次 read；有未读游标时强制 continue。 */
            if (op == "read" && app.PreferNewsContinue()) {
                ESP_LOGW("nl_mcp", "cloud sent op=read while continue preferred → force continue");
                op = "continue";
            }
            app.Schedule([]() { Application::GetInstance().OnNewsReadToolCalled(); });
            constexpr size_t kNewsSpokenCap = 1536;
            char *spoken = static_cast<char *>(
                heap_caps_malloc(kNewsSpokenCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!spoken) {
                spoken = static_cast<char *>(malloc(kNewsSpokenCap));
            }
            if (!spoken) {
                return std::unexpected(std::string("内存不足"));
            }
            spoken[0] = '\0';
            ESP_LOGI("nl_mcp", "tools/call self.skill.news op=%s", op.c_str());
            const bool ok = nodelio_skill_news_action(op.c_str(), spoken, kNewsSpokenCap);
            std::string out = spoken;
            heap_caps_free(spoken);
            ESP_LOGI("nl_mcp", "news result op=%s ok=%d bytes=%u", op.c_str(), ok ? 1 : 0,
                     (unsigned)out.size());
            if (!ok) {
                if (op == "read") {
                    return std::unexpected(std::string("暂无可读正文，请先打开新闻"));
                }
                if (op == "prev") {
                    return std::unexpected(std::string("没有上一篇"));
                }
                if (op == "continue") {
                    return std::unexpected(std::string("没有更多正文"));
                }
                if (op == "mark_read" || op == "mark") {
                    return std::unexpected(std::string("没有可标记的新闻"));
                }
                if (op == "close" || op == "dismiss") {
                    return std::string("已关闭");
                }
                return std::unexpected(std::string("没有下一篇"));
            }
            if (out.empty()) {
                return std::unexpected(std::string("正文加载失败"));
            }
            return out;
        });

    mcp.AddTool(
        "self.support.suggest",
        "self.support.suggest: 客服拟回复。听到「调support.suggest」必须调用。"
        "replies 为空时返回访客原话与回复语言；你必须立刻再次调用，replies 为 2 到 4 条短回复，用 | 分隔。"
        "回复语言必须与访客原话一致：英文访客→全英文，中文访客→简体中文。不要朗读。",
        PropertyList({
            Property("replies", kPropertyTypeString, std::string("")).SetMaxLength(512),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string replies = properties["replies"].value<std::string>();
            std::vector<std::string> lines;
            std::string cur;
            auto push = [&]() {
                while (!cur.empty() && (cur.front() == ' ' || cur.front() == '\t')) {
                    cur.erase(cur.begin());
                }
                while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t' || cur.back() == '\r')) {
                    cur.pop_back();
                }
                if (!cur.empty() && lines.size() < 4) {
                    lines.push_back(cur);
                }
                cur.clear();
            };
            if (!replies.empty() && replies.front() == '[') {
                cJSON *arr = cJSON_Parse(replies.c_str());
                if (cJSON_IsArray(arr)) {
                    const cJSON *it = nullptr;
                    cJSON_ArrayForEach(it, arr) {
                        if (cJSON_IsString(it) && it->valuestring && lines.size() < 4) {
                            cur = it->valuestring;
                            push();
                        }
                    }
                }
                cJSON_Delete(arr);
            }
            if (lines.empty()) {
                for (char c : replies) {
                    if (c == '|' || c == '\n') {
                        push();
                    } else {
                        cur.push_back(c);
                    }
                }
                push();
            }
            if (lines.empty()) {
                char visitor[256];
                visitor[0] = '\0';
                if (!nodelio_skill_support_visitor_text(visitor, sizeof(visitor)) || !visitor[0]) {
                    return std::unexpected(std::string("请先打开客服会话"));
                }
                Application::GetInstance().Schedule([]() {
                    Application::GetInstance().OnSupportSuggestToolCalled(false);
                });
                /* 与 go-chat EffectiveReplyLocale 同判：拉丁字母为主且无汉字 → 英文。 */
                int letters = 0, han = 0;
                for (const unsigned char *p = reinterpret_cast<const unsigned char *>(visitor); *p;) {
                    if (*p < 0x80) {
                        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                            letters++;
                        }
                        p++;
                        continue;
                    }
                    if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
                        const uint32_t cp = ((uint32_t)(p[0] & 0x0F) << 12) |
                                           ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
                        if (cp >= 0x4E00 && cp <= 0x9FFF) {
                            han++;
                        }
                        p += 3;
                        continue;
                    }
                    p += (*p & 0xF8) == 0xF0 ? 4 : ((*p & 0xE0) == 0xC0 ? 2 : 1);
                }
                const bool en = letters >= 3 && han == 0;
                std::string out = en ? "Visitor message: " : "访客原话：";
                out += visitor;
                if (en) {
                    out += "\nCRITICAL: Reply language is English ONLY (no Chinese). "
                           "Call self.support.suggest again now with replies = 2 to 4 short English "
                           "replies separated by |. Do not speak them aloud.";
                } else {
                    out += "\n关键：用简体中文写候选。立刻再次调用 self.support.suggest，"
                           "replies 为 2 到 4 条针对这句话的短回复，用 | 分隔。不要朗读。";
                }
                return out;
            }
            Application::GetInstance().Schedule([lines]() {
                const char *ptrs[4] = {};
                int n = 0;
                for (const auto &line : lines) {
                    if (n < 4) {
                        ptrs[n++] = line.c_str();
                    }
                }
                const bool ok = nodelio_skill_support_apply_cloud_replies(ptrs, n);
                if (!ok) {
                    nodelio_skill_support_cloud_suggest_failed();
                    nodelio_panel_ui_toast("小智未给出回复选项");
                }
                Application::GetInstance().OnSupportSuggestToolCalled(true);
            });
            return std::string("已写入发送按钮，不要朗读");
        });

    mcp.AddTool(
        "self.nodelio.reboot",
        "重启本机墙板。用户说重启/重新启动时调用。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_panel_ui_local_reboot();
            return std::string("正在重启");
        });

    mcp.AddTool(
        "self.nodelio.enter_wifi_config",
        "进入 SoftAP 配网。说进入配网/打开热点时调用。屏上网络页用 goto_page connect。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_panel_ui_local_wifi_config();
            return std::string("已进入配网热点，请手机连接后打开 192.168.4.1");
        });

    mcp.AddTool(
        "self.nodelio.goto_page",
        "切页面。page=home|apps|settings|connect|display|power|voice|media|controls|system|reset。"
        "不关技能弹窗；关电台/新闻须再调 self.skill.close。",
        PropertyList({Property("page", kPropertyTypeString, "home")}),
        [](const PropertyList &properties) -> ToolResult {
            std::string page = properties["page"].value<std::string>();
            int id = NL_UI_HOME;
            if (page == "apps" || page == "应用") {
                id = NL_UI_APPS;
            } else if (page == "settings" || page == "设置") {
                id = NL_UI_SETTINGS;
            } else if (page == "connect" || page == "连接" || page == "网络") {
                id = NL_UI_CONNECT;
            } else if (page == "display" || page == "显示" || page == "亮度") {
                id = NL_UI_DISP;
            } else if (page == "power" || page == "省电") {
                id = NL_UI_POWER;
            } else if (page == "voice" || page == "语音") {
                id = NL_UI_VOICE;
            } else if (page == "media" || page == "播放器") {
                id = NL_UI_MEDIA;
            } else if (page == "controls" || page == "控家") {
                id = NL_UI_CTRL;
            } else if (page == "system" || page == "设备信息" || page == "系统") {
                id = NL_UI_SYS;
            } else if (page == "reset" || page == "恢复出厂" || page == "出厂重置") {
                id = NL_UI_RESET;
            } else if (page == "home" || page == "首页") {
                id = NL_UI_HOME;
            } else {
                return std::unexpected(std::string(
                    "page 须为 home/apps/settings/connect/display/power/voice/media/controls/system/reset"));
            }
            nodelio_panel_ui_goto(id);
            if (id == NL_UI_RESET) {
                return std::string("已打开重置页，请在屏幕确认；语音不会直接恢复出厂");
            }
            return std::string("已切换页面");
        });

    mcp.AddTool(
        "self.nodelio.home_screen",
        "翻首页屏。dir=next|prev。说上一屏/下一屏时调用。到头说明原因。",
        PropertyList({Property("dir", kPropertyTypeString, "next")}),
        [](const PropertyList &properties) -> ToolResult {
            std::string dir = properties["dir"].value<std::string>();
            int delta = 0;
            if (dir == "next" || dir == "下一屏" || dir == "往后翻") {
                delta = 1;
            } else if (dir == "prev" || dir == "上一屏" || dir == "往前翻") {
                delta = -1;
            } else {
                return std::unexpected(std::string("dir 须为 next 或 prev"));
            }
            esp_err_t e = nodelio_panel_ui_home_page(delta);
            if (e == ESP_ERR_NOT_FOUND) {
                return std::string("只有一屏，无法翻页");
            }
            if (e == ESP_ERR_INVALID_STATE) {
                return std::string(delta < 0 ? "已经是第一屏" : "已经是最后一屏");
            }
            if (e != ESP_OK) {
                return std::unexpected(std::string("翻页失败"));
            }
            return std::string(delta < 0 ? "已切到上一屏" : "已切到下一屏");
        });

    mcp.AddTool(
        "self.nodelio.scroll",
        "竖滚设置/连接/应用列表。dir=up|down。首页磁贴用 home_screen。",
        PropertyList({Property("dir", kPropertyTypeString, "down")}),
        [](const PropertyList &properties) -> ToolResult {
            std::string dir = properties["dir"].value<std::string>();
            int d = 0;
            if (dir == "down" || dir == "往下翻" || dir == "向下滑") {
                d = 1;
            } else if (dir == "up" || dir == "往上翻" || dir == "向上滑") {
                d = -1;
            } else {
                return std::unexpected(std::string("dir 须为 up 或 down"));
            }
            esp_err_t e = nodelio_panel_ui_scroll(d);
            if (e == ESP_ERR_NOT_FOUND) {
                return std::string("当前页不能滚动");
            }
            if (e == ESP_ERR_INVALID_STATE) {
                return std::string(d < 0 ? "已经到顶了" : "已经到底了");
            }
            if (e != ESP_OK) {
                return std::unexpected(std::string("滚动失败"));
            }
            return std::string(d < 0 ? "已往上翻" : "已往下翻");
        });

    mcp.AddTool(
        "self.nodelio.set_pref",
        "改面板设置。亮度/音量用 key=brightness|volume value=0-100（你有此能力，请直接调用）。"
        "其它 key=idle_wifi_s|idle_sleep_s|bye_sleep_s|power_save|controls|xiaozhi|"
        "voice_barge_in|listen_idle_s|media_sleep|media_pause_s|media_wake_duck_pct|text_scale|ui_mode|"
        "layout_sync|fw_check|factory_reset。text_scale=0标准|1较大|2更大。ui_mode=0浅色|1深色。"
        "voice_barge_in=on/off 免唤醒词语音打断。",
        PropertyList({
            Property("key", kPropertyTypeString),
            Property("value", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string key = properties["key"].value<std::string>();
            std::string value = properties["value"].value<std::string>();
            char err[128];
            esp_err_t e = nodelio_panel_ui_set_pref(key.c_str(), value.c_str(), err, sizeof(err));
            if (e != ESP_OK) {
                return std::unexpected(std::string(err[0] ? err : "设置失败"));
            }
            if (strcasecmp(key.c_str(), "xiaozhi") == 0 &&
                (strcasecmp(value.c_str(), "off") == 0 || value == "0" || value == "关")) {
                return std::string("已关闭小智云；本轮可说完");
            }
            if (strcasecmp(key.c_str(), "local_cmd") == 0) {
                return std::string("本地口令已下线");
            }
            if (strcasecmp(key.c_str(), "layout_sync") == 0) {
                return std::string("已开始刷新布局");
            }
            if (strcasecmp(key.c_str(), "fw_check") == 0) {
                return std::string("已开始检查固件更新");
            }
            if (strcasecmp(key.c_str(), "factory_reset") == 0) {
                return std::string("已打开重置页，请在屏幕确认；未执行恢复出厂");
            }
            char cur[16];
            if (nodelio_panel_ui_get_pref(key.c_str(), cur, sizeof(cur)) == ESP_OK) {
                return std::string("已设置 ") + key + "=" + cur;
            }
            return std::string("已设置");
        });

    mcp.AddTool(
        "self.nodelio.open_app",
        "打开已装应用。听歌/曲库→name=曲库；电台/播客/新闻/天气同理。"
        "内存卡/TF卡/SD卡里的歌→name=内存卡（本机读卡，不走云端）。"
        "工作台可指定 DeepSeek/ChatGPT/WorkBuddy；查看任务打开默认入口。禁云端 play_music。"
        "name=曲库/内存卡/音乐/新闻/天气/电台/播客/空气/每日一篇/查看任务 或 app_id。",
        PropertyList({Property("name", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            std::string name = properties["name"].value<std::string>();
            ESP_LOGI(TAG, "MCP open_app name='%s'", name.c_str());
            if (name.empty()) {
                return std::unexpected(std::string("name 不能为空"));
            }
            /* 内存卡：本地挂卡+扫描，不经服务端技能 */
            if (name.find("内存卡") != std::string::npos || name.find("存储卡") != std::string::npos ||
                name.find("SD卡") != std::string::npos || name.find("sd卡") != std::string::npos ||
                name.find("TF卡") != std::string::npos || name.find("tf卡") != std::string::npos ||
                name.find("本地音乐") != std::string::npos || name.find("本地曲库") != std::string::npos) {
                nodelio_skill_request_sd_library(false);
                return std::string("正在读取内存卡曲库");
            }
            /* 明确平台同步查询；泛称工作台由已装入口解析默认平台。 */
            const char* workbench = nl_workbench_named_app(name.c_str());
            char selected[72] = {0};
            if (nl_workbench_app(name.c_str())) {
                nl_workbench_status_id(name.c_str(), selected, sizeof(selected));
                workbench = selected;
            }
            if (workbench) {
                nodelio_nvs_config_t cfg{};
                if (!load_cfg(&cfg)) {
                    return std::unexpected(std::string("未绑定 NodeLio 服务"));
                }
                return invoke_skill(&cfg, workbench, "{}");
            }
            if (!nodelio_panel_ui_open_app(name.c_str())) {
                ESP_LOGW(TAG, "MCP open_app fail name='%s'", name.c_str());
                return std::unexpected(std::string("无法打开应用：") + name);
            }
            ESP_LOGI(TAG, "MCP open_app scheduled name='%s'", name.c_str());
            return std::string("正在打开 ") + name;
        });

    mcp.AddTool(
        "self.nodelio.list_controls",
        "列出可控设备 JSON[{title,room?,kind?,online?}]。offline 勿控。"
        "不含开/关态；问开没开用 control action=STATUS。",
        PropertyList(), [](const PropertyList &) -> ReturnValue { return list_controls_json(); });

    mcp.AddTool(
        "self.nodelio.control",
        "控家。target_title；action=ON|OFF|TOGGLE|SET_PWM|OFF_IN|CANCEL_COUNTDOWN|STATUS|apply；"
        "value=秒或PWM0-100；phrase=备用口令。"
        "问开没开/哪路开着：action=STATUS（点名列该芯片各路；不点名只列还开着的）；短中文，勿猜 list_controls。"
        "N秒后关/开直接 OFF_IN，禁止先问确定。",
        PropertyList({
            Property("target_title", kPropertyTypeString, ""),
            Property("action", kPropertyTypeString, ""),
            Property("value", kPropertyTypeString, ""),
            Property("phrase", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(
                    std::string("未绑定 NodeLio：请在屏幕右下角「家」→ 设置填服务器与绑定码"));
            }
            if (!nodelio_controls_enabled()) {
                return std::unexpected(std::string(kControlsOff));
            }
            const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();

            std::string title = properties["target_title"].value<std::string>();
            std::string action = properties["action"].value<std::string>();
            std::string value = properties["value"].value<std::string>();
            std::string phrase = properties["phrase"].value<std::string>();

            ESP_LOGI(TAG, "control target_title='%s' action='%s' value='%s' phrase='%s'",
                     title.c_str(), action.c_str(), value.c_str(), phrase.c_str());

            if (strcasecmp(action.c_str(), "STATUS") == 0 ||
                strcasecmp(action.c_str(), "QUERY") == 0 ||
                strcasecmp(action.c_str(), "STATE") == 0) {
                return control_status(layout, title, phrase);
            }

            /* OFF_IN：action=OFF_IN + value=秒 → MQTT 字符串；CANCEL_COUNTDOWN 同理 */
            char off_in_buf[48];
            const char *val = nullptr;
            if (strcasecmp(action.c_str(), "OFF_IN") == 0 || strcasecmp(action.c_str(), "COUNTDOWN") == 0) {
                int sec = 0;
                if (!value.empty()) {
                    sec = atoi(value.c_str());
                }
                if (sec <= 0) {
                    return std::unexpected(std::string("OFF_IN 需要 value=正整数秒，例如 600"));
                }
                std::snprintf(off_in_buf, sizeof(off_in_buf), "OFF_IN %d", sec);
                val = off_in_buf;
            } else if (strcasecmp(action.c_str(), "CANCEL_COUNTDOWN") == 0) {
                val = "CANCEL_COUNTDOWN";
            } else {
                val = value.empty() ? (action.empty() ? "TOGGLE" : action.c_str()) : value.c_str();
            }

            // 1. 优先根据大模型指定的具名设备/通道精确控制（优先排布控件，其次全屋未排布设备）
            if (!title.empty()) {
                if (layout) {
                    const nl_widget_t *w = find_widget_by_title(layout, title.c_str());
                    if (w) {
                        return run_widget_set(&cfg, w, val);
                    }
                }
                UnassignedTarget unassigned;
                if (find_unassigned_target(title.c_str(), unassigned)) {
                    return run_unassigned_set(&cfg, unassigned, val);
                }
            }

            // 2. 若大模型未指定或未命中 title，尝试从 phrase 抽取名称查找
            if (!phrase.empty()) {
                char hint[48];
                extract_title_hint(phrase.c_str(), hint, sizeof(hint));
                if (hint[0]) {
                    /* 已解析出 OFF_IN/CANCEL 时保留 val，勿被开/关语义覆盖 */
                    const bool keep_val = (strncmp(val, "OFF_IN ", 7) == 0) ||
                                          (strcmp(val, "CANCEL_COUNTDOWN") == 0);
                    const char *fallback_val =
                        keep_val ? val
                                 : (value.empty() ? (phrase_implies_on(phrase.c_str()) ? "ON" : "OFF")
                                                  : value.c_str());
                    if (layout) {
                        const nl_widget_t *w = find_widget_by_title(layout, hint);
                        if (w) {
                            ESP_LOGI(TAG, "fallback by title_hint='%s' → %s", hint, w->id);
                            return run_widget_set(&cfg, w, fallback_val);
                        }
                    }
                    UnassignedTarget unassigned;
                    if (find_unassigned_target(hint, unassigned)) {
                        ESP_LOGI(TAG, "fallback unassigned by title_hint='%s' → %s", hint,
                                 unassigned.title.c_str());
                        return run_unassigned_set(&cfg, unassigned, fallback_val);
                    }
                }
            }

            return std::unexpected(std::string(
                "未找到对应设备。请先调用 list_controls 获取设备列表，确认名称（如厨房灯、卧室灯）。"));
        });

    mcp.AddTool(
        "self.nodelio.schedule",
        "定时：op=list|add|delete；target_title；action=ON|OFF|SET_PWM；time=HH:MM；"
        "days=mon,tue,...；delete 用 schedule_id。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
            Property("target_title", kPropertyTypeString, ""),
            Property("device_id", kPropertyTypeString, ""),
            Property("channel", kPropertyTypeInteger, -1),
            Property("action", kPropertyTypeString, ""),
            Property("value", kPropertyTypeString, ""),
            Property("time", kPropertyTypeString, ""),
            Property("days", kPropertyTypeString, ""),
            Property("scenario_id", kPropertyTypeInteger, 0),
            Property("schedule_id", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(std::string("未绑定 NodeLio 服务"));
            }
            if (!nodelio_controls_enabled()) {
                return std::unexpected(std::string(kControlsOff));
            }
            std::string op = properties["op"].value<std::string>();
            std::string title = properties["target_title"].value<std::string>();
            std::string device_id = properties["device_id"].value<std::string>();
            int channel = properties["channel"].value<int>();
            std::string action = properties["action"].value<std::string>();
            std::string value = properties["value"].value<std::string>();
            std::string time = properties["time"].value<std::string>();
            std::string days = properties["days"].value<std::string>();
            int scenario_id = properties["scenario_id"].value<int>();
            std::string schedule_id = properties["schedule_id"].value<std::string>();
            return schedule_tool(&cfg, op, device_id, title, channel, action, time, days, schedule_id,
                                value, scenario_id);
        });

    mcp.AddTool(
        "self.nodelio.reminder",
        "日历提醒/待办。op=add|list|done|cancel。"
        "add：title 必填；once 用 when=YYYY-MM-DDTHH:MM:SS（本地）；"
        "daily 用 time=HH:MM；weekly 用 time + days=mon,tue 或 weekday。"
        "时间含糊（回头/有空）必须追问，禁止默认现在。"
        "done/cancel：id 或 title/query 匹配。"
        "用户说提醒我…/我有什么待办/完成了/取消提醒时调用。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
            Property("title", kPropertyTypeString, ""),
            Property("when", kPropertyTypeString, ""),
            Property("time", kPropertyTypeString, ""),
            Property("recur", kPropertyTypeString, "once"),
            Property("days", kPropertyTypeString, ""),
            Property("id", kPropertyTypeInteger, 0),
            Property("query", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(std::string("未绑定 NodeLio 服务"));
            }
            return reminder_tool(&cfg, properties["op"].value<std::string>(),
                                 properties["title"].value<std::string>(),
                                 properties["when"].value<std::string>(),
                                 properties["time"].value<std::string>(),
                                 properties["recur"].value<std::string>(),
                                 properties["days"].value<std::string>(),
                                 properties["id"].value<int>(),
                                 properties["query"].value<std::string>());
        });

    mcp.AddTool(
        "self.skill.invoke",
        "调技能。skill_id=music.search|radio.stations|podcast.latest|news.latest|"
        "article.daily|codex.status|dsh.status|openai.status|workbuddy.status|workbench."
        "status|support.inbox。"
        "工作台查询用对应 .status；params 可带 computer=电脑名称，select_computer=true "
        "选择电脑。派活含 operation=task 和 instruction；续聊保留返回的 session，不得改投电脑。"
        "业务FAQ/知识库：support.inbox params={\"op\":\"kb_lookup\",\"text\":\"问题\"}（无需先开客服卡）。"
        "访问统计：support.inbox params={\"op\":\"analytics_summary\",\"range\":\"day|week|month\"}"
        "（无需开卡；也可 text 说今天/本周/本月访问）。"
        "客服卡已开时 skill_id=support.inbox，params 只带 text=口令转写（固件补 conversation_id 与 op=draft），勿直接发访客。"
        "听歌须 open_app(曲库)；旧 dsh.task 仍兼容；"
        "列表已开随便放→transport play。",
        PropertyList({
            Property("skill_id", kPropertyTypeString, ""),
            Property("params", kPropertyTypeString, ""),
        }),
        [](const PropertyList& properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(std::string("未绑定 NodeLio 服务"));
            }
            std::string skill_id = properties["skill_id"].value<std::string>();
            std::string params = properties["params"].value<std::string>();
            return invoke_skill(&cfg, skill_id, params);
        });

    mcp.AddTool(
        "self.skill.transport",
        "面板当前歌单播控。op=prev|next|pause|resume|play|stop|close。"
        "用户说换一首/下一首/切歌→next；上一首→prev。"
        "「播放第N首」→ op=play index=N（在当前列表里选，勿重新搜歌）。"
        "op=list=读出当前歌单曲名（回答「第一首是什么」前先看它）。"
        "电台/播客的 prev/next 在当前列表里换台或换集。直播流断开不会自动跳台。"
        "stop/close=停播并关掉技能弹窗（关闭电台/播客/曲库时用，勿只 goto_page）。"
        "随便放一首用 play，勿重新搜索。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
            Property("index", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
            const int index = properties["index"].value<int>();
            if (op == "list") {
                char buf[NL_ENV_SPOKEN];
                const int n = nodelio_skill_media_list_titles(buf, sizeof(buf), 12);
                if (n <= 0) {
                    return std::unexpected(std::string("当前没有可播放的曲目列表，请先打开面板曲库"));
                }
                return std::string(buf);
            }
            if (op == "prev") {
                if (!nodelio_skill_media_prev()) {
                    return std::unexpected(std::string("没有上一首"));
                }
                return std::string("已切换上一首");
            }
            if (op == "next") {
                if (!nodelio_skill_media_next()) {
                    return std::unexpected(std::string("没有下一首"));
                }
                return std::string("已切换下一首");
            }
            if (op == "pause") {
                if (nodelio_media_is_paused()) {
                    return std::string("已暂停");
                }
                if (!nodelio_media_is_playing()) {
                    return std::unexpected(std::string("当前没有在播"));
                }
                nodelio_media_pause();
                return std::string("已暂停");
            }
            if (op == "resume") {
                if (nodelio_media_is_playing()) {
                    return std::string("正在播放");
                }
                if (!nodelio_media_is_paused()) {
                    return std::unexpected(std::string("没有可继续的曲目"));
                }
                nodelio_media_resume();
                return std::string("继续播放");
            }
            if (op == "play" || op == "random" || op == "any") {
                char title[64];
                title[0] = '\0';
                /* 带 index 时按序号点播当前列表，绝不重新搜歌。 */
                if (index > 0) {
                    if (!nodelio_skill_media_play_index(index, title, sizeof(title))) {
                        return std::unexpected(std::string("当前列表没有第") + std::to_string(index) +
                                               "首，请先打开曲库看列表");
                    }
                    if (title[0]) {
                        return std::string("正在播放列表第") + std::to_string(index) + "首《" + title +
                               "》，歌单保留可继续下一首";
                    }
                    return std::string("已从当前列表开播");
                }
                if (!nodelio_skill_media_play_any(title, sizeof(title))) {
                    return std::unexpected(std::string("当前没有可播放的曲目列表，请先打开面板曲库"));
                }
                if (title[0]) {
                    return std::string("正在播放《") + title + "》，歌单保留可继续下一首";
                }
                return std::string("已从当前列表开播");
            }
            if (op == "stop" || op == "close" || op == "dismiss") {
                nodelio_skill_close_all();
                return std::string("已关闭技能弹窗");
            }
            return std::unexpected(
                std::string("op 须为 prev/next/pause/resume/play/stop/close/list"));
        });

    mcp.AddTool(
        "self.skill.close",
        "关掉技能弹窗并停播（电台/播客/曲库/新闻/天气等）。"
        "用户说关闭电台/关掉播客/收起新闻/退出应用时调用。goto_page home 不会关弹窗。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_skill_close_all();
            return std::string("已关闭技能弹窗");
        });

    mcp.AddTool(
        "self.skill.list",
        "列出已装技能。面板放歌用 self.skill.invoke(music.search)，勿用云端音乐工具。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return "[]";
            }
            return list_skills_json(&cfg);
        });

    mcp.AddTool(
        "self.notice",
        "面板未读消息提醒：op=list|open|clear；index=第几条(1起，0=全部/打开弹窗)。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
            Property("index", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
            int idx = properties["index"].value<int>();
            const int n = nodelio_panel_ui_notice_unread_count();
            if (op.empty() || op == "list") {
                cJSON *arr = cJSON_CreateArray();
                if (!arr) {
                    return "[]";
                }
                for (int i = 0; i < n; ++i) {
                    const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(i);
                    if (!it) {
                        continue;
                    }
                    cJSON *o = cJSON_CreateObject();
                    if (!o) {
                        continue;
                    }
                    cJSON_AddNumberToObject(o, "index", i + 1);
                    cJSON_AddStringToObject(o, "title", it->title[0] ? it->title : "新通知");
                    cJSON_AddStringToObject(o, "mode", it->mode[0] ? it->mode : "info");
                    cJSON_AddStringToObject(o, "severity", it->severity[0] ? it->severity : "normal");
                    if (it->app_id[0]) {
                        cJSON_AddStringToObject(o, "app_id", it->app_id);
                    }
                    cJSON_AddItemToArray(arr, o);
                }
                char *out = cJSON_PrintUnformatted(arr);
                cJSON_Delete(arr);
                if (!out) {
                    return "[]";
                }
                std::string s(out);
                cJSON_free(out);
                return s;
            }
            if (op == "open") {
                if (idx > 0) {
                    const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(idx - 1);
                    if (!it || !it->notice_id[0]) {
                        return std::unexpected(std::string("未找到该条通知"));
                    }
                    std::string title = it->title[0] ? it->title : "通知";
                    nodelio_panel_ui_notice_resolve(it->notice_id, true);
                    return std::string("已打开并标为已读：") + title;
                }
                nodelio_panel_ui_open_notices();
                return std::string("已打开消息中心");
            }
            if (op == "clear" || op == "dismiss") {
                if (idx > 0) {
                    const nodelio_panel_notice_t *it = nodelio_panel_ui_notice_get(idx - 1);
                    if (!it || !it->notice_id[0]) {
                        return std::unexpected(std::string("未找到该条通知"));
                    }
                    nodelio_panel_ui_notice_resolve(it->notice_id, false);
                    return std::string("已标为已读");
                }
                nodelio_panel_ui_notice_resolve_all();
                nodelio_panel_ui_close_notices();
                return std::string("已清空全部未读消息");
            }
            return std::unexpected(std::string("op 须为 list/open/clear"));
        });

    ESP_LOGI(TAG,
             "MCP tools registered: reboot/wifi/goto/open_app + list_controls/control/schedule + skills + notice");
}
