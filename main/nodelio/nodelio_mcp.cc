#include "nodelio_mcp.h"

#include "nvs_config.h"
#include "offline_cmd_map.h"
#include "panel_layout.h"
#include "panel_ui.h"

#include "mcp_server.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <string>

#define TAG "nl_mcp"

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
    for (int pi = 0; pi < layout->page_count; pi++) {
        const nl_page_t *pg = &layout->pages[pi];
        for (int si = 0; si < pg->section_count; si++) {
            const nl_section_t *sec = &pg->sections[si];
            for (int wi = 0; wi < sec->widget_count; wi++) {
                const nl_widget_t *w = &sec->widgets[wi];
                if (w->title[0] && strstr(w->title, title)) {
                    return w;
                }
                if (w->bind.name[0] && strstr(w->bind.name, title)) {
                    return w;
                }
                if (w->id[0] && strcmp(w->id, title) == 0) {
                    return w;
                }
            }
        }
    }
    return nullptr;
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

static std::string list_controls_json()
{
    const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return "[]";
    }
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
                    cJSON *o = cJSON_CreateObject();
                    if (!o) {
                        continue;
                    }
                    cJSON_AddStringToObject(o, "id", w->id);
                    cJSON_AddStringToObject(o, "title", w->title[0] ? w->title : w->bind.name);
                    const char *kind = "switch";
                    const char *use_for = "开灯/关灯/打开/关闭";
                    if (w->bind.kind == NL_BIND_HOME_SCENARIO) {
                        kind = "scene";
                        use_for = "回家/出门/情景";
                    } else if (w->type == NL_WIDGET_SLIDER_TILE ||
                               strcmp(w->bind.capability, "pwm") == 0) {
                        kind = "slider";
                        use_for = "调节亮度/开合";
                    } else if (w->bind.kind == NL_BIND_DEVICE_PIN) {
                        kind = "gpio";
                        use_for = "开灯/关灯/打开/关闭";
                    }
                    cJSON_AddStringToObject(o, "kind", kind);
                    cJSON_AddStringToObject(o, "use_for", use_for);
                    cJSON_AddItemToArray(arr, o);
                }
            }
        }
    }
    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string out = printed ? printed : "[]";
    free(printed);
    ESP_LOGI(TAG, "list_controls %s", out.c_str());
    return out;
}

void nodelio_mcp_register_tools(void)
{
    auto &mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.nodelio.list_controls",
        "List NodeLio wall-panel controls (JSON: id, title, kind, use_for). "
        "switch/gpio titles like 继电器开关 can be used when user says 开灯/关灯/打开继电器. "
        "After listing, you MUST call self.nodelio.control — do not only apologize.",
        PropertyList(), [](const PropertyList &) -> ReturnValue { return list_controls_json(); });

    mcp.AddTool(
        "self.nodelio.control",
        "Control smart-home via NodeLio panel/command. "
        "When user says 开灯/关灯/打开继电器/关闭开关/回家/出门: ALWAYS call this tool. "
        "Prefer phrase=user text (开灯 works for any switch including 继电器). "
        "Or widget_title=exact/partial title from list_controls + value=ON/OFF/TOGGLE.",
        PropertyList({
            Property("phrase", kPropertyTypeString, ""),
            Property("widget_title", kPropertyTypeString, ""),
            Property("value", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            nodelio_nvs_config_t cfg{};
            if (!load_cfg(&cfg)) {
                return std::unexpected(
                    std::string("未绑定 NodeLio：请点右下角「家」→ 设置填服务器与绑定码"));
            }
            const nodelio_panel_layout_t *layout = nodelio_panel_ui_get_layout();
            if (!layout) {
                return std::unexpected(std::string("暂无布局：请先联网并在设置里「刷新布局」"));
            }

            std::string phrase = properties["phrase"].value<std::string>();
            std::string title = properties["widget_title"].value<std::string>();
            std::string value = properties["value"].value<std::string>();

            ESP_LOGI(TAG, "control phrase='%s' title='%s' value='%s'", phrase.c_str(), title.c_str(),
                     value.c_str());

            if (!phrase.empty()) {
                esp_err_t err = nodelio_offline_try_phrase(&cfg, phrase.c_str(), layout);
                if (err == ESP_OK) {
                    std::string ok = std::string("已执行：") + phrase;
                    nodelio_panel_ui_toast(ok.c_str());
                    return ok;
                }
                if (err == ESP_ERR_INVALID_STATE) {
                    return std::unexpected(std::string("设备离线（请确认被控设备已联网上线）"));
                }
                if (err == ESP_ERR_NOT_FOUND) {
                    return std::unexpected(std::string(
                        "布局中没有可控制的开关。请用 APP 添加 switch_tile，或用 widget_title"));
                }
                /* INVALID_ARG：口令未收录 → 按标题关键字回退 */
                char hint[48];
                extract_title_hint(phrase.c_str(), hint, sizeof(hint));
                if (hint[0]) {
                    const nl_widget_t *w = find_widget_by_title(layout, hint);
                    if (w) {
                        const char *val = value.empty()
                                              ? (phrase_implies_on(phrase.c_str()) ? "ON" : "OFF")
                                              : value.c_str();
                        ESP_LOGI(TAG, "phrase fallback title_hint='%s' → %s", hint, w->id);
                        return run_widget_set(&cfg, w, val);
                    }
                }
                return std::unexpected(std::string(
                    "无法理解口令。可说：开灯/关灯/打开继电器/关闭开关/回家/出门；或先 "
                    "list_controls 再用 widget_title"));
            }

            if (title.empty()) {
                return std::unexpected(std::string("请提供 phrase 或 widget_title"));
            }
            const nl_widget_t *w = find_widget_by_title(layout, title.c_str());
            if (!w) {
                return std::unexpected(std::string("布局中未找到：") + title);
            }
            const char *val = value.empty() ? nullptr : value.c_str();
            return run_widget_set(&cfg, w, val);
        });

    ESP_LOGI(TAG, "MCP tools registered: self.nodelio.list_controls / control");
}
