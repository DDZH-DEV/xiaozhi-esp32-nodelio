#include "nodelio_mcp.h"

#include "content_envelope.h"
#include "device_caps.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "panel_layout.h"
#include "panel_ui.h"
#include "media_player.h"
#include "skill_result_ui.h"
#include "skill_runtime.h"

#include "mcp_server.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <unordered_set>

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
    if (!bind || bind->kind == NL_BIND_HOME_SCENARIO || !bind->device_id[0]) {
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
                    } else if (w->type == NL_WIDGET_SLIDER_TILE ||
                               strcmp(w->bind.capability, "pwm") == 0) {
                        kind = "pwm";
                    }
                    const char *room = sec->title[0] ? sec->title : pg->title;
                    const char *dev =
                        (w->bind.kind == NL_BIND_HOME_SCENARIO) ? nullptr : w->bind.device_id;
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

    /* 硬顶：MQTT MCP 常见 ~8KB；留余量给 jsonrpc 外壳 */
    constexpr size_t kMaxBytes = 3500;
    if (out.size() > kMaxBytes) {
        ESP_LOGW(TAG, "list_controls truncated %u→%u bytes (n=%d)", (unsigned)out.size(),
                 (unsigned)kMaxBytes, n);
        /* 粗暴截到合法 JSON 数组：找到最后一个完整对象 */
        size_t cut = out.rfind("},", kMaxBytes);
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
    } else if (low == "weather") {
        canon = "weather.today";
    } else if (low == "air" || low == "aqi") {
        canon = "air.now";
    } else if (low == "quote" || low == "daily") {
        canon = "quote.daily";
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

static ToolResult invoke_skill(const nodelio_nvs_config_t *cfg, const std::string &skill_id_in,
                               const std::string &params)
{
    if (skill_id_in.empty()) {
        return std::unexpected(std::string("skill_id 不能为空"));
    }
    const std::string skill_id = normalize_skill_id_alias(skill_id_in);
    const bool music_app = nodelio_skill_id_is_music(skill_id.c_str());
    if (!music_app && nodelio_media_preempt_for_heavy_app()) {
        ESP_LOGI(TAG, "preempt media before skill '%s'", skill_id.c_str());
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
    bool force_play = false;
    if (music_app) {
        body = music_normalize_invoke_body(body, &force_play);
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
    esp_err_t err =
        nodelio_http_post_json_ms(url, cfg->device_key, body.c_str(), &status, resp, cap - 1, 10000);
    if (err != ESP_OK || (status != 200 && status != 201)) {
        ESP_LOGW(TAG, "invoke skill '%s' fail: status=%d err=%d", skill_id.c_str(), status, err);
        free(resp);
        if (status == 404) {
            return std::unexpected(std::string("未安装该技能，请先在应用市场安装"));
        }
        if (status == 0 || (err != ESP_OK && status < 400)) {
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

    /* 双投递：UI Envelope + 模型短摘要（禁止只塞整段 JSON） */
    char spoken[NL_ENV_SPOKEN];
    /* 打开曲库/点播别名 force；「有什么歌」等浏览仍跟 Envelope auto_play。 */
    nodelio_skill_deliver_json(cfg, resp, force_play, spoken, sizeof(spoken));
    free(resp);
    ESP_LOGI(TAG, "invoke skill '%s' force=%d spoken=%s", skill_id.c_str(), force_play ? 1 : 0,
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
    esp_err_t err = nodelio_http_get(url, cfg->device_key, NULL, &status, resp, kCap - 1);
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

void nodelio_mcp_register_tools(void)
{
    auto &mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.nodelio.reboot",
        "重启本机墙板。用户说重启/重新启动时调用。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_panel_ui_local_reboot();
            return std::string("正在重启");
        });

    mcp.AddTool(
        "self.nodelio.enter_wifi_config",
        "进入 SoftAP 配网热点。用户说进入配网/打开热点时调用。屏上网络表单用 goto_page connect。",
        PropertyList(), [](const PropertyList &) -> ReturnValue {
            nodelio_panel_ui_local_wifi_config();
            return std::string("已进入配网热点，请手机连接后打开 192.168.4.1");
        });

    mcp.AddTool(
        "self.nodelio.goto_page",
        "切墙板页面（不结束对话）。page=home|apps|settings|connect|display|power|voice|media|"
        "controls|system|reset。reset=恢复出厂确认页（须触屏确认）。"
        "只切底层 Tab，不会关掉技能弹窗；用户说关闭电台/播客/曲库/新闻时必须再调 self.skill.close。"
        "打开后继续听下一句。",
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
        "翻首页用户屏（与左滑下一屏、右滑上一屏相同）。用户说上一屏/下一屏/往前翻/往后翻时调用。"
        "dir=next|prev。不在首页时先回到首页再翻。到头或只有一屏时说明原因，不结束对话。",
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
        "竖向滚动当前可见页（设置首页/设置内页/网络连接/应用列表/启动台）。"
        "用户说往上翻/往下翻/向上滑/向下滑时调用。dir=up|down。"
        "首页有磁贴网格时通常不能滚（用 home_screen 换屏）。到顶/底说明原因，不结束对话。",
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
        "改设置（不结束对话）。空闲断网→idle_wifi_s=秒(15-300)；息屏→idle_sleep_s；"
        "退下息屏→bye_sleep_s；另 brightness/volume/power_save/controls/xiaozhi/local_cmd/"
        "listen_idle_s/media_sleep|media_pause_s；动作 layout_sync|fw_check|factory_reset。",
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
                return std::string(strcasecmp(value.c_str(), "on") == 0 || value == "1" || value == "开"
                                       ? "已启用本地口令"
                                       : "已关闭本地口令");
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
        "打开已装应用并执行（曲库/音乐会自动开播）。name=曲库/音乐/新闻/天气/电台/播客/空气/每日一句或 "
        "app_id。用户说打开音乐/曲库优先本工具。仅列应用页用 goto_page apps。",
        PropertyList({Property("name", kPropertyTypeString)}),
        [](const PropertyList &properties) -> ToolResult {
            std::string name = properties["name"].value<std::string>();
            if (name.empty()) {
                return std::unexpected(std::string("name 不能为空"));
            }
            if (!nodelio_panel_ui_open_app(name.c_str())) {
                return std::unexpected(std::string("无法打开应用：") + name);
            }
            return std::string("正在打开 ") + name;
        });

    mcp.AddTool(
        "self.nodelio.list_controls",
        "列出首页可控设备 JSON[{title,room?,kind?,online?}]。offline 勿控。配合 self.nodelio.control。",
        PropertyList(), [](const PropertyList &) -> ReturnValue { return list_controls_json(); });

    mcp.AddTool(
        "self.nodelio.control",
        "控家：target_title=名称；action=ON|OFF|TOGGLE|SET_PWM|OFF_IN|CANCEL_COUNTDOWN|apply；"
        "value=OFF_IN秒或PWM0-100；phrase=备用口令。离线目标勿控。",
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
        "self.skill.invoke",
        "调用已装技能。skill_id 须用完整 id：music.search / radio.stations / podcast.latest / "
        "news.latest / weather.today / air.now / quote.daily（短写 radio/music 固件会归一）。"
        "打开曲库/电台/播客优先 open_app。曲库 keyword 空=自动播；有什么歌=只列表。"
        "列表已开随便放→transport op=play。",
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
        "self.skill.transport",
        "面板当前歌单播控。op=prev|next|pause|resume|play|stop|close。"
        "stop/close=停播并关掉技能弹窗（关闭电台/播客/曲库时用，勿只 goto_page）。"
        "随便放一首用 play，勿重新搜索。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
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
            return std::unexpected(std::string("op 须为 prev/next/pause/resume/play/stop/close"));
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
        "self.skill.news",
        "新闻卡朗读：op=read 开读；有【续】再 continue；见【完】/【摘要】则停。"
        "勿在朗读前说已读完。prev|next|mark_read|skip|close。未开卡先 invoke news.latest。",
        PropertyList({
            Property("op", kPropertyTypeString, ""),
        }),
        [](const PropertyList &properties) -> ToolResult {
            std::string op = properties["op"].value<std::string>();
            if (op != "prev" && op != "next" && op != "read" && op != "continue" && op != "mark_read" &&
                op != "mark" && op != "skip" && op != "close" && op != "dismiss") {
                return std::unexpected(std::string("op 须为 prev/next/read/continue/mark_read/skip/close"));
            }
            // 分段回包：约 900 字/段，避免整篇顶爆 MQTT。
            constexpr size_t kNewsSpokenCap = 4096;
            char *spoken = static_cast<char *>(
                heap_caps_malloc(kNewsSpokenCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!spoken) {
                spoken = static_cast<char *>(malloc(kNewsSpokenCap));
            }
            if (!spoken) {
                return std::unexpected(std::string("内存不足"));
            }
            spoken[0] = '\0';
            const bool ok = nodelio_skill_news_action(op.c_str(), spoken, kNewsSpokenCap);
            std::string out = spoken;
            heap_caps_free(spoken);
            if (!ok) {
                if (op == "prev") {
                    return std::unexpected(std::string("没有上一篇"));
                }
                if (op == "continue") {
                    return std::unexpected(std::string("没有更多正文"));
                }
                if (op == "mark_read" || op == "mark") {
                    return std::unexpected(std::string("没有可标记的新闻"));
                }
                return std::unexpected(std::string("没有下一篇"));
            }
            if (out.empty()) {
                return std::unexpected(std::string("正文加载失败"));
            }
            return out;
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

    ESP_LOGI(TAG,
             "MCP tools registered: reboot/wifi/goto/open_app + list_controls/control/schedule + skills");
}
