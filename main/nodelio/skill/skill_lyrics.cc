#include "skill_lyrics.h"

#include "application.h"
#include "content_envelope.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"

#include <cJSON.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "nl_lyric"

extern lv_obj_t *s_now_sub;

namespace {

constexpr int kLineMax = 100;
constexpr int kTextMax = 84;
constexpr int kRespCap = 48 * 1024;

struct lyric_line_t {
    uint32_t ms;
    char text[kTextMax];
};

struct lyric_job_t {
    uint32_t gen;
    char url[1024];
    char device_key[65];
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
};

EXT_RAM_BSS_ATTR lyric_line_t s_lines[kLineMax];
/* 只在 nl_lyric 里写。不能放进 lyric_worker 栈：100 行约 8.8KB，任务栈只有 8KB。 */
EXT_RAM_BSS_ATTR lyric_line_t s_parse_lines[kLineMax];
int s_line_n = 0;
bool s_synced = false;
uint32_t s_gen = 0;
char s_key[NL_ENV_TITLE + NL_ENV_SUB + 4];
SemaphoreHandle_t s_mu = nullptr;
QueueHandle_t s_q = nullptr;
lv_timer_t *s_timer = nullptr;
char s_shown[kTextMax];

void ensure_mu()
{
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
}

void url_encode(const char *in, char *out, size_t out_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    if (!out || out_len == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p && o + 1 < out_len; ++p) {
        const unsigned char c = *p;
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out[o++] = static_cast<char>(c);
            continue;
        }
        if (o + 3 >= out_len) {
            break;
        }
        out[o++] = '%';
        out[o++] = hex[c >> 4];
        out[o++] = hex[c & 0x0f];
    }
    out[o] = '\0';
}

void copy_key(const char *title, const char *artist, char *out, size_t n)
{
    snprintf(out, n, "%s\n%s", title ? title : "", artist ? artist : "");
}

/** 显式截断，躲 -Werror=format-truncation */
void clip_into(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%.*s", static_cast<int>(dst_len - 1), src);
}

/* 编码后 title/artist 上限 + 固定前缀 */
constexpr size_t kLyricUrlCap = NL_ENV_TITLE * 3 + NL_ENV_SUB * 3 + 128;

int pick_index(uint32_t pos, int n, bool synced)
{
    if (n <= 0) {
        return -1;
    }
    if (!synced) {
        /* 无时间戳：按播放进度均分，勿用墙上时钟（沉浸歌词会对不上） */
        uint32_t dur = Application::GetInstance().HttpAudioDurationMs();
        if (dur == 0) {
            dur = static_cast<uint32_t>(n) * 4000u;
        }
        uint32_t idx = (pos * static_cast<uint32_t>(n)) / dur;
        if (idx >= static_cast<uint32_t>(n)) {
            idx = static_cast<uint32_t>(n) - 1;
        }
        return static_cast<int>(idx);
    }
    int idx = -1;
    for (int i = 0; i < n; ++i) {
        if (s_lines[i].ms <= pos) {
            idx = i;
        } else {
            break;
        }
    }
    return idx;
}

void lyric_timer_cb(lv_timer_t *)
{
    if (!nodelio_media_is_playing()) {
        return;
    }
    nodelio_lyrics_apply(s_now_sub);
}

void ensure_timer()
{
    if (s_timer) {
        return;
    }
    s_timer = lv_timer_create(lyric_timer_cb, 300, nullptr);
}

char *alloc_buf()
{
    char *buf = static_cast<char *>(heap_caps_malloc(kRespCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<char *>(malloc(kRespCap));
    }
    return buf;
}

void sort_by_ms(int n)
{
    for (int i = 1; i < n; ++i) {
        lyric_line_t tmp = s_parse_lines[i];
        int j = i - 1;
        while (j >= 0 && s_parse_lines[j].ms > tmp.ms) {
            s_parse_lines[j + 1] = s_parse_lines[j];
            --j;
        }
        s_parse_lines[j + 1] = tmp;
    }
}

int frac_to_ms(const char *frac)
{
    if (!frac || !frac[0]) {
        return 0;
    }
    const size_t n = strlen(frac);
    int v = atoi(frac);
    if (n <= 1) {
        return v * 100;
    }
    if (n == 2) {
        return v * 10;
    }
    if (n == 3) {
        return v;
    }
    /* 截到毫秒 */
    char tmp[4] = {frac[0], frac[1], frac[2], '\0'};
    return atoi(tmp);
}

bool parse_stamp(const char *p, int *min_out, int *sec_out, int *ms_out, const char **end)
{
    if (!p || *p != '[') {
        return false;
    }
    const char *q = p + 1;
    char *e = nullptr;
    long min_n = strtol(q, &e, 10);
    if (!e || *e != ':') {
        return false;
    }
    q = e + 1;
    long sec_n = strtol(q, &e, 10);
    if (!e || (*e != ':' && *e != '.' && *e != ']')) {
        return false;
    }
    int ms = 0;
    if (*e == ':' || *e == '.') {
        q = e + 1;
        const char *frac = q;
        while (*q && *q != ']') {
            ++q;
        }
        if (*q != ']') {
            return false;
        }
        char frac_buf[8];
        size_t flen = static_cast<size_t>(q - frac);
        if (flen >= sizeof(frac_buf)) {
            flen = sizeof(frac_buf) - 1;
        }
        memcpy(frac_buf, frac, flen);
        frac_buf[flen] = '\0';
        ms = frac_to_ms(frac_buf);
        e = const_cast<char *>(q);
    }
    if (*e != ']') {
        return false;
    }
    *min_out = static_cast<int>(min_n);
    *sec_out = static_cast<int>(sec_n);
    *ms_out = ms;
    *end = e + 1;
    return true;
}

int parse_synced_lrc(const char *raw)
{
    if (!raw || !raw[0]) {
        return 0;
    }
    int n = 0;
    const char *line = raw;
    while (*line && n < kLineMax) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? static_cast<size_t>(nl - line) : strlen(line);
        char row[512];
        if (len >= sizeof(row)) {
            len = sizeof(row) - 1;
        }
        memcpy(row, line, len);
        row[len] = '\0';
        if (len > 0 && row[len - 1] == '\r') {
            row[len - 1] = '\0';
        }

        const char *p = row;
        int stamp_ms[8];
        int stamp_n = 0;
        const char *text_start = row;
        while (*p == '[') {
            int min_n = 0, sec_n = 0, ms = 0;
            const char *end = nullptr;
            if (!parse_stamp(p, &min_n, &sec_n, &ms, &end)) {
                break;
            }
            if (stamp_n < 8) {
                int t = min_n * 60000 + sec_n * 1000 + ms;
                if (t < 0) {
                    t = 0;
                }
                stamp_ms[stamp_n++] = t;
            }
            p = end;
            text_start = end;
        }
        if (stamp_n > 0) {
            while (*text_start == ' ' || *text_start == '\t') {
                ++text_start;
            }
            if (text_start[0]) {
                for (int i = 0; i < stamp_n && n < kLineMax; ++i) {
                    s_parse_lines[n].ms = static_cast<uint32_t>(stamp_ms[i]);
                    clip_into(s_parse_lines[n].text, sizeof(s_parse_lines[n].text), text_start);
                    ++n;
                }
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    if (n > 1) {
        sort_by_ms(n);
    }
    return n;
}

int parse_plain_text(const char *raw)
{
    if (!raw || !raw[0]) {
        return 0;
    }
    int n = 0;
    const char *line = raw;
    while (*line && n < kLineMax) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? static_cast<size_t>(nl - line) : strlen(line);
        char row[256];
        if (len >= sizeof(row)) {
            len = sizeof(row) - 1;
        }
        memcpy(row, line, len);
        row[len] = '\0';
        if (len > 0 && row[len - 1] == '\r') {
            row[len - 1] = '\0';
        }
        char *t = row;
        while (*t == ' ' || *t == '\t') {
            ++t;
        }
        if (*t) {
            s_parse_lines[n].ms = 0;
            clip_into(s_parse_lines[n].text, sizeof(s_parse_lines[n].text), t);
            ++n;
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    return n;
}

int parse_server_json(const char *buf, bool *synced_out)
{
    *synced_out = false;
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return 0;
    }
    bool synced = cJSON_IsTrue(cJSON_GetObjectItem(root, "synced"));
    int n = 0;
    cJSON *lines = cJSON_GetObjectItem(root, "lines");
    if (cJSON_IsArray(lines)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, lines) {
            if (n >= kLineMax) {
                break;
            }
            cJSON *text = cJSON_GetObjectItem(it, "text");
            if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
                continue;
            }
            cJSON *ms = cJSON_GetObjectItem(it, "ms");
            s_parse_lines[n].ms =
                cJSON_IsNumber(ms) && ms->valuedouble > 0 ? static_cast<uint32_t>(ms->valuedouble) : 0;
            clip_into(s_parse_lines[n].text, sizeof(s_parse_lines[n].text), text->valuestring);
            ++n;
        }
    }
    cJSON_Delete(root);
    *synced_out = synced;
    return n;
}

int lines_from_lrclib_hit(cJSON *hit, bool *synced_out)
{
    *synced_out = false;
    if (!hit) {
        return 0;
    }
    cJSON *synced = cJSON_GetObjectItemCaseSensitive(hit, "syncedLyrics");
    if (cJSON_IsString(synced) && synced->valuestring && synced->valuestring[0]) {
        int n = parse_synced_lrc(synced->valuestring);
        if (n > 0) {
            *synced_out = true;
            return n;
        }
    }
    cJSON *plain = cJSON_GetObjectItemCaseSensitive(hit, "plainLyrics");
    if (cJSON_IsString(plain) && plain->valuestring && plain->valuestring[0]) {
        int n = parse_plain_text(plain->valuestring);
        if (n > 0) {
            *synced_out = false;
            return n;
        }
    }
    return 0;
}

bool http_get_public(const char *url, char *buf, int *status)
{
    buf[0] = '\0';
    *status = 0;
    return nodelio_http_get_ms(url, nullptr, nullptr, status, buf, kRespCap - 1, 10000) == ESP_OK &&
           *status == 200;
}

int fetch_lrclib(const char *title, const char *artist, char *buf, bool *synced_out)
{
    *synced_out = false;
    char tq[NL_ENV_TITLE * 3 + 4];
    char aq[NL_ENV_SUB * 3 + 4];
    url_encode(title, tq, sizeof(tq));
    url_encode(artist && artist[0] ? artist : "", aq, sizeof(aq));
    char url[kLyricUrlCap];
    if (artist && artist[0]) {
        snprintf(url, sizeof(url), "https://lrclib.net/api/get?track_name=%s&artist_name=%s", tq, aq);
    } else {
        snprintf(url, sizeof(url), "https://lrclib.net/api/get?track_name=%s", tq);
    }
    int status = 0;
    if (http_get_public(url, buf, &status)) {
        cJSON *one = cJSON_Parse(buf);
        int n = lines_from_lrclib_hit(one, synced_out);
        if (one) {
            cJSON_Delete(one);
        }
        if (n > 0) {
            ESP_LOGI(TAG, "lrclib get ok lines=%d", n);
            return n;
        }
    }

    if (artist && artist[0]) {
        snprintf(url, sizeof(url), "https://lrclib.net/api/search?track_name=%s&artist_name=%s", tq, aq);
    } else {
        snprintf(url, sizeof(url), "https://lrclib.net/api/search?track_name=%s", tq);
    }
    if (!http_get_public(url, buf, &status)) {
        return 0;
    }
    cJSON *arr = cJSON_Parse(buf);
    if (!cJSON_IsArray(arr)) {
        if (arr) {
            cJSON_Delete(arr);
        }
        return 0;
    }
    /* 优先带 syncedLyrics 的命中 */
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        cJSON *synced = cJSON_GetObjectItemCaseSensitive(it, "syncedLyrics");
        if (!cJSON_IsString(synced) || !synced->valuestring || !synced->valuestring[0]) {
            continue;
        }
        int n = lines_from_lrclib_hit(it, synced_out);
        if (n > 0) {
            cJSON_Delete(arr);
            ESP_LOGI(TAG, "lrclib search synced lines=%d", n);
            return n;
        }
    }
    cJSON_ArrayForEach(it, arr) {
        int n = lines_from_lrclib_hit(it, synced_out);
        if (n > 0) {
            cJSON_Delete(arr);
            ESP_LOGI(TAG, "lrclib search plain lines=%d", n);
            return n;
        }
    }
    cJSON_Delete(arr);
    return 0;
}

int fetch_lrcmux(const char *title, const char *artist, char *buf, bool *synced_out)
{
    *synced_out = false;
    char tq[NL_ENV_TITLE * 3 + 4];
    char aq[NL_ENV_SUB * 3 + 4];
    url_encode(title, tq, sizeof(tq));
    url_encode(artist && artist[0] ? artist : "", aq, sizeof(aq));
    char url[kLyricUrlCap];
    if (artist && artist[0]) {
        snprintf(url, sizeof(url), "https://api.lrcmux.dev/get?title=%s&artist=%s&format=json", tq, aq);
    } else {
        snprintf(url, sizeof(url), "https://api.lrcmux.dev/get?title=%s&format=json", tq);
    }
    int status = 0;
    if (!http_get_public(url, buf, &status)) {
        return 0;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        return 0;
    }
    cJSON *meta = cJSON_GetObjectItemCaseSensitive(root, "meta");
    if (cJSON_IsObject(meta) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(meta, "instrumental"))) {
        cJSON_Delete(root);
        return 0;
    }
    bool synced = false;
    if (cJSON_IsObject(meta)) {
        cJSON *level = cJSON_GetObjectItemCaseSensitive(meta, "level");
        if (cJSON_IsString(level) && level->valuestring) {
            synced = strcmp(level->valuestring, "line") == 0 || strcmp(level->valuestring, "word") == 0;
        }
    }
    int n = 0;
    cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (cJSON_IsArray(lines)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, lines) {
            if (n >= kLineMax) {
                break;
            }
            cJSON *text = cJSON_GetObjectItemCaseSensitive(it, "text");
            if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0]) {
                continue;
            }
            cJSON *start = cJSON_GetObjectItemCaseSensitive(it, "start");
            int ms = cJSON_IsNumber(start) ? static_cast<int>(start->valuedouble) : 0;
            if (ms < 0) {
                ms = 0;
            }
            s_parse_lines[n].ms = static_cast<uint32_t>(ms);
            clip_into(s_parse_lines[n].text, sizeof(s_parse_lines[n].text), text->valuestring);
            ++n;
        }
    }
    cJSON_Delete(root);
    if (n > 0) {
        *synced_out = synced;
        if (synced && n > 1) {
            sort_by_ms(n);
        }
        ESP_LOGI(TAG, "lrcmux ok synced=%d lines=%d", (int)synced, n);
    }
    return n;
}

void commit_lines(uint32_t gen, int n, bool synced, const char *src)
{
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (gen == s_gen) {
        if (n > 0) {
            memcpy(s_lines, s_parse_lines, sizeof(lyric_line_t) * static_cast<size_t>(n));
            s_line_n = n;
            s_synced = synced;
            s_shown[0] = '\0';
            ESP_LOGI(TAG, "ready src=%s synced=%d lines=%d", src ? src : "?", (int)synced, n);
        } else {
            s_key[0] = '\0';
        }
    }
    xSemaphoreGive(s_mu);
}

void lyric_worker(void *)
{
    lyric_job_t job{};
    for (;;) {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char *buf = alloc_buf();
        if (!buf) {
            continue;
        }

        int n = 0;
        bool synced = false;
        const char *src = nullptr;

        if (job.url[0]) {
            int status = 0;
            const esp_err_t err =
                nodelio_http_get_ms(job.url, job.device_key, nullptr, &status, buf, kRespCap, 8000);
            if (err == ESP_OK && status == 200) {
                n = parse_server_json(buf, &synced);
                if (n > 0) {
                    src = "server";
                }
            } else {
                ESP_LOGW(TAG, "server fail status=%d err=%d", status, (int)err);
            }
        }

        if (n == 0 && job.title[0]) {
            n = fetch_lrclib(job.title, job.artist, buf, &synced);
            if (n > 0) {
                src = "lrclib";
            }
        }
        if (n == 0 && job.title[0]) {
            n = fetch_lrcmux(job.title, job.artist, buf, &synced);
            if (n > 0) {
                src = "lrcmux";
            }
        }

        free(buf);
        commit_lines(job.gen, n, synced, src);
    }
}

void ensure_worker()
{
    if (s_q) {
        return;
    }
    ensure_mu();
    s_q = xQueueCreate(2, sizeof(lyric_job_t));
    if (!s_q) {
        return;
    }
    constexpr uint32_t kStack = 12288;
    auto *stack = static_cast<StackType_t *>(heap_caps_malloc(kStack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    static StaticTask_t tcb;
    if (stack) {
        xTaskCreateStatic(lyric_worker, "nl_lyric", kStack / sizeof(StackType_t), nullptr, 3, stack, &tcb);
        return;
    }
    xTaskCreate(lyric_worker, "nl_lyric", kStack, nullptr, 3, nullptr);
}

}  // namespace

void nodelio_lyrics_clear(void)
{
    ensure_mu();
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_gen++;
    s_line_n = 0;
    s_synced = false;
    s_key[0] = '\0';
    s_shown[0] = '\0';
    xSemaphoreGive(s_mu);
}

void nodelio_lyrics_request(const nodelio_nvs_config_t *cfg, const char *title, const char *artist,
                            bool force)
{
    if (!title || !title[0]) {
        nodelio_lyrics_clear();
        return;
    }
    const char *art = artist && artist[0] && strcmp(artist, "未命名") != 0 ? artist : "";
    char key[sizeof(s_key)];
    copy_key(title, art, key, sizeof(key));
    ensure_mu();
    if (!s_mu) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (!force && strcmp(s_key, key) == 0) {
        xSemaphoreGive(s_mu);
        return;
    }
    s_gen++;
    s_line_n = 0;
    s_synced = false;
    s_shown[0] = '\0';
    snprintf(s_key, sizeof(s_key), "%s", key);
    const uint32_t gen = s_gen;
    xSemaphoreGive(s_mu);

    lyric_job_t job{};
    job.gen = gen;
    snprintf(job.title, sizeof(job.title), "%s", title);
    snprintf(job.artist, sizeof(job.artist), "%s", art);

    if (cfg && cfg->server_url[0] && cfg->device_id[0]) {
        char title_q[NL_ENV_TITLE * 3 + 4];
        char artist_q[NL_ENV_SUB * 3 + 4];
        url_encode(title, title_q, sizeof(title_q));
        url_encode(art, artist_q, sizeof(artist_q));
        char path[800];
        const int path_n =
            snprintf(path, sizeof(path),
                     "/api/devices/%s/panel/lyrics?title=%s&artist=%s%s", cfg->device_id, title_q,
                     artist_q, force ? "&force=1" : "");
        if (path_n >= 0 && path_n < static_cast<int>(sizeof(path))) {
            char base[160];
            nl_url_strip_base(cfg->server_url, base, sizeof(base));
            if (cfg->device_key[0]) {
                snprintf(job.device_key, sizeof(job.device_key), "%s", cfg->device_key);
            }
            snprintf(job.url, sizeof(job.url), "%s%s", base, path);
        }
    }

    ensure_worker();
    if (!s_q) {
        return;
    }
    if (xQueueSend(s_q, &job, 0) != pdTRUE) {
        lyric_job_t drop{};
        xQueueReceive(s_q, &drop, 0);
        xQueueSend(s_q, &job, 0);
    }
}

bool nodelio_lyrics_apply(lv_obj_t *label)
{
    ensure_timer();
    if (!label) {
        return false;
    }
    ensure_mu();
    if (!s_mu) {
        return false;
    }
    char text[kTextMax];
    text[0] = '\0';
    bool synced = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int n = s_line_n;
    synced = s_synced;
    if (n > 0) {
        const uint32_t pos = Application::GetInstance().HttpAudioPositionMs();
        const int idx = pick_index(pos, n, synced);
        if (idx >= 0 && s_lines[idx].text[0]) {
            snprintf(text, sizeof(text), "%s", s_lines[idx].text);
        }
    }
    xSemaphoreGive(s_mu);
    if (!text[0]) {
        return false;
    }
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, synced ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_SCROLL_CIRCULAR);
    if (strcmp(s_shown, text) == 0) {
        return true;
    }
    snprintf(s_shown, sizeof(s_shown), "%s", text);
    lv_label_set_text(label, text);
    return true;
}

bool nodelio_lyrics_ready(void)
{
    ensure_mu();
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool ok = s_line_n > 0;
    xSemaphoreGive(s_mu);
    return ok;
}

int nodelio_lyrics_line_count(void)
{
    ensure_mu();
    if (!s_mu) {
        return 0;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int n = s_line_n;
    xSemaphoreGive(s_mu);
    return n;
}

int nodelio_lyrics_current_index(uint32_t position_ms)
{
    ensure_mu();
    if (!s_mu) {
        return -1;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const int n = s_line_n;
    const bool synced = s_synced;
    const int idx = pick_index(position_ms, n, synced);
    xSemaphoreGive(s_mu);
    return idx;
}

bool nodelio_lyrics_line_text(int idx, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    ensure_mu();
    if (!s_mu) {
        return false;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool ok = false;
    if (idx >= 0 && idx < s_line_n && s_lines[idx].text[0]) {
        snprintf(out, out_len, "%s", s_lines[idx].text);
        ok = true;
    }
    xSemaphoreGive(s_mu);
    return ok;
}
