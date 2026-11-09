#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_ENV_STR 64
#define NL_ENV_TITLE 96
#define NL_ENV_SUB 220
#define NL_ENV_URL 320
#define NL_ENV_SPOKEN 320
#define NL_ENV_TRACK_MAX 24
#define NL_ENV_NEWS_MAX 8
#define NL_ENV_DAILY_MAX 15

typedef struct {
    char date[11];
    char condition[40];
    char high[16];
    char low[16];
    char rain[16];
} nl_env_daily_day_t;

typedef struct {
    char city[64];
    char message[128];
    char updated[32];
    char temperature[16];
    char summary[128];
    char air[160];
    char air_updated[32];
    int day_n;
    nl_env_daily_day_t days[NL_ENV_DAILY_MAX];
} nl_env_daily_t;
#define NL_ENV_NEWS_TITLE 128
#define NL_ENV_SUMMARY 384
#define NL_ENV_IMG 192
#define NL_ENV_ACTION_MAX 2
#define NL_ENV_FILTER_MAX 8
#define NL_ENV_FILTER_TITLE 24
#define NL_ENV_WS_PATH 160 /* filter.workspace；点选时再拼 params */
#define NL_ENV_PARAMS 384 /* DSH arm / answer(submit qid+oid)；勿截断 notice_id */
#define NL_ENV_CHAT_MAX 16
#define NL_ENV_CHAT_TEXT 256
#define NL_ENV_CHIP_MAX 8

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
    bool unread; /* support_inbox：有未读访客消息 */
    bool online; /* support_inbox：访客 WS 仍在本机会话房间 */
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

/** 客服气泡：访客左、客服/机器人右；正文不进 96 字节标题。 */
typedef struct {
    char id[NL_ENV_STR];
    char role[16]; /* visitor | agent | bot | system */
    char name[32];
    char text[NL_ENV_CHAT_TEXT];
    char image_url[NL_ENV_URL]; /* 非空则气泡内显示 JPEG；普惠字库没有的表情走 emoji 回退字 */
    char sent_at[20];           /* 本机时区的短时间，如 16:32 */
} nl_env_chat_msg_t;

typedef struct {
    char status[16]; /* ok / error */
    char app_id[NL_ENV_STR];
    char card_type[32]; /* music_list / news_list / list / support_inbox / support_thread / … */
    char display_title[NL_ENV_STR];
    char bridge_id[64];
    char bridge_name[128];
    char workbench_provider[16];
    bool workbench_picker;
    char workbench_picker_kind[16];
    int workbench_page;
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
    nl_env_daily_t daily;
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
    /** support_thread：气泡列表，PSRAM/堆上分配，随 env 释放。 */
    int chat_n;
    nl_env_chat_msg_t *chats;
    /** support_thread 底部操作（接管 / 发送 / 返回）。 */
    int chip_n;
    nl_env_invoke_action_t chips[NL_ENV_CHIP_MAX];
    /** display.conversation_id；语音 draft 用。 */
    uint64_t conversation_id;
    /** display.message_id；小智拟回复写发送按钮时用。 */
    uint64_t message_id;
    /** display.cloud_suggest：go-chat 无建议时由小智拟 2–4 条。 */
    bool cloud_suggest;
    /** display.support_meta：会话顶栏，时间 / IP / 位置 / 客户端，可含换行。 */
    char support_meta[192];
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
