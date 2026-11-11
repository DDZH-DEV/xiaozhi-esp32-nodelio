#include "skill_lyrics.h"

#include "application.h"
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
#include <cstring>

#define TAG "nl_lyric"

extern lv_obj_t *s_now_sub;

namespace {

constexpr int kLineMax = 100;
constexpr int kTextMax = 84;
constexpr int kRespCap = 16 * 1024;

struct lyric_line_t {
    uint32_t ms;
    char text[kTextMax];
};

struct lyric_job_t {
    uint32_t gen;
    char url[1024];
    char device_key[65];
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

int pick_index(uint32_t pos, int n, bool synced)
{
    if (n <= 0) {
        return -1;
    }
    if (!synced) {
        const uint32_t step = static_cast<uint32_t>(esp_timer_get_time() / 4000000LL);
        return static_cast<int>(step % static_cast<uint32_t>(n));
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

void lyric_worker(void *)
{
    lyric_job_t job{};
    for (;;) {
        if (xQueueReceive(s_q, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        char *buf = static_cast<char *>(heap_caps_malloc(kRespCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buf) {
            buf = static_cast<char *>(malloc(kRespCap));
        }
        if (!buf) {
            continue;
        }
        int status = 0;
        const esp_err_t err = nodelio_http_get_ms(job.url, job.device_key, nullptr, &status, buf, kRespCap, 8000);
        if (err != ESP_OK || status != 200) {
            ESP_LOGW(TAG, "get fail status=%d err=%d", status, (int)err);
            free(buf);
            if (s_mu) {
                xSemaphoreTake(s_mu, portMAX_DELAY);
                if (job.gen == s_gen) {
                    s_key[0] = '\0';
                }
                xSemaphoreGive(s_mu);
            }
            continue;
        }
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) {
            continue;
        }
        int n = 0;
        bool synced = cJSON_IsTrue(cJSON_GetObjectItem(root, "synced"));
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
                s_parse_lines[n].ms = cJSON_IsNumber(ms) && ms->valuedouble > 0 ? static_cast<uint32_t>(ms->valuedouble) : 0;
                snprintf(s_parse_lines[n].text, sizeof(s_parse_lines[n].text), "%s", text->valuestring);
                n++;
            }
        }
        cJSON_Delete(root);
        if (!s_mu) {
            continue;
        }
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (job.gen == s_gen) {
            if (n > 0) {
                memcpy(s_lines, s_parse_lines, sizeof(lyric_line_t) * static_cast<size_t>(n));
                s_line_n = n;
                s_synced = synced;
                s_shown[0] = '\0';
                ESP_LOGI(TAG, "ready synced=%d lines=%d", (int)synced, n);
            } else {
                s_key[0] = '\0';
            }
        }
        xSemaphoreGive(s_mu);
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
    constexpr uint32_t kStack = 8192;
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

void nodelio_lyrics_request(const nodelio_nvs_config_t *cfg, const char *title, const char *artist)
{
    if (!cfg || !title || !title[0] || !cfg->server_url[0] || !cfg->device_id[0]) {
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
    if (strcmp(s_key, key) == 0) {
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

    char title_q[NL_ENV_TITLE * 3 + 4];
    char artist_q[NL_ENV_SUB * 3 + 4];
    url_encode(title, title_q, sizeof(title_q));
    url_encode(art, artist_q, sizeof(artist_q));
    char path[800];
    const int path_n = snprintf(path, sizeof(path), "/api/devices/%s/panel/lyrics?title=%s&artist=%s", cfg->device_id,
                                title_q, artist_q);
    if (path_n < 0 || path_n >= static_cast<int>(sizeof(path))) {
        nodelio_lyrics_clear();
        return;
    }
    char base[160];
    nl_url_strip_base(cfg->server_url, base, sizeof(base));
    lyric_job_t job{};
    job.gen = gen;
    if (cfg->device_key[0]) {
        snprintf(job.device_key, sizeof(job.device_key), "%s", cfg->device_key);
    }
    const int url_n = snprintf(job.url, sizeof(job.url), "%s%s", base, path);
    if (url_n < 0 || url_n >= static_cast<int>(sizeof(job.url))) {
        nodelio_lyrics_clear();
        return;
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
        uint32_t pos = 0;
        if (synced) {
            pos = Application::GetInstance().HttpAudioPositionMs();
        }
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
