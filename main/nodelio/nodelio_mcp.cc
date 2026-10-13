#include "nodelio_mcp.h"

#include "nodelio_http.h"
#include "nvs_config.h"
#include "offline_cmd_map.h"
#include "panel_layout.h"
#include "panel_ui.h"

#include "mcp_server.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <string>
#include <unordered_set>

#define TAG "nl_mcp"

static void strip_base(const char *base, char *out, size_t out_len)
{
    if (!base || !out || out_len == 0) {
        return;
    }
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') {
        n--;
    }
    if (n >= 22 && strncmp(base + n - 22, "/api/devices/register", 22) == 0) {
        n -= 22;
    }
    snprintf(out, out_len, "%.*s", (int)n, base);
}

static bool load_cfg(nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    memset(cfg, 0, sizeof(*cfg));
    if (nodelio_nvs_load(cfg) != ESP_OK) {
        return false;
    }
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

static ToolResult run_widget_set(const nodelio_nvs_config_t *cfg, const nl_widget_t *w, const char *val)
{
    if (!w) {
        return std::unexpected(std::string("布局中未找到控件"));
    }
    if (!val) {
        if (w->bind.kind == NL_BIND_HOME_SCENARIO) {
            val = w->bind.name;
        } else {
            val = "TOGGLE";
        }
    }
    int http = 0;
    esp_err_t err = nodelio_panel_command(cfg, w->id, "set", &w->bind, val, &http);
    if (err != ESP_OK) {
        if (http == 503) {
            return std::unexpected(std::string("设备离线（请确认被控设备已联网上线）"));
        }
        return std::unexpected(std::string("执行失败"));
    }
    const char *name = w->title[0] ? w->title : w->bind.name;
    std::string ok = std::string("已控制：") + (name[0] ? name : w->id);
    nodelio_panel_ui_toast(ok.c_str());
    nodelio_panel_ui_set_voice_hint(ok.c_str());
    return ok;
}

static ToolResult run_unassigned_set(const nodelio_nvs_config_t *cfg, const UnassignedTarget &t, const char *val)
{
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
            return std::unexpected(std::string("设备离线（请确认被控设备已联网上线）"));
        }
        return std::unexpected(std::string("执行失败"));
    }
    std::string ok = std::string("已控制：") + t.title;
    nodelio_panel_ui_toast(ok.c_str());
    nodelio_panel_ui_set_voice_hint(ok.c_str());
    return ok;
}

static std::string list_controls_json()
{
    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return "[]";
    }

    std::unordered_set<std::string> bound_keys;

    // 1. 遍历当前布局排布的 widgets
    if (layout) {
        for (int pi = 0; pi < layout->page_count; pi++) {
            const nl_page_t *pg = &layout->pages[pi];
            for (int si = 0; si < pg->section_count; si++) {
                const nl_section_t *sec = &pg->sections[si];
                for (int wi = 0; wi < sec->widget_count; wi++) {
                    const nl_widget_t *w = &sec->widgets[wi];
                    if (w->type == NL_WIDGET_SECTION_HEADER || w->type == NL_WIDGET_SPACER ||
                        w->bind.kind == NL_BIND_NONE) {
                        continue;
                    }

                    if (w->bind.device_id[0]) {
                        if (w->bind.channel >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":" + std::to_string(w->bind.channel));
                        } else if (w->bind.pin >= 0) {
                            bound_keys.insert(std::string(w->bind.device_id) + ":pin" + std::to_string(w->bind.pin));
                        } else {
                            bound_keys.insert(std::string(w->bind.device_id));
                        }
                    }

                    cJSON *o = cJSON_CreateObject();
                    if (!o) {
                        continue;
                    }
                    cJSON_AddStringToObject(o, "id", w->id);
                    cJSON_AddStringToObject(o, "title", w->title[0] ? w->title : w->bind.name);
                    cJSON_AddStringToObject(o, "room_section", sec->title[0] ? sec->title : pg->title);
                    if (w->bind.device_id[0]) {
                        cJSON_AddStringToObject(o, "device_id", w->bind.device_id);
                    }
                    if (w->bind.channel >= 0) {
                        cJSON_AddNumberToObject(o, "channel", w->bind.channel);
                    } else if (w->bind.pin >= 0) {
                        cJSON_AddNumberToObject(o, "pin", w->bind.pin);
                    }

                    const char *kind = "switch";
                    const char *supported_actions = "ON, OFF, TOGGLE";
                    if (w->bind.kind == NL_BIND_HOME_SCENARIO) {
                        kind = "scene";
                        supported_actions = "apply";
                    } else if (w->type == NL_WIDGET_SLIDER_TILE ||
                               strcmp(w->bind.capability, "pwm") == 0) {
                        kind = "slider_pwm";
                        supported_actions = "ON, OFF, SET_PWM (value 0-100)";
                    } else if (w->bind.kind == NL_BIND_DEVICE_PIN) {
                        kind = "gpio";
                        supported_actions = "ON, OFF, TOGGLE";
                    }
                    cJSON_AddStringToObject(o, "kind", kind);
                    cJSON_AddStringToObject(o, "actions", supported_actions);
                    cJSON_AddItemToArray(arr, o);
                }
            }
        }
    }

    // 2. 遍历快照缓存中未排布在画板上的设备与通道
    int dev_cnt = nodelio_panel_ui_get_dev_count();
    for (int i = 0; i < dev_cnt; i++) {
        const nl_dev_cache_t *d = nodelio_panel_ui_get_dev(i);
        if (!d || !d->id[0]) {
            continue;
        }
        if (d->ch_n > 0) {
            for (int ci = 0; ci < d->ch_n; ci++) {
                int ch_idx = d->ch_idx[ci];
                std::string key = std::string(d->id) + ":" + std::to_string(ch_idx);
                if (bound_keys.count(key)) {
                    continue;
                }
                cJSON *o = cJSON_CreateObject();
                if (!o) {
                    continue;
                }
                std::string cid = std::string(d->id) + "_ch" + std::to_string(ch_idx);
                cJSON_AddStringToObject(o, "id", cid.c_str());
                std::string title = d->ch_name[ci][0]
                                        ? d->ch_name[ci]
                                        : (std::string(d->name[0] ? d->name : d->id) + " 通道" + std::to_string(ch_idx + 1));
                cJSON_AddStringToObject(o, "title", title.c_str());
                cJSON_AddStringToObject(o, "room_section", "全屋设备(未排布)");
                cJSON_AddStringToObject(o, "device_id", d->id);
                cJSON_AddNumberToObject(o, "channel", ch_idx);
                cJSON_AddStringToObject(o, "kind", "switch");
                cJSON_AddStringToObject(o, "actions", "ON, OFF, TOGGLE");
                cJSON_AddItemToArray(arr, o);
            }
        } else {
            std::string key = std::string(d->id);
            if (bound_keys.count(key)) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            if (!o) {
                continue;
            }
            cJSON_AddStringToObject(o, "id", d->id);
            cJSON_AddStringToObject(o, "title", d->name[0] ? d->name : d->id);
            cJSON_AddStringToObject(o, "room_section", "全屋设备(未排布)");
            cJSON_AddStringToObject(o, "device_id", d->id);
            cJSON_AddStringToObject(o, "kind", "switch");
            cJSON_AddStringToObject(o, "actions", "ON, OFF, TOGGLE");
            cJSON_AddItemToArray(arr, o);
        }
    }

    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string out = printed ? printed : "[]";
    free(printed);
    ESP_LOGI(TAG, "list_controls count=%s", out.c_str());
    return out;
}

static ToolResult invoke_skill(const nodelio_nvs_config_t *cfg, const std::string &skill_id,
                               const std::string &params)
{
    if (skill_id.empty()) {
        return std::unexpected(std::string("skill_id 不能为空"));
    }
    char base[160];
    strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base, cfg->device_id,
             skill_id.c_str());

    std::string body = params.empty() ? "{}" : params;
    if (body.front() != '{') {
        body = "{\"query\":\"" + body + "\"}";
    }

    char resp[2048] = {0};
    int status = 0;
    esp_err_t err = nodelio_http_post_json(url, cfg->device_key, body.c_str(), &status, resp, sizeof(resp) - 1);
    if (err != ESP_OK || (status != 200 && status != 201)) {
        ESP_LOGW(TAG, "invoke skill '%s' fail: status=%d err=%d", skill_id.c_str(), status, err);
        return std::unexpected(std::string("调用技能失败：HTTP ") + std::to_string(status));
    }
    return std::string(resp);
}

static std::string list_skills_json(const nodelio_nvs_config_t *cfg)
{
    char base[160];
    strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);

    char resp[4096] = {0};
    int status = 0;
    esp_err_t err = nodelio_http_get(url, cfg->device_key, NULL, &status, resp, sizeof(resp) - 1);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "list skills fail: status=%d err=%d", status, err);
        return "[]";
    }
    return std::string(resp);
}

void nodelio_mcp_register_tools(void)
{
    auto &mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.nodelio.list_controls",
        "Get all available smart-home devices/channels/scenes in NodeLio. "
        "Returns JSON array: [{id, title, room_section, kind, actions}]. "
        "Call this to inspect device names (e.g. 厨房灯, 卧室灯, 客厅主灯) and supported actions before controlling. "
        "If user says 开灯/关灯 without room and multiple lights exist, you can ask which room/device to control.",
        PropertyList(), [](const PropertyList &) -> ReturnValue { return list_controls_json(); });

    mcp.AddTool(
        "self.nodelio.control",
        "Control a device, channel or scene via NodeLio wall-panel. "
        "Parameters: "
        "- target_title: device/channel title or id (e.g. '客厅主灯', '厨房灯', '回家') "
        "- action: 'ON', 'OFF', 'TOGGLE', 'SET_PWM', or 'apply' "
        "- value: optional string parameter, e.g. '75' for 75% PWM brightness or speed, 'ON'/'OFF' "
        "- phrase: fallback user voice text if target_title cannot be determined.",
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
            const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();

            std::string title = properties["target_title"].value<std::string>();
            std::string action = properties["action"].value<std::string>();
            std::string value = properties["value"].value<std::string>();
            std::string phrase = properties["phrase"].value<std::string>();

            ESP_LOGI(TAG, "control target_title='%s' action='%s' value='%s' phrase='%s'",
                     title.c_str(), action.c_str(), value.c_str(), phrase.c_str());

            const char *val = value.empty() ? (action.empty() ? "TOGGLE" : action.c_str()) : value.c_str();

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
                    if (layout) {
                        const nl_widget_t *w = find_widget_by_title(layout, hint);
                        if (w) {
                            const char *fallback_val = value.empty()
                                                          ? (phrase_implies_on(phrase.c_str()) ? "ON" : "OFF")
                                                          : value.c_str();
                            ESP_LOGI(TAG, "fallback by title_hint='%s' → %s", hint, w->id);
                            return run_widget_set(&cfg, w, fallback_val);
                        }
                    }
                    UnassignedTarget unassigned;
                    if (find_unassigned_target(hint, unassigned)) {
                        const char *fallback_val = value.empty()
                                                      ? (phrase_implies_on(phrase.c_str()) ? "ON" : "OFF")
                                                      : value.c_str();
                        ESP_LOGI(TAG, "fallback unassigned by title_hint='%s' → %s", hint, unassigned.title.c_str());
                        return run_unassigned_set(&cfg, unassigned, fallback_val);
                    }
                }

                // 3. 仅作为离线短语最后的兜底尝试（若全无指定目标）
                if (layout) {
                    esp_err_t err = nodelio_offline_try_phrase(&cfg, phrase.c_str(), layout);
                    if (err == ESP_OK) {
                        std::string ok = std::string("已执行：") + phrase;
                        nodelio_panel_ui_toast(ok.c_str());
                        return ok;
                    }
                    if (err == ESP_ERR_INVALID_STATE) {
                        return std::unexpected(std::string("设备离线（请确认被控设备已联网上线）"));
                    }
                }
            }

            return std::unexpected(std::string(
                "未找到对应设备。请先调用 list_controls 获取设备列表，确认名称（如厨房灯、卧室灯）。"));
        });

    mcp.AddTool(
        "self.skill.invoke",
        "Invoke an external dynamic skill registered in NodeLio (e.g. online music search, weather, etc.). "
        "Parameters: "
        "- skill_id: unique skill identifier (e.g. 'music_search') "
        "- params: JSON object string of input parameters, e.g. '{\"keyword\":\"周杰伦\"}'",
        PropertyList({
            Property("skill_id", kPropertyTypeString, ""),
            Property("params", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(std::string("未绑定 NodeLio 服务"));
            }
            std::string skill_id = properties["skill_id"].value<std::string>();
            std::string params = properties["params"].value<std::string>();
            return invoke_skill(&cfg, skill_id, params);
        });

    mcp.AddTool(
        "self.skill.list",
        "List all available dynamic skills enabled for this panel. Returns JSON list of skills and descriptions.",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return "[]";
            }
            return list_skills_json(&cfg);
        });

    ESP_LOGI(TAG, "MCP tools registered: nodelio controls & dynamic skills");
}
