#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_ENV_STR 64
#define NL_ENV_URL 320
#define NL_ENV_SPOKEN 160
#define NL_ENV_TRACK_MAX 24
#define NL_ENV_NEWS_MAX 8
#define NL_ENV_NEWS_TITLE 128
#define NL_ENV_SUMMARY 384
#define NL_ENV_IMG 192

typedef struct {
    char id[NL_ENV_STR];
    char title[NL_ENV_STR];
    char artist[NL_ENV_STR];
    char playback_url[NL_ENV_URL];
    char mime[32];
    int64_t expires_at;
    bool has_play_action;
} nl_env_track_t;

typedef struct {
    char id[NL_ENV_STR];
    char title[NL_ENV_NEWS_TITLE];
    char summary[NL_ENV_SUMMARY];
    char source[NL_ENV_STR];
    char published_at[40];
    char image_url[NL_ENV_IMG];
} nl_env_news_t;

typedef struct {
    char status[16]; /* ok / error */
    char app_id[NL_ENV_STR];
    char card_type[32]; /* music_list / news_list / list / plain_result / error */
    char display_title[NL_ENV_STR];
    char preferred_surface[24];
    char spoken[NL_ENV_SPOKEN];
    char error_message[NL_ENV_STR];
    int ttl_sec;
    int track_n;
    nl_env_track_t tracks[NL_ENV_TRACK_MAX];
    int news_n;
    nl_env_news_t news[NL_ENV_NEWS_MAX];
    bool truncated;
    /** display.auto_play；缺省 true。浏览曲库时服务端置 false。 */
    bool auto_play;
} nl_content_envelope_t;

/**
 * 解析 Protocol Content Envelope（或降级旧 mock）。
 * 超限按 caps limits 截断；未知 card_type 降级。
 * 成功返回 true。
 */
bool nodelio_envelope_parse(const char *json, nl_content_envelope_t *out);

/** 给 LLM 的短摘要（优先 spoken，否则拼标题列表）。 */
void nodelio_envelope_spoken_summary(const nl_content_envelope_t *env, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
