#include "media_player.h"

#include "application.h"
#include "device_caps.h"
#include "nl_url_util.h"
#include "panel_ui.h"
#include "skill_result_ui.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define TAG "nl_media"

enum media_op_t : uint8_t { MEDIA_OP_PLAY = 1, MEDIA_OP_STOP = 2, MEDIA_OP_RETRY = 3 };

struct media_msg_t {
    media_op_t op;
    char url[NL_ENV_URL];
    char title[NL_ENV_STR];
    char device_key[65];
    int8_t retries_left; /* RETRY：会话未结束时延后再试 */
};

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static char s_now_title[NL_ENV_STR];
static char s_now_url[NL_ENV_URL];
static char s_now_key[65];
static volatile bool s_playing;
static volatile bool s_paused;
static uint32_t s_playback_id = 1;
static TickType_t s_play_started_at;

static bool same_url(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

/** 同 URL 且仍在播 → 跳过，避免 LLM 连打 music.search+auto_play 打断 HTTP 流 */
static bool should_skip_restart(const char *url)
{
    if (!s_playing || !url || !url[0] || !s_now_url[0]) {
        return false;
    }
    return same_url(s_now_url, url);
}

static bool cat_url(char *out, size_t out_len, const char *a, const char *b)
{
    if (!out || out_len == 0 || !a) {
        return false;
    }
    size_t la = strlen(a);
    size_t lb = b ? strlen(b) : 0;
    if (la + lb + 1 > out_len) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, a, la);
    if (lb) {
        memcpy(out + la, b, lb);
    }
    out[la + lb] = '\0';
    return true;
}

static void resolve_url(const nodelio_nvs_config_t *cfg, const char *in, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!in || !in[0] || !out_len) {
        return;
    }
    if (strncmp(in, "http://", 7) == 0 || strncmp(in, "https://", 8) == 0) {
        (void)cat_url(out, out_len, in, nullptr);
        return;
    }
    if (strncmp(in, "sample://", 9) == 0) {
        if (cfg && cfg->server_url[0] && cfg->device_id[0]) {
            char base[160];
            char mid[96];
            nl_url_strip_base(cfg->server_url, base, sizeof(base));
            const char *p1 = "/api/devices/";
            const char *p2 = "/panel/media/sample.ogg";
            size_t l1 = strlen(p1);
            size_t lid = strlen(cfg->device_id);
            size_t l2 = strlen(p2);
            if (l1 + lid + l2 + 1 > sizeof(mid)) {
                return;
            }
            memcpy(mid, p1, l1);
            memcpy(mid + l1, cfg->device_id, lid);
            memcpy(mid + l1 + lid, p2, l2 + 1);
            (void)cat_url(out, out_len, base, mid);
        }
        return;
    }
    if (in[0] == '/' && cfg && cfg->server_url[0]) {
        char base[160];
        nl_url_strip_base(cfg->server_url, base, sizeof(base));
        (void)cat_url(out, out_len, base, in);
        return;
    }
    (void)cat_url(out, out_len, in, nullptr);
}

static void media_worker(void *arg)
{
    (void)arg;
    media_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.op == MEDIA_OP_STOP) {
            Application::GetInstance().Schedule([]() { Application::GetInstance().StopHttpAudio(); });
            s_playing = false;
            if (!s_paused) {
                s_now_title[0] = '\0';
                s_now_url[0] = '\0';
                s_now_key[0] = '\0';
                nodelio_panel_ui_media_now_playing("", false);
            }
            continue;
        }
        if (msg.op != MEDIA_OP_PLAY || msg.url[0] == '\0') {
            continue;
        }
        if (!nodelio_caps_has_media()) {
            nodelio_panel_ui_toast_defer("本机未启用播放");
            continue;
        }

        /* 队列里积压的同曲 PLAY：worker 侧再挡一层 */
        if (should_skip_restart(msg.url)) {
            ESP_LOGI(TAG, "queue skip same url title=%s", msg.title);
            snprintf(s_now_title, sizeof(s_now_title), "%s", msg.title);
            s_playing = true;
            s_paused = false;
            nodelio_panel_ui_media_now_playing(s_now_title, true);
            continue;
        }

        std::string url(msg.url);
        std::string title(msg.title);
        std::string device_key(msg.device_key);
        uint32_t id = ++s_playback_id;
        snprintf(s_now_title, sizeof(s_now_title), "%s", title.c_str());
        snprintf(s_now_url, sizeof(s_now_url), "%s", url.c_str());
        snprintf(s_now_key, sizeof(s_now_key), "%s", device_key.c_str());
        s_playing = true;
        s_paused = false;
        s_play_started_at = xTaskGetTickCount();
        nodelio_panel_ui_media_now_playing(s_now_title, true);

        auto try_start = std::make_shared<std::function<void(int)>>();
        *try_start = [url, title, device_key, id, try_start](int left) {
            auto &app = Application::GetInstance();
            DeviceState st = app.GetDeviceState();
            /* 仅换曲时打断当前 HTTP；同 id 重入不应走到这里 */
            if (st == kDeviceStateNotifying) {
                app.StopHttpAudio();
            } else if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                       st == kDeviceStateConnecting) {
                app.SetDeviceState(kDeviceStateIdle);
            }
            if (app.GetDeviceState() != kDeviceStateIdle) {
                if (left > 0) {
                    app.Schedule([try_start, left]() { (*try_start)(left - 1); });
                } else {
                    ESP_LOGW(TAG, "play give up title=%s", title.c_str());
                    s_playing = false;
                    s_now_url[0] = '\0';
                    nodelio_panel_ui_media_now_playing("", false);
                }
                return;
            }
            /* 若排队期间又被要求播别的曲，丢弃过期启动 */
            if (!same_url(s_now_url, url.c_str())) {
                ESP_LOGW(TAG, "stale play drop id=%lu", (unsigned long)id);
                return;
            }
            app.PlayHttpAudio(url, device_key);
            ESP_LOGI(TAG, "play id=%lu title=%s", (unsigned long)id, title.c_str());
        };
        Application::GetInstance().Schedule([try_start]() { (*try_start)(8); });
    }
}

esp_err_t nodelio_media_init(void)
{
    if (s_q) {
        return ESP_OK;
    }
    s_q = xQueueCreate(4, sizeof(media_msg_t));
    if (!s_q) {
        return ESP_ERR_NO_MEM;
    }
    constexpr uint32_t kStack = 10240;
    StackType_t *stack =
        (StackType_t *)heap_caps_malloc(kStack * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb =
        (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack && tcb) {
        s_task = xTaskCreateStatic(media_worker, "nl_media", kStack, nullptr, 3, stack, tcb);
    } else {
        free(stack);
        free(tcb);
        xTaskCreate(media_worker, "nl_media", kStack, nullptr, 3, &s_task);
    }
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

bool nodelio_media_is_playing(void)
{
    return s_playing;
}

bool nodelio_media_is_paused(void)
{
    return s_paused && s_now_title[0] != '\0';
}

bool nodelio_media_is_playing_url(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    return should_skip_restart(url);
}

esp_err_t nodelio_media_play_track(const nodelio_nvs_config_t *cfg, const nl_env_track_t *track)
{
    if (!track || track->playback_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nodelio_caps_has("media.play_local")) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!s_q && nodelio_media_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
    media_msg_t msg{};
    msg.op = MEDIA_OP_PLAY;
    resolve_url(cfg, track->playback_url, msg.url, sizeof(msg.url));
    if (msg.url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(msg.title, sizeof(msg.title), "%s", track->title[0] ? track->title : "播放中");
    if (cfg && cfg->device_key[0]) {
        snprintf(msg.device_key, sizeof(msg.device_key), "%s", cfg->device_key);
    }

    if (should_skip_restart(msg.url)) {
        ESP_LOGI(TAG, "skip restart same url title=%s", msg.title);
        snprintf(s_now_title, sizeof(s_now_title), "%s", msg.title);
        s_playing = true;
        s_paused = false;
        nodelio_panel_ui_media_now_playing(s_now_title, true);
        return ESP_OK;
    }

    /* 换曲才停；同曲不发 STOP，避免掐断下载。暂停态不抑制清空。 */
    s_paused = false;
    media_msg_t stop{.op = MEDIA_OP_STOP};
    xQueueSend(s_q, &stop, 0);
    if (xQueueSend(s_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void nodelio_media_stop(void)
{
    if (!s_q) {
        return;
    }
    s_paused = false;
    media_msg_t msg{.op = MEDIA_OP_STOP};
    xQueueSend(s_q, &msg, 0);
}

void nodelio_media_pause(void)
{
    if (!s_playing || !s_now_url[0]) {
        return;
    }
    s_paused = true;
    s_playing = false;
    /* 先刷新 UI（保留条），再掐流。ended 回调见到 paused 不会清标题 */
    nodelio_panel_ui_media_now_playing(s_now_title, false);
    Application::GetInstance().Schedule([]() { Application::GetInstance().StopHttpAudio(); });
    ESP_LOGI(TAG, "paused title=%s", s_now_title);
}

void nodelio_media_resume(void)
{
    if (!s_paused || !s_now_url[0]) {
        return;
    }
    if (!s_q && nodelio_media_init() != ESP_OK) {
        return;
    }
    media_msg_t msg{};
    msg.op = MEDIA_OP_PLAY;
    snprintf(msg.url, sizeof(msg.url), "%s", s_now_url);
    snprintf(msg.title, sizeof(msg.title), "%s", s_now_title[0] ? s_now_title : "播放中");
    snprintf(msg.device_key, sizeof(msg.device_key), "%s", s_now_key);
    s_paused = false;
    if (xQueueSend(s_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_paused = true;
        ESP_LOGW(TAG, "resume queue full");
    }
}

void nodelio_media_now_title(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    snprintf(buf, len, "%s", s_now_title);
}

void nodelio_media_on_playback_ended(bool success)
{
    (void)success;
    /* 暂停掐流也会走到这里：保留标题与 URL，播放器不关 */
    if (s_paused) {
        return;
    }
    /* 若已换曲进入新一轮 notifying，勿清新曲状态 */
    if (Application::GetInstance().GetDeviceState() == kDeviceStateNotifying) {
        return;
    }
    s_playing = false;
    s_now_url[0] = '\0';
    s_now_title[0] = '\0';
    s_now_key[0] = '\0';
    nodelio_panel_ui_media_now_playing("", false);
}
