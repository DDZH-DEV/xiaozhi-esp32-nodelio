#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_ENV_STR 64
#define NL_ENV_TITLE 96
#define NL_ENV_SUB 128
#define NL_ENV_URL 320
#define NL_ENV_SPOKEN 320
#define NL_ENV_TRACK_MAX 24
#define NL_ENV_NEWS_MAX 8
#define NL_ENV_NEWS_TITLE 128
#define NL_ENV_SUMMARY 384
#define NL_ENV_IMG 192
#define NL_ENV_ACTION_MAX 2
#define NL_ENV_FILTER_MAX 8
#define NL_ENV_FILTER_TITLE 24
#define NL_ENV_WS_PATH 160 /* filter.workspace；点选时再拼 params */
#define NL_ENV_PARAMS 384 /* DSH arm / answer(submit qid+oid)；勿截断 notice_id */

/** list_item 上 run:invoke 按钮（最多 2 个，如允许/拒绝；提问选项每行 1 个「选择」）. */
typedef struct {
    char id[24];
    char title[24];
    char app_id[NL_ENV_STR];
    char instance_id[NL_ENV_STR];
    char params_json[NL_ENV_PARAMS];
} nl_env_invoke_action_t;

/**
 * display.filters[] 顶栏 chip（瘦身：不存整份 invoke，点选用 workspace+envelope.app_id 拼）。
 * 整份 Envelope 须驻 PSRAM，禁止再塞内部 BSS。
 */
typedef struct {
    char workspace[NL_ENV_WS_PATH];
    char title[NL_ENV_FILTER_TITLE];
    bool selected;
} nl_env_filter_t;

typedef struct {
    char id[NL_ENV_STR];
    char title[NL_ENV_TITLE];
    char artist[NL_ENV_SUB]; /* list_item.subtitle 亦落入此字段 */
    char playback_url[NL_ENV_URL];
    char mime[32];
    int64_t expires_at;
    bool has_play_action;
    int invoke_n;
    nl_env_invoke_action_t invokes[NL_ENV_ACTION_MAX];
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
    char density[16]; /* compact | comfortable；缺省空=按 card_type */
    /** display.media_kind：track | live | episode；缺省按 app_id 回退 */
    char media_kind[16];
    char spoken[NL_ENV_SPOKEN];
    char spoken_audio_url[NL_ENV_URL]; /* spoken.audio.url，首段；兼容旧包 */
#define NL_ENV_SPOKEN_AUDIO_MAX 6
    int spoken_audio_n; /* spoken.audio.urls 段数；0 则仅用 spoken_audio_url */
    char spoken_audio_urls[NL_ENV_SPOKEN_AUDIO_MAX][NL_ENV_URL];
    char error_message[NL_ENV_SPOKEN];
    int ttl_sec;
    int track_n;
    nl_env_track_t tracks[NL_ENV_TRACK_MAX];
    int news_n;
    nl_env_news_t news[NL_ENV_NEWS_MAX];
    int filter_n;
    nl_env_filter_t filters[NL_ENV_FILTER_MAX];
    bool truncated;
    /** display.auto_play；缺省 true。浏览曲库时服务端置 false。 */
    bool auto_play;
    /**
     * list_item.body（会话详情全文）；PSRAM/堆上分配，解析时写入，调用方负责随 env 释放。
     * 仅详情卡使用；普通列表一般为 NULL。
     */
    char *detail_body;
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
