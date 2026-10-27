#include "skill_runtime.h"

#include "content_envelope.h"
#include "device_caps.h"
#include "panel_ui.h"
#include "skill_result_ui.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nl_skill";

static char s_last_spoken_audio[NL_ENV_URL];
static int s_last_spoken_audio_n;
static char s_last_spoken_audios[NL_ENV_SPOKEN_AUDIO_MAX][NL_ENV_URL];

bool nodelio_skill_id_is_music(const char *skill_id)
{
    if (!skill_id || !skill_id[0]) {
        return false;
    }
    /* 音频列表类：曲库 / 电台 / 播客（互切不抢占正在播的 HTTP 音频） */
    return strstr(skill_id, "music.search") != NULL || strstr(skill_id, "music_search") != NULL ||
           strstr(skill_id, "radio.stations") != NULL || strstr(skill_id, "podcast.latest") != NULL;
}

void nodelio_skill_deliver_json(const nodelio_nvs_config_t *cfg, const char *json, bool auto_play_first,
                                char *spoken_out, size_t spoken_len)
{
    if (spoken_out && spoken_len) {
        spoken_out[0] = '\0';
    }
    if (!json || !json[0]) {
        if (spoken_out && spoken_len) {
            snprintf(spoken_out, spoken_len, "技能无响应");
        }
        return;
    }

    /* ~14KB：禁止放 mcp_http / nl_skill_test 任务栈 */
    nl_content_envelope_t *env =
        (nl_content_envelope_t *)heap_caps_malloc(sizeof(*env), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!env) {
        env = (nl_content_envelope_t *)malloc(sizeof(*env));
    }
    if (!env) {
        if (spoken_out && spoken_len) {
            snprintf(spoken_out, spoken_len, "内存不足");
        }
        nodelio_panel_ui_toast_defer("内存不足");
        return;
    }

    if (!nodelio_envelope_parse(json, env)) {
        if (spoken_out && spoken_len) {
            snprintf(spoken_out, spoken_len, "%s",
                     env->error_message[0] ? env->error_message : "解析失败");
        }
        nodelio_panel_ui_toast_defer(env->error_message[0] ? env->error_message : "技能解析失败");
        free(env);
        return;
    }

    /* E5：error Envelope 也上屏，并 toast，禁止当成功静默 */
    if (strcmp(env->status, "ok") != 0) {
        ESP_LOGW(TAG, "deliver error app=%s msg=%s",
                 env->app_id[0] ? env->app_id : "-",
                 env->error_message[0] ? env->error_message : "(none)");
        nodelio_panel_ui_toast_defer(env->error_message[0] ? env->error_message : "技能失败");
    }

    /* env.auto_play：服务端点播/搜歌；auto_play_first：打开曲库强制播（须识别 market::music.search）。 */
    const bool caps = nodelio_caps_has_media();
    const bool ok = strcmp(env->status, "ok") == 0;
    const bool play = ok && caps && (env->auto_play || auto_play_first);
    ESP_LOGI(TAG,
             "deliver play=%d env_ap=%d force=%d caps=%d ok=%d card=%s app=%s tracks=%d url0=%d "
             "spoken_wav=%d title=%s",
             play ? 1 : 0, env->auto_play ? 1 : 0, auto_play_first ? 1 : 0, caps ? 1 : 0, ok ? 1 : 0,
             env->card_type[0] ? env->card_type : "-", env->app_id[0] ? env->app_id : "-",
             env->track_n, (env->track_n > 0 && env->tracks[0].playback_url[0]) ? 1 : 0,
             env->spoken_audio_url[0] ? 1 : 0, env->display_title[0] ? env->display_title : "-");

    /* 先摘 spoken/音频 URL：show 接管 env 所有权后不可再读 */
    if (spoken_out && spoken_len) {
        nodelio_envelope_spoken_summary(env, spoken_out, spoken_len);
    }
    s_last_spoken_audio[0] = '\0';
    s_last_spoken_audio_n = 0;
    for (int i = 0; i < NL_ENV_SPOKEN_AUDIO_MAX; i++) {
        s_last_spoken_audios[i][0] = '\0';
    }
    if (env->spoken_audio_n > 0) {
        s_last_spoken_audio_n =
            env->spoken_audio_n < NL_ENV_SPOKEN_AUDIO_MAX ? env->spoken_audio_n : NL_ENV_SPOKEN_AUDIO_MAX;
        for (int i = 0; i < s_last_spoken_audio_n; i++) {
            snprintf(s_last_spoken_audios[i], sizeof(s_last_spoken_audios[i]), "%s",
                     env->spoken_audio_urls[i]);
        }
        snprintf(s_last_spoken_audio, sizeof(s_last_spoken_audio), "%s", s_last_spoken_audios[0]);
    } else if (env->spoken_audio_url[0]) {
        snprintf(s_last_spoken_audio, sizeof(s_last_spoken_audio), "%s", env->spoken_audio_url);
        snprintf(s_last_spoken_audios[0], sizeof(s_last_spoken_audios[0]), "%s",
                 env->spoken_audio_url);
        s_last_spoken_audio_n = 1;
    }
    nodelio_skill_result_show(cfg, env, play);
}

const char *nodelio_skill_last_spoken_audio_url(void)
{
    return s_last_spoken_audio[0] ? s_last_spoken_audio : NULL;
}

int nodelio_skill_last_spoken_audio_urls(char (*urls)[NL_ENV_URL], int max_n)
{
    if (!urls || max_n <= 0) {
        return 0;
    }
    int n = s_last_spoken_audio_n;
    if (n > max_n) {
        n = max_n;
    }
    if (n > NL_ENV_SPOKEN_AUDIO_MAX) {
        n = NL_ENV_SPOKEN_AUDIO_MAX;
    }
    for (int i = 0; i < n; i++) {
        memcpy(urls[i], s_last_spoken_audios[i], NL_ENV_URL);
        urls[i][NL_ENV_URL - 1] = '\0';
    }
    return n;
}
