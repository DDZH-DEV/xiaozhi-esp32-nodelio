#include "skill_favorites.h"

#include "application.h"
#include "content_envelope.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "nvs_config.h"
#include "overlay_priv.h"
#include "panel_jobs.h"
#include "panel_ui.h"
#include "skill_immersive_player.h"
#include "skill_result_ui.h"
#include "skill_ui_priv.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "nl_fav";

constexpr int kFavCacheMax = 48;

struct FavCache {
    char urls[kFavCacheMax][NL_ENV_URL];
    int n = 0;
};

FavCache s_cache;
bool s_inited = false;

const char *guess_source(const char *url)
{
    if (!url || !url[0]) {
        return "external";
    }
    if (strncmp(url, "sd://", 5) == 0) {
        return "sd";
    }
    if (strstr(url, "/panel/media/") != nullptr || strstr(url, "/api/devices/") != nullptr) {
        return "cloud";
    }
    return "external";
}

bool cache_has(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    for (int i = 0; i < s_cache.n; i++) {
        if (strcmp(s_cache.urls[i], url) == 0) {
            return true;
        }
    }
    return false;
}

void cache_add(const char *url)
{
    if (!url || !url[0] || cache_has(url)) {
        return;
    }
    if (s_cache.n >= kFavCacheMax) {
        /* 挤掉最旧 */
        memmove(s_cache.urls[0], s_cache.urls[1], sizeof(s_cache.urls[0]) * (kFavCacheMax - 1));
        s_cache.n = kFavCacheMax - 1;
    }
    snprintf(s_cache.urls[s_cache.n], sizeof(s_cache.urls[0]), "%s", url);
    s_cache.n++;
}

void cache_del(const char *url)
{
    if (!url || !url[0]) {
        return;
    }
    for (int i = 0; i < s_cache.n; i++) {
        if (strcmp(s_cache.urls[i], url) != 0) {
            continue;
        }
        if (i + 1 < s_cache.n) {
            memmove(s_cache.urls[i], s_cache.urls[i + 1],
                    sizeof(s_cache.urls[0]) * static_cast<size_t>(s_cache.n - i - 1));
        }
        s_cache.n--;
        return;
    }
}

void cache_clear()
{
    s_cache.n = 0;
}

void sync_immersive_ui()
{
    nodelio_skill_immersive_set_favorited(nodelio_media_is_current_favorited());
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
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xf];
        }
    }
    out[o] = '\0';
}

bool load_cfg(nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    if (s_skill_cfg.server_url[0] && s_skill_cfg.device_id[0]) {
        *cfg = s_skill_cfg;
    } else {
        nl_ov_cfg_for_http(cfg);
    }
    return cfg->server_url[0] && cfg->device_id[0] && cfg->device_key[0];
}

struct list_job_t {
    nodelio_nvs_config_t cfg;
    uint32_t view_epoch;
};

void list_discard(void *p) { free(p); }

void list_run(void *p)
{
    auto *job = static_cast<list_job_t *>(p);
    char base[160];
    nl_url_strip_base(job->cfg.server_url, base, sizeof(base));
    char url[320];
    snprintf(url, sizeof(url), "%s/api/devices/%s/panel/media/favorites", base, job->cfg.device_id);
    const int cap = 24 * 1024;
    char *buf = static_cast<char *>(
        heap_caps_malloc(static_cast<size_t>(cap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<char *>(malloc(static_cast<size_t>(cap)));
    }
    if (!buf) {
        nodelio_panel_ui_toast_defer("内存不足");
        free(job);
        return;
    }
    buf[0] = '\0';
    int status = 0;
    (void)nodelio_http_get_ms(url, job->cfg.device_key, nullptr, &status, buf, cap - 1, 8000);
    if (status != 200) {
        ESP_LOGW(kTag, "list favorites status=%d", status);
        nodelio_panel_ui_toast_defer("收藏加载失败");
        free(buf);
        free(job);
        return;
    }

    nl_content_envelope_t *env = static_cast<nl_content_envelope_t *>(
        heap_caps_calloc(1, sizeof(nl_content_envelope_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!env) {
        env = static_cast<nl_content_envelope_t *>(calloc(1, sizeof(nl_content_envelope_t)));
    }
    if (!env) {
        nodelio_panel_ui_toast_defer("内存不足");
        free(buf);
        free(job);
        return;
    }
    snprintf(env->status, sizeof(env->status), "ok");
    snprintf(env->app_id, sizeof(env->app_id), "music.favorites");
    snprintf(env->card_type, sizeof(env->card_type), "music_list");
    snprintf(env->display_title, sizeof(env->display_title), "收藏");
    snprintf(env->media_kind, sizeof(env->media_kind), "track");

    cache_clear();
    cJSON *root = cJSON_Parse(buf);
    const cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, "items") : nullptr;
    int n = 0;
    if (cJSON_IsArray(arr)) {
        const cJSON *it = nullptr;
        cJSON_ArrayForEach(it, arr)
        {
            if (n >= NL_ENV_TRACK_MAX) {
                break;
            }
            const cJSON *title = cJSON_GetObjectItemCaseSensitive(it, "title");
            const cJSON *artist = cJSON_GetObjectItemCaseSensitive(it, "artist");
            const cJSON *purl = cJSON_GetObjectItemCaseSensitive(it, "playbackUrl");
            if (!purl) {
                purl = cJSON_GetObjectItemCaseSensitive(it, "playback_url");
            }
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (!cJSON_IsString(purl) || !purl->valuestring || !purl->valuestring[0]) {
                continue;
            }
            nl_env_track_t *tr = &env->tracks[n];
            if (cJSON_IsNumber(id)) {
                snprintf(tr->id, sizeof(tr->id), "fav%lld", (long long)id->valuedouble);
            } else {
                snprintf(tr->id, sizeof(tr->id), "fav%d", n);
            }
            snprintf(tr->title, sizeof(tr->title), "%s",
                     cJSON_IsString(title) && title->valuestring ? title->valuestring : "未命名");
            snprintf(tr->artist, sizeof(tr->artist), "%s",
                     cJSON_IsString(artist) && artist->valuestring ? artist->valuestring : "收藏");
            snprintf(tr->playback_url, sizeof(tr->playback_url), "%s", purl->valuestring);
            tr->has_play_action = true;
            cache_add(tr->playback_url);
            n++;
        }
    }
    if (root) {
        cJSON_Delete(root);
    }
    free(buf);
    env->track_n = n;
    const uint32_t epoch = job->view_epoch;
    const nodelio_nvs_config_t cfg = job->cfg;
    free(job);

    if (!nodelio_panel_skill_view_current(epoch)) {
        free(env);
        return;
    }
    if (n <= 0) {
        free(env);
        nodelio_panel_ui_toast_defer("暂无收藏");
        return;
    }
    Application::GetInstance().Schedule([env, cfg]() {
        nodelio_skill_result_show(&cfg, env, false);
        sync_immersive_ui();
    });
}

struct toggle_job_t {
    nodelio_nvs_config_t cfg;
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    char url[NL_ENV_URL];
    char source[16];
    bool remove;
};

void toggle_discard(void *p) { free(p); }

void toggle_run(void *p)
{
    auto *job = static_cast<toggle_job_t *>(p);
    char base[160];
    nl_url_strip_base(job->cfg.server_url, base, sizeof(base));
    int status = 0;
    char resp[256];
    resp[0] = '\0';
    if (job->remove) {
        char enc[NL_ENV_URL * 3 + 4];
        url_encode(job->url, enc, sizeof(enc));
        char url[640];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/media/favorites?url=%s", base,
                 job->cfg.device_id, enc);
        (void)nodelio_http_delete(url, job->cfg.device_key, &status, resp, sizeof(resp) - 1);
        if (status == 200 || status == 204) {
            cache_del(job->url);
            nodelio_panel_ui_toast_defer("已取消收藏");
        } else {
            ESP_LOGW(kTag, "unfav status=%d", status);
            nodelio_panel_ui_toast_defer("取消收藏失败");
        }
    } else {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "title", job->title);
        cJSON_AddStringToObject(body, "artist", job->artist);
        cJSON_AddStringToObject(body, "playbackUrl", job->url);
        cJSON_AddStringToObject(body, "source", job->source);
        char *json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (!json) {
            nodelio_panel_ui_toast_defer("内存不足");
            free(job);
            return;
        }
        char url[320];
        snprintf(url, sizeof(url), "%s/api/devices/%s/panel/media/favorites", base,
                 job->cfg.device_id);
        (void)nodelio_http_post_json_ms(url, job->cfg.device_key, json, &status, resp,
                                        sizeof(resp) - 1, 8000);
        cJSON_free(json);
        if (status == 200 || status == 201) {
            cache_add(job->url);
            nodelio_panel_ui_toast_defer("已收藏");
        } else {
            ESP_LOGW(kTag, "fav status=%d body=%s", status, resp);
            nodelio_panel_ui_toast_defer("收藏失败");
        }
    }
    free(job);
    Application::GetInstance().Schedule([]() { sync_immersive_ui(); });
}

void on_fav_btn() { nodelio_media_favorite_toggle_current(); }

}  // namespace

extern "C" void nodelio_favorites_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    nodelio_skill_immersive_set_fav_cb(on_fav_btn);
}

extern "C" bool nodelio_media_is_current_favorited(void)
{
    char url[NL_ENV_URL];
    nodelio_media_now_url(url, sizeof(url));
    return cache_has(url);
}

extern "C" void nodelio_favorites_sync_immersive(void) { sync_immersive_ui(); }

extern "C" void nodelio_skill_request_favorites(void)
{
    nodelio_favorites_init();
    nodelio_nvs_config_t cfg{};
    if (!load_cfg(&cfg)) {
        nl_ov_toast("未绑定服务");
        return;
    }
    auto *job = static_cast<list_job_t *>(calloc(1, sizeof(list_job_t)));
    if (!job) {
        nl_ov_toast("内存不足");
        return;
    }
    job->cfg = cfg;
    job->view_epoch = nodelio_panel_begin_skill_view();
    nl_ov_toast("加载收藏…");
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Interactive, list_run, job, list_discard,
                                 job->view_epoch, "fav.list")) {
        free(job);
        nl_ov_toast("任务繁忙");
    }
}

extern "C" void nodelio_media_favorite_toggle_current(void)
{
    nodelio_favorites_init();
    char url[NL_ENV_URL];
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB];
    nodelio_media_now_url(url, sizeof(url));
    nodelio_media_now_title(title, sizeof(title));
    nodelio_media_now_artist(artist, sizeof(artist));
    if (!url[0]) {
        nl_ov_toast("当前没有可收藏曲目");
        return;
    }
    nodelio_nvs_config_t cfg{};
    if (!load_cfg(&cfg)) {
        nl_ov_toast("未绑定服务");
        return;
    }
    auto *job = static_cast<toggle_job_t *>(calloc(1, sizeof(toggle_job_t)));
    if (!job) {
        nl_ov_toast("内存不足");
        return;
    }
    job->cfg = cfg;
    snprintf(job->url, sizeof(job->url), "%s", url);
    snprintf(job->title, sizeof(job->title), "%s", title[0] ? title : "未命名");
    snprintf(job->artist, sizeof(job->artist), "%s", artist);
    snprintf(job->source, sizeof(job->source), "%s", guess_source(url));
    job->remove = cache_has(url);
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Background, toggle_run, job, toggle_discard,
                                 0, "fav.toggle", true)) {
        free(job);
        nl_ov_toast("任务繁忙");
    }
}
