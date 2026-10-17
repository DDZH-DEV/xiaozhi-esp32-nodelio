#include "panel_layout.h"

#include "board_info.h"
#include "layout_cache.h"
#include "nl_url_util.h"
#include "nodelio_bridge.h"
#include "nodelio_http.h"
#include "panel_ui.h"
#include "wifi_provision.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_layout";

static nodelio_nvs_config_t s_cfg;
static int s_applied_version = -1;
static volatile bool s_sync_kick;
static volatile bool s_snapshot_kick;
static bool s_layout_task_started;
static bool s_boot_cloud_done; /* 开机云端 layout+snapshot 已成功过一次 */
static int64_t s_last_snapshot_ms; /* MQTT 自动 snapshot 限频 */

/** MQTT 推送 snapshot 最短间隔；手动「刷新布局」不受限 */
#define NL_SNAPSHOT_MIN_INTERVAL_MS (60 * 1000)

/** cJSON 节点默认吃内部 DRAM；与 WS/MQTT 并行时易 tlsf assert → 改走 PSRAM. */
static void *cjson_malloc_psram(size_t sz)
{
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(sz);
    }
    return p;
}

static void cjson_free_psram(void *p)
{
    free(p);
}

static void ensure_cjson_psram_hooks(void)
{
    static bool once;
    if (once) {
        return;
    }
    once = true;
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_malloc_psram,
        .free_fn = cjson_free_psram,
    };
    cJSON_InitHooks(&hooks);
}

static void copy_str(char *dst, size_t dst_len, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, dst_len, "%s", v->valuestring);
    }
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        return v->valueint;
    }
    return def;
}

static nl_widget_type_t parse_widget_type(const char *t)
{
    if (!t) {
        return NL_WIDGET_UNKNOWN;
    }
    if (strcmp(t, "scene_chip") == 0) {
        return NL_WIDGET_SCENE_CHIP;
    }
    if (strcmp(t, "scene_grid") == 0) {
        return NL_WIDGET_SCENE_GRID;
    }
    if (strcmp(t, "switch_tile") == 0) {
        return NL_WIDGET_SWITCH_TILE;
    }
    if (strcmp(t, "pin_toggle") == 0) {
        return NL_WIDGET_PIN_TOGGLE;
    }
    if (strcmp(t, "slider_tile") == 0) {
        return NL_WIDGET_SLIDER_TILE;
    }
    if (strcmp(t, "status_badge") == 0) {
        return NL_WIDGET_STATUS_BADGE;
    }
    if (strcmp(t, "section_header") == 0) {
        return NL_WIDGET_SECTION_HEADER;
    }
    if (strcmp(t, "spacer") == 0) {
        return NL_WIDGET_SPACER;
    }
    if (strcmp(t, "skill_tile") == 0) {
        return NL_WIDGET_SKILL_TILE;
    }
    return NL_WIDGET_UNKNOWN;
}

static void parse_default_invoke(const cJSON *w, char *dst, size_t n)
{
    if (!dst || n == 0) {
        return;
    }
    snprintf(dst, n, "{}");
    if (!w) {
        return;
    }
    const cJSON *di = cJSON_GetObjectItemCaseSensitive(w, "defaultInvoke");
    if (cJSON_IsString(di) && di->valuestring && di->valuestring[0] == '{') {
        snprintf(dst, n, "%s", di->valuestring);
        return;
    }
    if (!cJSON_IsObject(di)) {
        return;
    }
    char *printed = cJSON_PrintUnformatted(di);
    if (!printed) {
        return;
    }
    if (printed[0] == '{') {
        snprintf(dst, n, "%s", printed);
    }
    cJSON_free(printed);
}

static void parse_bind(const cJSON *bind_obj, nl_bind_t *b)
{
    memset(b, 0, sizeof(*b));
    b->channel = -1;
    b->pin = -1;
    b->min = 0;
    b->max = 100;
    if (!cJSON_IsObject(bind_obj)) {
        b->kind = NL_BIND_NONE;
        return;
    }
    char kind[32];
    copy_str(kind, sizeof(kind), bind_obj, "kind");
    if (strcmp(kind, "home_scenario") == 0) {
        b->kind = NL_BIND_HOME_SCENARIO;
    } else if (strcmp(kind, "device_channel") == 0) {
        b->kind = NL_BIND_DEVICE_CHANNEL;
    } else if (strcmp(kind, "device_pin") == 0) {
        b->kind = NL_BIND_DEVICE_PIN;
    } else {
        b->kind = NL_BIND_NONE;
    }
    copy_str(b->device_id, sizeof(b->device_id), bind_obj, "deviceId");
    copy_str(b->name, sizeof(b->name), bind_obj, "name");
    copy_str(b->capability, sizeof(b->capability), bind_obj, "capability");
    b->channel = json_int(bind_obj, "channel", -1);
    b->pin = json_int(bind_obj, "pin", -1);
    b->min = json_int(bind_obj, "min", 0);
    b->max = json_int(bind_obj, "max", 100);
}

static bool grid_fits(const bool *occ, int cols, int rows, int c, int r, int gw, int gh)
{
    if (c < 0 || r < 0 || gw < 1 || gh < 1 || c + gw > cols || r + gh > rows) {
        return false;
    }
    for (int y = r; y < r + gh; y++) {
        for (int x = c; x < c + gw; x++) {
            if (occ[y * cols + x]) {
                return false;
            }
        }
    }
    return true;
}

static void grid_mark(bool *occ, int cols, int c, int r, int gw, int gh)
{
    for (int y = r; y < r + gh; y++) {
        for (int x = c; x < c + gw; x++) {
            occ[y * cols + x] = true;
        }
    }
}

static void layout_autopack(nodelio_panel_layout_t *out)
{
    if (!out || out->grid_cols == 0 || out->grid_rows == 0) {
        return;
    }
    const int cols = out->grid_cols;
    const int rows = out->grid_rows;
    const int cells = cols * rows;
    bool *occ = calloc((size_t)cells, sizeof(bool));
    if (!occ) {
        return;
    }

    for (int pi = 0; pi < out->page_count; pi++) {
        nl_page_t *p = &out->pages[pi];
        if (p->system_page[0] != '\0') {
            continue;
        }
        memset(occ, 0, (size_t)cells * sizeof(bool));
        /* 先钉死已有坐标 */
        for (int si = 0; si < p->section_count; si++) {
            nl_section_t *s = &p->sections[si];
            for (int wi = 0; wi < s->widget_count; wi++) {
                nl_widget_t *w = &s->widgets[wi];
                if (w->gw < 1) {
                    w->gw = (w->type == NL_WIDGET_SLIDER_TILE) ? 2 : 1;
                }
                if (w->gh < 1) {
                    w->gh = 1;
                }
                if (w->gw > cols) {
                    w->gw = (int8_t)cols;
                }
                if (w->gh > rows) {
                    w->gh = (int8_t)rows;
                }
                if (w->col >= 0 && w->row >= 0 && grid_fits(occ, cols, rows, w->col, w->row, w->gw, w->gh)) {
                    grid_mark(occ, cols, w->col, w->row, w->gw, w->gh);
                } else if (w->col >= 0 && w->row >= 0) {
                    w->col = -1;
                    w->row = -1;
                }
            }
        }
        /* 再装箱未定位的 */
        for (int si = 0; si < p->section_count; si++) {
            nl_section_t *s = &p->sections[si];
            for (int wi = 0; wi < s->widget_count; wi++) {
                nl_widget_t *w = &s->widgets[wi];
                if (w->col >= 0 && w->row >= 0) {
                    continue;
                }
                bool placed = false;
                for (int r = 0; r < rows && !placed; r++) {
                    for (int c = 0; c < cols && !placed; c++) {
                        if (grid_fits(occ, cols, rows, c, r, w->gw, w->gh)) {
                            w->col = (int8_t)c;
                            w->row = (int8_t)r;
                            grid_mark(occ, cols, c, r, w->gw, w->gh);
                            placed = true;
                        }
                    }
                }
                if (!placed) {
                    w->col = 0;
                    w->row = 0;
                    w->gw = 1;
                    w->gh = 1;
                }
            }
        }
    }
    free(occ);
}

static void parse_layout_object(const cJSON *root, int outer_version, nodelio_panel_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    const nodelio_board_t *board = nodelio_board_get();
    out->display_w = board->info.display_w;
    out->display_h = board->info.display_h;
    out->grid_cols = 6;
    out->grid_rows = 6;
    out->layout_version = outer_version > 0 ? outer_version : json_int(root, "layoutVersion", 1);

    const cJSON *theme = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if (cJSON_IsObject(theme)) {
        copy_str(out->theme_primary, sizeof(out->theme_primary), theme, "primary");
        copy_str(out->theme_bg, sizeof(out->theme_bg), theme, "bg");
    }
    if (out->theme_primary[0] == '\0') {
        snprintf(out->theme_primary, sizeof(out->theme_primary), "#34c759");
    }
    if (out->theme_bg[0] == '\0') {
        snprintf(out->theme_bg, sizeof(out->theme_bg), "#0a0f0c");
    }

    const cJSON *grid = cJSON_GetObjectItemCaseSensitive(root, "grid");
    if (cJSON_IsObject(grid)) {
        int gc = json_int(grid, "cols", 6);
        int gr = json_int(grid, "rows", 6);
        if (gc >= 2 && gc <= 12) {
            out->grid_cols = (uint8_t)gc;
        }
        if (gr >= 2 && gr <= 12) {
            out->grid_rows = (uint8_t)gr;
        }
    }

    const cJSON *dp = cJSON_GetObjectItemCaseSensitive(root, "displayProfile");
    if (cJSON_IsObject(dp)) {
        int w = json_int(dp, "w", out->display_w);
        int h = json_int(dp, "h", out->display_h);
        if (w > 0) {
            out->display_w = (uint16_t)w;
        }
        if (h > 0) {
            out->display_h = (uint16_t)h;
        }
    }

    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (!cJSON_IsArray(pages)) {
        return;
    }
    int user_pages = 0;
    int skipped_user = 0;
    const cJSON *page;
    cJSON_ArrayForEach(page, pages)
    {
        if (!cJSON_IsObject(page)) {
            continue;
        }
        char sys_peek[24];
        copy_str(sys_peek, sizeof(sys_peek), page, "systemPage");
        const bool is_sys = (sys_peek[0] != '\0');
        if (!is_sys) {
            if (user_pages >= NL_LAYOUT_MAX_USER_PAGES) {
                skipped_user++;
                continue;
            }
        }
        if (out->page_count >= NL_LAYOUT_MAX_PAGES) {
            if (!is_sys) {
                skipped_user++;
            } else {
                ESP_LOGW(TAG, "layout pages full; drop system page");
            }
            continue;
        }
        nl_page_t *p = &out->pages[out->page_count++];
        copy_str(p->id, sizeof(p->id), page, "id");
        copy_str(p->title, sizeof(p->title), page, "title");
        copy_str(p->system_page, sizeof(p->system_page), page, "systemPage");
        if (!is_sys) {
            user_pages++;
        }

        const cJSON *sections = cJSON_GetObjectItemCaseSensitive(page, "sections");
        if (!cJSON_IsArray(sections)) {
            continue;
        }
        const cJSON *sec;
        cJSON_ArrayForEach(sec, sections)
        {
            if (p->section_count >= NL_LAYOUT_MAX_SECTIONS || !cJSON_IsObject(sec)) {
                continue;
            }
            nl_section_t *s = &p->sections[p->section_count++];
            copy_str(s->id, sizeof(s->id), sec, "id");
            copy_str(s->title, sizeof(s->title), sec, "title");

            const cJSON *widgets = cJSON_GetObjectItemCaseSensitive(sec, "widgets");
            if (!cJSON_IsArray(widgets)) {
                continue;
            }
            const cJSON *w;
            cJSON_ArrayForEach(w, widgets)
            {
                if (s->widget_count >= NL_LAYOUT_MAX_WIDGETS || !cJSON_IsObject(w)) {
                    continue;
                }
                nl_widget_t *wid = &s->widgets[s->widget_count++];
                copy_str(wid->id, sizeof(wid->id), w, "id");
                copy_str(wid->title, sizeof(wid->title), w, "title");
                copy_str(wid->bg_color, sizeof(wid->bg_color), w, "bgColor");
                copy_str(wid->skin, sizeof(wid->skin), w, "skin");
                copy_str(wid->pinyin_on, sizeof(wid->pinyin_on), w, "pinyinOn");
                copy_str(wid->pinyin_off, sizeof(wid->pinyin_off), w, "pinyinOff");
                copy_str(wid->skill_id, sizeof(wid->skill_id), w, "skillId");
                parse_default_invoke(w, wid->default_invoke, sizeof(wid->default_invoke));
                const cJSON *lbls = cJSON_GetObjectItemCaseSensitive(w, "labels");
                if (cJSON_IsObject(lbls)) {
                    copy_str(wid->labels_on, sizeof(wid->labels_on), lbls, "on");
                    copy_str(wid->labels_off, sizeof(wid->labels_off), lbls, "off");
                }
                char tbuf[32];
                copy_str(tbuf, sizeof(tbuf), w, "type");
                wid->type = parse_widget_type(tbuf);
                parse_bind(cJSON_GetObjectItemCaseSensitive(w, "bind"), &wid->bind);
                if (wid->type == NL_WIDGET_SKILL_TILE && wid->skill_id[0] == '\0' &&
                    wid->bind.device_id[0] != '\0') {
                    /* 兼容误写 bind.deviceId；勿 snprintf 同源重叠（-Werror=restrict） */
                    char tmp[NL_WIDGET_SKILL_ID_MAX];
                    snprintf(tmp, sizeof(tmp), "%s", wid->bind.device_id);
                    snprintf(wid->skill_id, sizeof(wid->skill_id), "%s", tmp);
                }
                wid->col = (int8_t)json_int(w, "col", -1);
                wid->row = (int8_t)json_int(w, "row", -1);
                int def_w = (wid->type == NL_WIDGET_SLIDER_TILE) ? 2 : 1;
                wid->gw = (int8_t)json_int(w, "w", def_w);
                wid->gh = (int8_t)json_int(w, "h", 1);
                if (wid->gw < 1) {
                    wid->gw = (int8_t)def_w;
                }
                if (wid->gh < 1) {
                    wid->gh = 1;
                }
            }
        }
    }
    if (skipped_user > 0) {
        ESP_LOGW(TAG, "truncated %d user page(s); max=%d", skipped_user, NL_LAYOUT_MAX_USER_PAGES);
    }
    layout_autopack(out);
}

esp_err_t nodelio_layout_parse(const char *json, nodelio_panel_layout_t *out)
{
    ensure_cjson_psram_hooks();
    if (!json || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    int outer_ver = json_int(root, "layoutVersion", 0);
    const cJSON *layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    if (cJSON_IsObject(layout)) {
        parse_layout_object(layout, outer_ver, out);
    } else if (cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "pages"))) {
        parse_layout_object(root, outer_ver, out);
    } else {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t nodelio_layout_render(const nodelio_panel_layout_t *layout)
{
    if (!layout) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "render v%d pages=%d → UI", layout->layout_version, layout->page_count);
    return nodelio_panel_ui_apply_layout(layout);
}

esp_err_t nodelio_layout_fetch(const nodelio_nvs_config_t *cfg, int known_version, nodelio_panel_layout_t *out)
{
    if (!cfg || !out || cfg->server_url[0] == '\0' || cfg->device_id[0] == '\0' || cfg->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/layout", base, cfg->device_id);

    const int cap = 24 * 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    body[0] = '\0';

    char etag[32] = {0};
    if (known_version > 0) {
        snprintf(etag, sizeof(etag), "\"v%d\"", known_version);
    }
    int status = 0;
    esp_err_t err =
        nodelio_http_get(url, cfg->device_key, etag[0] ? etag : NULL, &status, body, cap);

    if (err != ESP_OK && status != 304) {
        ESP_LOGW(TAG, "GET layout failed: %s http=%d", esp_err_to_name(err), status);
        free(body);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    if (status == 304) {
        free(body);
        return ESP_ERR_NOT_FINISHED;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET layout HTTP %d", status);
        free(body);
        return ESP_FAIL;
    }

    err = nodelio_layout_parse(body, out);
    if (err == ESP_OK) {
        if (known_version > 0 && out->layout_version == known_version) {
            free(body);
            return ESP_ERR_NOT_FINISHED;
        }
        (void)nodelio_layout_cache_save(out->layout_version, body, strlen(body));
    }
    free(body);
    return err;
}

esp_err_t nodelio_panel_command(const nodelio_nvs_config_t *cfg, const char *widget_id, const char *action,
                                const nl_bind_t *bind, const char *value_str, int *http_status)
{
    if (!cfg || !bind || cfg->server_url[0] == '\0' || cfg->device_id[0] == '\0' || cfg->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!action) {
        action = "set";
    }
    if (!widget_id) {
        widget_id = "";
    }
    if (!value_str) {
        value_str = "";
    }

    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/command", base, cfg->device_id);

    const char *kind = "none";
    switch (bind->kind) {
    case NL_BIND_HOME_SCENARIO:
        kind = "home_scenario";
        break;
    case NL_BIND_DEVICE_CHANNEL:
        kind = "device_channel";
        break;
    case NL_BIND_DEVICE_PIN:
        kind = "device_pin";
        break;
    default:
        break;
    }

    char body[512];
    if (bind->kind == NL_BIND_HOME_SCENARIO) {
        const char *name = bind->name[0] ? bind->name : value_str;
        snprintf(body, sizeof(body),
                 "{\"widgetId\":\"%s\",\"action\":\"apply_home_scenario\",\"name\":\"%s\","
                 "\"bind\":{\"kind\":\"home_scenario\",\"name\":\"%s\"}}",
                 widget_id, name, name);
    } else if (bind->kind == NL_BIND_DEVICE_PIN && strcmp(bind->capability, "pwm") == 0) {
        snprintf(body, sizeof(body),
                 "{\"widgetId\":\"%s\",\"action\":\"%s\",\"value\":%s,"
                 "\"bind\":{\"kind\":\"device_pin\",\"deviceId\":\"%s\",\"pin\":%d,\"capability\":\"pwm\"}}",
                 widget_id, action, value_str[0] ? value_str : "0", bind->device_id, bind->pin);
    } else if (bind->kind == NL_BIND_DEVICE_CHANNEL) {
        snprintf(body, sizeof(body),
                 "{\"widgetId\":\"%s\",\"action\":\"%s\",\"value\":\"%s\","
                 "\"bind\":{\"kind\":\"device_channel\",\"deviceId\":\"%s\",\"channel\":%d}}",
                 widget_id, action, value_str[0] ? value_str : "TOGGLE", bind->device_id, bind->channel);
    } else if (bind->kind == NL_BIND_DEVICE_PIN) {
        snprintf(body, sizeof(body),
                 "{\"widgetId\":\"%s\",\"action\":\"%s\",\"value\":\"%s\","
                 "\"bind\":{\"kind\":\"device_pin\",\"deviceId\":\"%s\",\"pin\":%d,\"capability\":\"%s\"}}",
                 widget_id, action, value_str[0] ? value_str : "TOGGLE", bind->device_id, bind->pin,
                 bind->capability[0] ? bind->capability : "gpio_out");
    } else {
        ESP_LOGW(TAG, "command: unsupported bind kind=%s", kind);
        return ESP_ERR_INVALID_ARG;
    }

    int status = 0;
    esp_err_t err = nodelio_http_post_json(url, cfg->device_key, body, &status, NULL, 0);
    if (http_status) {
        *http_status = status;
    }
    if (err != ESP_OK && status != 503) {
        ESP_LOGW(TAG, "POST command failed: %s http=%d", esp_err_to_name(err), status);
        return err;
    }
    ESP_LOGI(TAG, "command widget=%s → HTTP %d", widget_id, status);
    if (status == 200) {
        /* 语音会话期 snapshot 让路；命令成功后立刻乐观刷首页，避免「已开关页面不变」 */
        nodelio_panel_ui_apply_bind_state(bind, value_str);
        return ESP_OK;
    }
    if (status == 503) {
        return ESP_ERR_INVALID_STATE; /* offline — UI 回滚 */
    }
    return ESP_FAIL;
}

esp_err_t nodelio_snapshot_fetch(const nodelio_nvs_config_t *cfg)
{
    if (!cfg || cfg->server_url[0] == '\0' || cfg->device_id[0] == '\0' || cfg->device_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    char url[256];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/snapshot", base, cfg->device_id);

    const int cap = 16 * 1024;
    char *body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = malloc(cap);
    }
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    body[0] = '\0';
    int status = 0;
    esp_err_t err = nodelio_http_get(url, cfg->device_key, NULL, &status, body, cap);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET snapshot fail err=%s http=%d", esp_err_to_name(err), status);
        free(body);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    /* cJSON 已钩 PSRAM；AFE 后内部常 <4KB，勿用 12KB 门槛直接放弃 */
    ensure_cjson_psram_hooks();
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        char snip[96];
        size_t n = strlen(body);
        if (n >= sizeof(snip)) {
            n = sizeof(snip) - 1;
        }
        memcpy(snip, body, n);
        snip[n] = '\0';
        ESP_LOGW(TAG, "snapshot JSON parse fail len=%u snip=%s", (unsigned)strlen(body), snip);
        free(body);
        return ESP_FAIL;
    }
    free(body);
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    int n = cJSON_IsArray(devices) ? cJSON_GetArraySize(devices) : 0;
    int online_n = 0;
    nodelio_panel_ui_snapshot_begin();
    /* 本地口令词表改由 layout 磁贴 title 注册；snapshot 只更新在线态 */
    if (cJSON_IsArray(devices) && n > 0) {
        const cJSON *d;
        cJSON_ArrayForEach(d, devices)
        {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(d, "deviceId");
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(d, "name");
            const cJSON *on = cJSON_GetObjectItemCaseSensitive(d, "online");
            const cJSON *pw = cJSON_GetObjectItemCaseSensitive(d, "powerState");
            const char *id_s = cJSON_IsString(id) ? id->valuestring : "";
            const char *name_s = cJSON_IsString(nm) ? nm->valuestring : "";
            bool online = cJSON_IsTrue(on);
            const char *power = cJSON_IsString(pw) ? pw->valuestring : "";
            if (online) {
                online_n++;
            }
            nodelio_panel_ui_snapshot_add(id_s, name_s, online, power);
            {
                const cJSON *cd = cJSON_GetObjectItemCaseSensitive(d, "countdown");
                if (cJSON_IsNumber(cd) && cd->valuedouble > 0) {
                    nodelio_panel_ui_note_countdown(id_s, -1, (int)cd->valuedouble);
                }
            }
            const cJSON *chs = cJSON_GetObjectItemCaseSensitive(d, "channels");
            if (cJSON_IsArray(chs)) {
                const cJSON *ch;
                cJSON_ArrayForEach(ch, chs)
                {
                    const cJSON *ix = cJSON_GetObjectItemCaseSensitive(ch, "index");
                    const cJSON *cn = cJSON_GetObjectItemCaseSensitive(ch, "name");
                    const cJSON *st = cJSON_GetObjectItemCaseSensitive(ch, "state");
                    const cJSON *ccd = cJSON_GetObjectItemCaseSensitive(ch, "countdown");
                    if (!cJSON_IsNumber(ix) || !cJSON_IsString(st)) {
                        continue;
                    }
                    int ch_idx = (int)ix->valuedouble;
                    const char *ch_name_s = cJSON_IsString(cn) ? cn->valuestring : "";
                    nodelio_panel_ui_snapshot_add_channel(id_s, ch_idx, ch_name_s, st->valuestring);
                    if (cJSON_IsNumber(ccd) && ccd->valuedouble > 0) {
                        nodelio_panel_ui_note_countdown(id_s, ch_idx, (int)ccd->valuedouble);
                    }
                }
            }
        }
    }
    nodelio_panel_ui_snapshot_end();
    ESP_LOGI(TAG, "snapshot devices=%d online=%d", n, online_n);
    cJSON_Delete(root);
    return ESP_OK;
}

/** @return 1=全量 sync（手动/layout 推送），0=只 snapshot（MQTT） */
static int layout_wait_kick(void)
{
    /* 无定时轮询：只等开机后的 MQTT / 设置页手动踢 */
    while (!s_sync_kick && !s_snapshot_kick) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (s_sync_kick) {
        s_sync_kick = false;
        s_snapshot_kick = false;
        return 1;
    }
    s_snapshot_kick = false;
    return 0;
}

static esp_err_t snapshot_fetch_limited(bool force)
{
    if (!force) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_last_snapshot_ms > 0 &&
            (now_ms - s_last_snapshot_ms) < (int64_t)NL_SNAPSHOT_MIN_INTERVAL_MS) {
            ESP_LOGD(TAG, "snapshot rate-limit skip (%lldms)",
                     (long long)(now_ms - s_last_snapshot_ms));
            return ESP_OK;
        }
    }
    esp_err_t err = nodelio_snapshot_fetch(&s_cfg);
    if (err == ESP_OK) {
        s_last_snapshot_ms = esp_timer_get_time() / 1000;
        s_boot_cloud_done = true;
    }
    return err;
}

static void layout_do_sync(nodelio_panel_layout_t *layout, bool toast_result)
{
    if (!nodelio_wifi_is_connected()) {
        ESP_LOGD(TAG, "layout sync skip (WiFi down)");
        if (toast_result) {
            nodelio_panel_ui_toast("未联网");
        }
        return;
    }
    esp_err_t err = nodelio_layout_fetch(&s_cfg, s_applied_version, layout);
    if (err == ESP_OK) {
        s_applied_version = layout->layout_version;
        nodelio_layout_render(layout);
        if (toast_result) {
            char msg[32];
            snprintf(msg, sizeof(msg), "已刷新 v%d", s_applied_version);
            nodelio_panel_ui_toast(msg);
        }
    } else if (err == ESP_ERR_NOT_FINISHED) {
        ESP_LOGD(TAG, "layout unchanged v%d", s_applied_version);
        if (toast_result) {
            char msg[32];
            snprintf(msg, sizeof(msg), "已是最新 v%d", s_applied_version > 0 ? s_applied_version : 0);
            nodelio_panel_ui_toast(msg);
        }
    } else {
        ESP_LOGW(TAG, "layout sync: %s", esp_err_to_name(err));
        if (toast_result) {
            nodelio_panel_ui_toast("刷新失败");
        }
    }
    err = snapshot_fetch_limited(true); /* 手动/layout 同步：强制拉 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshot: %s", esp_err_to_name(err));
    }
}

/** 注册成功后同步拉一次 layout+snapshot；幂等，避免与 register 并行竞态。 */
static void layout_boot_cloud_once(void)
{
    if (s_boot_cloud_done) {
        return;
    }
    if (!nodelio_wifi_is_connected() || !s_cfg.device_id[0] || !s_cfg.device_key[0]) {
        return;
    }
    nodelio_panel_layout_t *layout =
        heap_caps_malloc(sizeof(*layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!layout) {
        layout = malloc(sizeof(*layout));
    }
    if (!layout) {
        ESP_LOGE(TAG, "boot layout buf OOM");
        return;
    }
    layout_do_sync(layout, false);
    free(layout);
    if (s_boot_cloud_done) {
        ESP_LOGI(TAG, "boot cloud layout done v%d", s_applied_version);
    }
}

/** MQTT/手动踢；无定时轮询。开机云端同步在 start_poll→boot_cloud_once。 */
static void layout_sync_task(void *arg)
{
    (void)arg;
    nodelio_panel_layout_t *layout =
        heap_caps_malloc(sizeof(*layout), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!layout) {
        layout = malloc(sizeof(*layout));
    }
    if (!layout) {
        ESP_LOGE(TAG, "layout buf OOM");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        int kick = layout_wait_kick();
        /* 语音会话让路：等空闲再拉，避免 HTTP 抢 UDP 造成断续 */
        while (nodelio_voice_session_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (kick == 1) {
            layout_do_sync(layout, true);
        } else if (nodelio_wifi_is_connected()) {
            esp_err_t err = snapshot_fetch_limited(false); /* MQTT：限频 */
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "snapshot: %s", esp_err_to_name(err));
            }
        }
    }
}

void nodelio_layout_request_sync(void)
{
    s_sync_kick = true;
}

void nodelio_layout_request_snapshot(void)
{
    s_snapshot_kick = true;
}

esp_err_t nodelio_layout_start_poll(const nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    ensure_cjson_psram_hooks();

    if (!s_layout_task_started) {
        s_applied_version = -1;
        /* 先挂缓存：云未通时也能出上一版布局 */
        (void)nodelio_layout_cache_mount();
        char *cached = NULL;
        int cver = 0;
        if (nodelio_layout_cache_load(&cached, NULL, &cver) == ESP_OK && cached) {
            nodelio_panel_layout_t *tmp =
                heap_caps_malloc(sizeof(*tmp), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!tmp) {
                tmp = malloc(sizeof(*tmp));
            }
            if (tmp && nodelio_layout_parse(cached, tmp) == ESP_OK) {
                s_applied_version = tmp->layout_version;
                nodelio_layout_render(tmp);
                ESP_LOGI(TAG, "boot from cache v%d", s_applied_version);
            }
            free(tmp);
            free(cached);
        }

        BaseType_t ok = xTaskCreate(layout_sync_task, "nl_layout", 6144, NULL, 3, NULL);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        s_layout_task_started = true;
    }

    /* 注册/唤醒后同步拉一次；已成功则跳过，不再二次 kick */
    layout_boot_cloud_once();
    return ESP_OK;
}
