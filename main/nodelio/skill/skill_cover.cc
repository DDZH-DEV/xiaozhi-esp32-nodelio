#include "skill_cover.h"

#include "application.h"
#include "content_envelope.h"
#include "jpg/jpeg_to_image.h"
#include "nodelio_http.h"
#include "panel_jobs.h"
#include "skill_media_cache.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "nl_cover";
constexpr int kJpegCap = 180 * 1024;
constexpr int kJsonCap = 24 * 1024;
constexpr int kCoverPx = 140; /* 与沉浸中心封面同级，旋转更轻 */

struct CoverState {
    char key[160]{};
    uint32_t gen = 0;
    lv_image_dsc_t dsc{};
    uint8_t *rgb = nullptr;
    nodelio_cover_done_cb_t cb = nullptr;
    void *user = nullptr;
    SemaphoreHandle_t mu = nullptr;
};

CoverState s_cv;

void ensure_mu()
{
    if (!s_cv.mu) {
        s_cv.mu = xSemaphoreCreateMutex();
    }
}

void url_encode(const char *in, char *out, size_t out_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p && o + 4 < out_len;
         ++p) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out[o++] = static_cast<char>(c);
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        }
    }
    out[o] = '\0';
}

void free_rgb_locked()
{
    if (s_cv.rgb) {
        heap_caps_free(s_cv.rgb);
        s_cv.rgb = nullptr;
    }
    memset(&s_cv.dsc, 0, sizeof(s_cv.dsc));
}

bool decode_jpeg_to_dsc(const uint8_t *jpeg, size_t jpeg_n, lv_image_dsc_t *out_dsc, uint8_t **out_rgb)
{
    if (!jpeg || jpeg_n == 0 || !out_dsc || !out_rgb) {
        return false;
    }
    uint8_t *rgb = nullptr;
    size_t rgb_len = 0, w = 0, h = 0, stride = 0;
    const esp_err_t err =
        jpeg_to_image_fit(jpeg, jpeg_n, kCoverPx, kCoverPx, kCoverPx * kCoverPx * 2 + 64, &rgb,
                          &rgb_len, &w, &h, &stride);
    if (err != ESP_OK || !rgb || w == 0 || h == 0) {
        if (rgb) {
            heap_caps_free(rgb);
        }
        return false;
    }
    memset(out_dsc, 0, sizeof(*out_dsc));
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    out_dsc->header.w = static_cast<uint32_t>(w);
    out_dsc->header.h = static_cast<uint32_t>(h);
    out_dsc->header.stride = static_cast<uint32_t>(stride ? stride : w * 2);
    out_dsc->data_size = rgb_len;
    out_dsc->data = rgb;
    *out_rgb = rgb;
    return true;
}

bool http_get_text(const char *url, char *buf, int cap, int *status)
{
    buf[0] = '\0';
    *status = 0;
    return nodelio_http_get_ms(url, nullptr, nullptr, status, buf, cap - 1, 9000) == ESP_OK &&
           *status == 200;
}

bool http_get_bin(const char *url, uint8_t *buf, int cap, size_t *out_n, int *status)
{
    *out_n = 0;
    *status = 0;
    return nodelio_http_get_bin_ms(url, nullptr, status, buf, cap, out_n, 12000) == ESP_OK &&
           *status == 200 && *out_n > 0;
}

bool itunes_artwork_url(const char *title, const char *artist, char *out, size_t out_len)
{
    char tq[NL_ENV_TITLE * 3 + 4];
    char aq[NL_ENV_SUB * 3 + 4];
    url_encode(title, tq, sizeof(tq));
    url_encode(artist && artist[0] ? artist : "", aq, sizeof(aq));
    char url[640];
    if (artist && artist[0]) {
        snprintf(url, sizeof(url),
                 "https://itunes.apple.com/search?term=%s+%s&media=music&entity=song&limit=1", tq,
                 aq);
    } else {
        snprintf(url, sizeof(url),
                 "https://itunes.apple.com/search?term=%s&media=music&entity=song&limit=1", tq);
    }
    char *buf = static_cast<char *>(heap_caps_malloc(kJsonCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        return false;
    }
    int status = 0;
    bool got = false;
    if (http_get_text(url, buf, kJsonCap, &status)) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "results") : nullptr;
        if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) > 0) {
            cJSON *one = cJSON_GetArrayItem(arr, 0);
            cJSON *art = cJSON_GetObjectItemCaseSensitive(one, "artworkUrl100");
            if (cJSON_IsString(art) && art->valuestring && art->valuestring[0]) {
                char tmp[320];
                snprintf(tmp, sizeof(tmp), "%s", art->valuestring);
                char *p = strstr(tmp, "100x100");
                if (p) {
                    memcpy(p, "140x140", 7);
                }
                snprintf(out, out_len, "%s", tmp);
                got = true;
            }
        }
        if (root) {
            cJSON_Delete(root);
        }
    }
    heap_caps_free(buf);
    return got;
}

bool musicbrainz_cover_url(const char *title, const char *artist, char *out, size_t out_len)
{
    /* title/artist 显式截断，躲 -Werror=format-truncation */
    constexpr int kQTitle = 80;
    constexpr int kQArtist = 80;
    char query[NL_ENV_TITLE + NL_ENV_SUB + 48];
    if (artist && artist[0]) {
        snprintf(query, sizeof(query), "recording:\"%.*s\" AND artist:\"%.*s\"", kQTitle,
                 title ? title : "", kQArtist, artist);
    } else {
        snprintf(query, sizeof(query), "recording:\"%.*s\"", kQTitle, title ? title : "");
    }
    char qenc[sizeof(query) * 3 + 4];
    url_encode(query, qenc, sizeof(qenc));
    char url[sizeof(qenc) + 96];
    snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/recording/?query=%s&fmt=json&limit=1",
             qenc);

    char *buf = static_cast<char *>(heap_caps_malloc(kJsonCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        return false;
    }
    int status = 0;
    char mbid[48]{};
    if (http_get_text(url, buf, kJsonCap, &status)) {
        cJSON *root = cJSON_Parse(buf);
        cJSON *recs = root ? cJSON_GetObjectItemCaseSensitive(root, "recordings") : nullptr;
        if (cJSON_IsArray(recs) && cJSON_GetArraySize(recs) > 0) {
            cJSON *rec = cJSON_GetArrayItem(recs, 0);
            cJSON *rels = cJSON_GetObjectItemCaseSensitive(rec, "releases");
            if (cJSON_IsArray(rels) && cJSON_GetArraySize(rels) > 0) {
                cJSON *rel = cJSON_GetArrayItem(rels, 0);
                cJSON *id = cJSON_GetObjectItemCaseSensitive(rel, "id");
                if (cJSON_IsString(id) && id->valuestring) {
                    snprintf(mbid, sizeof(mbid), "%s", id->valuestring);
                }
            }
        }
        if (root) {
            cJSON_Delete(root);
        }
    }
    heap_caps_free(buf);
    if (!mbid[0]) {
        return false;
    }
    snprintf(out, out_len, "https://coverartarchive.org/release/%s/front-250", mbid);
    return true;
}

struct cover_job_t {
    uint32_t gen;
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
};

void cover_discard(void *p) { free(p); }

void cover_run(void *p)
{
    auto *job = static_cast<cover_job_t *>(p);
    uint8_t *jpeg = static_cast<uint8_t *>(
        heap_caps_malloc(kJpegCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpeg) {
        free(job);
        return;
    }
    size_t jpeg_n = 0;
    bool from_cache = nodelio_media_cache_load_cover(job->title, job->artist, jpeg, kJpegCap, &jpeg_n);
    if (!from_cache) {
        char art_url[360]{};
        bool have = itunes_artwork_url(job->title, job->artist, art_url, sizeof(art_url));
        if (!have) {
            ESP_LOGI(kTag, "itunes miss, try CAA title=%s", job->title);
            have = musicbrainz_cover_url(job->title, job->artist, art_url, sizeof(art_url));
        }
        if (!have || !art_url[0]) {
            ESP_LOGW(kTag, "no cover title=%s", job->title);
            heap_caps_free(jpeg);
            free(job);
            return;
        }
        int status = 0;
        if (!http_get_bin(art_url, jpeg, kJpegCap, &jpeg_n, &status)) {
            ESP_LOGW(kTag, "cover download fail status=%d url=%s", status, art_url);
            heap_caps_free(jpeg);
            free(job);
            return;
        }
        nodelio_media_cache_save_cover(job->title, job->artist, jpeg, jpeg_n);
    }

    lv_image_dsc_t dsc{};
    uint8_t *rgb = nullptr;
    const bool decoded = decode_jpeg_to_dsc(jpeg, jpeg_n, &dsc, &rgb);
    heap_caps_free(jpeg);
    if (!decoded) {
        free(job);
        return;
    }

    ensure_mu();
    xSemaphoreTake(s_cv.mu, portMAX_DELAY);
    if (job->gen != s_cv.gen) {
        heap_caps_free(rgb);
        xSemaphoreGive(s_cv.mu);
        free(job);
        return;
    }
    free_rgb_locked();
    s_cv.rgb = rgb;
    s_cv.dsc = dsc;
    auto cb = s_cv.cb;
    void *user = s_cv.user;
    const uint32_t done_gen = job->gen;
    xSemaphoreGive(s_cv.mu);
    free(job);

    if (cb) {
        Application::GetInstance().Schedule([cb, user, done_gen]() {
            ensure_mu();
            if (!s_cv.mu) {
                return;
            }
            xSemaphoreTake(s_cv.mu, portMAX_DELAY);
            if (done_gen != s_cv.gen || !s_cv.rgb) {
                xSemaphoreGive(s_cv.mu);
                return;
            }
            lv_image_dsc_t copy = s_cv.dsc;
            xSemaphoreGive(s_cv.mu);
            cb(&copy, user);
        });
    }
}

}  // namespace

extern "C" void nodelio_cover_clear(void)
{
    ensure_mu();
    if (!s_cv.mu) {
        return;
    }
    xSemaphoreTake(s_cv.mu, portMAX_DELAY);
    s_cv.gen++;
    s_cv.key[0] = '\0';
    free_rgb_locked();
    xSemaphoreGive(s_cv.mu);
}

extern "C" void nodelio_cover_request(const char *title, const char *artist,
                                     nodelio_cover_done_cb_t cb, void *user)
{
    if (!title || !title[0] || !cb) {
        return;
    }
    const char *art = (artist && artist[0] && strcmp(artist, "未命名") != 0 &&
                       strcmp(artist, "内存卡") != 0)
                          ? artist
                          : "";
    char key[sizeof(s_cv.key)];
    snprintf(key, sizeof(key), "%s\n%s", title, art);
    ensure_mu();
    xSemaphoreTake(s_cv.mu, portMAX_DELAY);
    if (strcmp(s_cv.key, key) == 0 && s_cv.rgb) {
        lv_image_dsc_t copy = s_cv.dsc;
        s_cv.cb = cb;
        s_cv.user = user;
        xSemaphoreGive(s_cv.mu);
        cb(&copy, user);
        return;
    }
    s_cv.gen++;
    const uint32_t gen = s_cv.gen;
    snprintf(s_cv.key, sizeof(s_cv.key), "%s", key);
    s_cv.cb = cb;
    s_cv.user = user;
    free_rgb_locked();
    xSemaphoreGive(s_cv.mu);

    auto *job = static_cast<cover_job_t *>(calloc(1, sizeof(cover_job_t)));
    if (!job) {
        return;
    }
    job->gen = gen;
    snprintf(job->title, sizeof(job->title), "%s", title);
    snprintf(job->artist, sizeof(job->artist), "%s", art);
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Background, cover_run, job, cover_discard, 0,
                                 "cover.fetch", true)) {
        free(job);
    }
}
