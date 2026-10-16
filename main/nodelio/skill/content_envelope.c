#include "content_envelope.h"

#include "device_caps.h"

#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static void copy_str(char *dst, size_t dst_len, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    if (!obj || !dst_len) {
        return;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, dst_len, "%s", v->valuestring);
    }
}

static void degrade_card_type(nl_content_envelope_t *out)
{
    if (out->card_type[0] == '\0') {
        if (out->news_n > 0) {
            snprintf(out->card_type, sizeof(out->card_type), "news_list");
        } else if (out->track_n > 0) {
            snprintf(out->card_type, sizeof(out->card_type), "music_list");
        } else if (out->spoken[0]) {
            snprintf(out->card_type, sizeof(out->card_type), "plain_result");
        } else {
            snprintf(out->card_type, sizeof(out->card_type), "error");
        }
        return;
    }
    if (strcmp(out->card_type, "music_list") == 0) {
        if (!nodelio_caps_has("content.music_list")) {
            snprintf(out->card_type, sizeof(out->card_type),
                     nodelio_caps_has("content.list") ? "list" : "plain_result");
        }
        return;
    }
    if (strcmp(out->card_type, "news_list") == 0) {
        if (!nodelio_caps_has("content.news_list")) {
            snprintf(out->card_type, sizeof(out->card_type),
                     nodelio_caps_has("content.list") ? "list" : "plain_result");
        }
        return;
    }
    if (strcmp(out->card_type, "list") == 0 && !nodelio_caps_has("content.list")) {
        snprintf(out->card_type, sizeof(out->card_type), "plain_result");
    }
}

static void parse_track_obj(const cJSON *item, nl_env_track_t *t, bool allow_play)
{
    memset(t, 0, sizeof(*t));
    copy_str(t->id, sizeof(t->id), item, "id");
    copy_str(t->title, sizeof(t->title), item, "title");
    copy_str(t->artist, sizeof(t->artist), item, "artist");
    if (t->title[0] == '\0') {
        copy_str(t->title, sizeof(t->title), item, "name");
    }
    const cJSON *playback = cJSON_GetObjectItemCaseSensitive(item, "playback");
    if (cJSON_IsObject(playback)) {
        copy_str(t->playback_url, sizeof(t->playback_url), playback, "url");
        copy_str(t->mime, sizeof(t->mime), playback, "mime");
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(playback, "expires_at");
        if (cJSON_IsNumber(exp)) {
            t->expires_at = (int64_t)exp->valuedouble;
        }
    } else {
        /* 旧 mock: url 在条目根上 */
        copy_str(t->playback_url, sizeof(t->playback_url), item, "url");
    }
    const cJSON *actions = cJSON_GetObjectItemCaseSensitive(item, "actions");
    if (cJSON_IsArray(actions) && allow_play) {
        const cJSON *a;
        cJSON_ArrayForEach(a, actions)
        {
            const cJSON *local = cJSON_GetObjectItemCaseSensitive(a, "local");
            if (cJSON_IsString(local) && local->valuestring &&
                strcmp(local->valuestring, "media.play") == 0) {
                t->has_play_action = true;
                break;
            }
        }
    }
    if (allow_play && t->playback_url[0] && !t->has_play_action) {
        t->has_play_action = true; /* 有 URL 即可点播 */
    }
    if (!allow_play) {
        t->has_play_action = false;
    }
}

static void parse_news_obj(const cJSON *item, nl_env_news_t *n)
{
    memset(n, 0, sizeof(*n));
    copy_str(n->id, sizeof(n->id), item, "id");
    copy_str(n->title, sizeof(n->title), item, "title");
    copy_str(n->summary, sizeof(n->summary), item, "summary");
    copy_str(n->source, sizeof(n->source), item, "source");
    copy_str(n->published_at, sizeof(n->published_at), item, "published_at");
    copy_str(n->image_url, sizeof(n->image_url), item, "image_url");
}

static bool parse_legacy_mock(cJSON *root, nl_content_envelope_t *out)
{
    /* { status, action:music_play, result, tracks:[{title,url,artist}] } */
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsArray(tracks) && !cJSON_IsString(result)) {
        return false;
    }
    snprintf(out->status, sizeof(out->status), "ok");
    snprintf(out->app_id, sizeof(out->app_id), "music_search");
    snprintf(out->card_type, sizeof(out->card_type), "music_list");
    if (cJSON_IsString(result) && result->valuestring) {
        snprintf(out->spoken, sizeof(out->spoken), "%s", result->valuestring);
        snprintf(out->display_title, sizeof(out->display_title), "搜索结果");
    }
    const bool allow_play = nodelio_caps_has("media.play_local");
    const int max_items = nodelio_caps_max_list_items();
    if (cJSON_IsArray(tracks)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, tracks)
        {
            if (out->track_n >= NL_ENV_TRACK_MAX || out->track_n >= max_items) {
                out->truncated = true;
                break;
            }
            parse_track_obj(item, &out->tracks[out->track_n], allow_play);
            if (out->tracks[out->track_n].title[0] == '\0') {
                continue;
            }
            out->track_n++;
        }
    }
    degrade_card_type(out);
    return true;
}

bool nodelio_envelope_parse(const char *json, nl_content_envelope_t *out)
{
    if (!json || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->auto_play = true;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(out->status, sizeof(out->status), "error");
        snprintf(out->card_type, sizeof(out->card_type), "error");
        snprintf(out->error_message, sizeof(out->error_message), "无效响应");
        return false;
    }

    /* Envelope 标志：有 schema_version 或 display / content */
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    const cJSON *display = cJSON_GetObjectItemCaseSensitive(root, "display");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
    const bool is_envelope = cJSON_IsNumber(schema) || cJSON_IsObject(display) || cJSON_IsArray(content);

    if (!is_envelope) {
        bool ok = parse_legacy_mock(root, out);
        cJSON_Delete(root);
        return ok;
    }

    copy_str(out->status, sizeof(out->status), root, "status");
    if (out->status[0] == '\0') {
        snprintf(out->status, sizeof(out->status), "ok");
    }
    copy_str(out->app_id, sizeof(out->app_id), root, "app_id");
    if (out->app_id[0] == '\0') {
        copy_str(out->app_id, sizeof(out->app_id), root, "skill_id");
    }

    if (cJSON_IsObject(display)) {
        copy_str(out->card_type, sizeof(out->card_type), display, "card_type");
        copy_str(out->display_title, sizeof(out->display_title), display, "title");
        copy_str(out->preferred_surface, sizeof(out->preferred_surface), display, "preferred_surface");
        const cJSON *ttl = cJSON_GetObjectItemCaseSensitive(display, "ttl_sec");
        if (cJSON_IsNumber(ttl)) {
            out->ttl_sec = ttl->valueint;
        }
        const cJSON *ap = cJSON_GetObjectItemCaseSensitive(display, "auto_play");
        if (cJSON_IsBool(ap) && !cJSON_IsTrue(ap)) {
            out->auto_play = false;
        }
        const cJSON *max_items = cJSON_GetObjectItemCaseSensitive(display, "max_items");
        (void)max_items;
    }

    const cJSON *spoken = cJSON_GetObjectItemCaseSensitive(root, "spoken");
    if (cJSON_IsObject(spoken)) {
        copy_str(out->spoken, sizeof(out->spoken), spoken, "text");
    } else if (cJSON_IsString(spoken) && spoken->valuestring) {
        snprintf(out->spoken, sizeof(out->spoken), "%s", spoken->valuestring);
    }

    const bool allow_play = nodelio_caps_has("media.play_local");
    int limit = nodelio_caps_max_list_items();
    if (cJSON_IsObject(display)) {
        const cJSON *mi = cJSON_GetObjectItemCaseSensitive(display, "max_items");
        if (cJSON_IsNumber(mi) && mi->valueint > 0 && mi->valueint < limit) {
            limit = mi->valueint;
        }
    }

    if (cJSON_IsArray(content)) {
        const cJSON *item;
        cJSON_ArrayForEach(item, content)
        {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
            const char *ts = (cJSON_IsString(type) && type->valuestring) ? type->valuestring : "";
            if (strcmp(ts, "error") == 0) {
                copy_str(out->error_message, sizeof(out->error_message), item, "message");
                if (out->error_message[0] == '\0') {
                    copy_str(out->error_message, sizeof(out->error_message), item, "title");
                }
                continue;
            }
            if (strcmp(ts, "news") == 0) {
                if (out->news_n >= NL_ENV_NEWS_MAX || out->news_n >= limit) {
                    out->truncated = true;
                    continue;
                }
                parse_news_obj(item, &out->news[out->news_n]);
                if (out->news[out->news_n].title[0] == '\0') {
                    continue;
                }
                out->news_n++;
                continue;
            }
            if (strcmp(ts, "music") != 0 && strcmp(ts, "list_item") != 0 && ts[0] != '\0') {
                /* 其它类型：尽量当列表项 */
                if (strcmp(ts, "text") == 0) {
                    if (out->spoken[0] == '\0') {
                        copy_str(out->spoken, sizeof(out->spoken), item, "text");
                    }
                    continue;
                }
            }
            if (out->track_n >= NL_ENV_TRACK_MAX || out->track_n >= limit) {
                out->truncated = true;
                continue;
            }
            parse_track_obj(item, &out->tracks[out->track_n], allow_play);
            if (out->tracks[out->track_n].title[0] == '\0') {
                continue;
            }
            out->track_n++;
        }
    }

    if (strcmp(out->status, "error") == 0 || strcmp(out->status, "ok") != 0) {
        if (out->error_message[0] == '\0') {
            copy_str(out->error_message, sizeof(out->error_message), root, "error");
            if (out->error_message[0] == '\0') {
                snprintf(out->error_message, sizeof(out->error_message), "技能失败");
            }
        }
        if (out->card_type[0] == '\0') {
            snprintf(out->card_type, sizeof(out->card_type), "error");
        }
    }

    degrade_card_type(out);
    cJSON_Delete(root);
    return true;
}

void nodelio_envelope_spoken_summary(const nl_content_envelope_t *env, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    if (!env) {
        return;
    }
    if (env->spoken[0]) {
        snprintf(buf, buf_len, "%s", env->spoken);
        return;
    }
    if (strcmp(env->status, "ok") != 0) {
        snprintf(buf, buf_len, "%s", env->error_message[0] ? env->error_message : "技能失败");
        return;
    }
    if (env->news_n > 0 && env->track_n <= 0) {
        snprintf(buf, buf_len, "%s", env->news[0].title);
        return;
    }
    if (env->track_n <= 0) {
        snprintf(buf, buf_len, "未找到结果");
        return;
    }
    if (env->track_n == 1) {
        snprintf(buf, buf_len, "找到《%s》%s%s", env->tracks[0].title,
                 env->tracks[0].artist[0] ? " - " : "", env->tracks[0].artist);
        return;
    }
    /* 多列几首给 LLM/语音，避免只说「有N首」却报不出歌名 */
    char names[NL_ENV_SPOKEN];
    size_t off = 0;
    names[0] = '\0';
    int n_show = env->track_n < 5 ? env->track_n : 5;
    for (int i = 0; i < n_show; i++) {
        int w = snprintf(names + off, sizeof(names) - off, "%s《%s》", i ? "、" : "",
                         env->tracks[i].title);
        if (w <= 0 || (size_t)w >= sizeof(names) - off) {
            break;
        }
        off += (size_t)w;
    }
    if (env->track_n > n_show) {
        snprintf(buf, buf_len, "共%d首，例如%s等，可点播或说歌名", env->track_n, names);
    } else {
        snprintf(buf, buf_len, "共%d首：%s，可点播或说歌名", env->track_n, names);
    }
}
