#include "media_player.h"

#include "application.h"
#include "board.h"
#include "device_caps.h"
#include "nl_url_util.h"
#include "nvs_config.h"
#include "panel_ui.h"
#include "skill_result_ui.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <ctime>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define TAG "nl_media"

enum media_op_t : uint8_t { MEDIA_OP_PLAY = 1, MEDIA_OP_STOP = 2 };

struct media_msg_t {
    media_op_t op;
    char url[NL_ENV_URL];
    char title[NL_ENV_STR];
    char device_key[65];
};

static QueueHandle_t s_q;
static TaskHandle_t s_task;
static char s_now_title[NL_ENV_STR];
static char s_now_url[NL_ENV_URL];
/** 已经交给 HTTP 播放器的地址。请求地址会提前写入 s_now_url，不能拿它判断「同一首」。 */
static char s_started_url[NL_ENV_URL];
static char s_now_key[65];
static volatile bool s_playing;
static volatile bool s_paused;
static uint32_t s_playback_id = 1;
static TickType_t s_play_started_at;
static int s_retry_count = 0;
/** 主动掐流换曲时，忽略上一首的 ended，避免误清状态或再跳一首。 */
static volatile bool s_suppress_ended = false;
/** 暂停后无操作则关掉播放器；秒数来自 NVS「播放器」设置。 */
static esp_timer_handle_t s_pause_close_timer;
/** 语音打断后应在退下时续播（与用户手动暂停区分）。 */
static volatile bool s_resume_after_voice = false;
/** 说话未结束时把 PLAY 退回队列的次数，成功开播后清零。 */
static int s_defer_n = 0;

static void pause_close_cb(void *arg)
{
    (void)arg;
    if (!s_paused) {
        return;
    }
    ESP_LOGI(TAG, "pause timeout, close player");
    nodelio_media_stop();
}

static void disarm_pause_close(void)
{
    if (s_pause_close_timer) {
        esp_timer_stop(s_pause_close_timer);
    }
}

static void arm_pause_close(void)
{
    if (!s_pause_close_timer) {
        esp_timer_create_args_t args = {};
        args.callback = pause_close_cb;
        args.name = "nl_pause_close";
        if (esp_timer_create(&args, &s_pause_close_timer) != ESP_OK) {
            ESP_LOGW(TAG, "pause close timer create failed");
            return;
        }
    }
    esp_timer_stop(s_pause_close_timer);
    uint16_t sec = nodelio_media_pause_seconds();
    if (sec < NODELIO_MEDIA_PAUSE_MIN) {
        sec = NODELIO_MEDIA_PAUSE_MIN;
    }
    esp_timer_start_once(s_pause_close_timer, (uint64_t)sec * 1000000ull);
}

static bool same_url(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    return strcmp(a, b) == 0;
}

static const char *url_tail(const char *url)
{
    if (!url || !url[0]) {
        return "";
    }
    size_t n = strlen(url);
    return n > 48 ? url + (n - 48) : url;
}

/** 仅当这条 URL 已经在出声时才跳过。s_now_url 在入队前就会改成新曲。 */
static bool should_skip_restart(const char *url)
{
    if (!s_playing || !url || !url[0] || !s_started_url[0]) {
        return false;
    }
    return same_url(s_started_url, url);
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

/** StartNotification 在上一首 HTTP 任务未退出时会直接丢弃。先停再等。 */
static void release_previous_http(void)
{
    auto &app = Application::GetInstance();
    DeviceState st0 = app.GetDeviceState();
    const bool busy0 = app.IsHttpAudioBusy();
    const bool need_stop = st0 == kDeviceStateNotifying || busy0;
    ESP_LOGI(TAG, "release prev state=%s busy=%d stop=%d", DeviceStateMachine::GetStateName(st0),
             (int)busy0, (int)need_stop);
    if (need_stop) {
        s_suppress_ended = true;
        app.Schedule([]() { Application::GetInstance().StopHttpAudio(); });
    }
    for (int i = 0; i < 80; ++i) {
        DeviceState st = app.GetDeviceState();
        if (!app.IsHttpAudioBusy() && st != kDeviceStateNotifying && st != kDeviceStateSpeaking) {
            s_suppress_ended = false;
            ESP_LOGI(TAG, "release prev done i=%d state=%s", i, DeviceStateMachine::GetStateName(st));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_suppress_ended = false;
    ESP_LOGW(TAG, "wait previous audio timeout state=%s busy=%d",
             DeviceStateMachine::GetStateName(app.GetDeviceState()), (int)app.IsHttpAudioBusy());
}

static bool speech_busy(void)
{
    auto &app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        return true;
    }
    return !app.GetAudioService().IsPlaybackIdle();
}

/**
 * 语音里点歌：先等「即将播放」这句说完再开流。
 * 退下续播：通道已关但尾句还在解码器里时同样要等，否则重试会把曲目清掉。
 * 队列里已有下一条指令则放弃本次，交给后续 PLAY/STOP。
 */
static bool wait_out_voice_cue(void)
{
    auto &app = Application::GetInstance();
    DeviceState st = app.GetDeviceState();
    const bool in_session = st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                            st == kDeviceStateConnecting;
    if (!in_session) {
        for (int i = 0; i < 200 && speech_busy(); ++i) {
            if (uxQueueMessagesWaiting(s_q) > 0) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return true;
    }

    if (st != kDeviceStateSpeaking) {
        for (int i = 0; i < 10; ++i) {
            if (uxQueueMessagesWaiting(s_q) > 0) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            st = app.GetDeviceState();
            if (st == kDeviceStateSpeaking ||
                (st != kDeviceStateListening && st != kDeviceStateConnecting)) {
                break;
            }
        }
    }

    int quiet = 0;
    for (int i = 0; i < 200; ++i) {
        if (uxQueueMessagesWaiting(s_q) > 0) {
            return false;
        }
        if (speech_busy()) {
            quiet = 0;
        } else {
            st = app.GetDeviceState();
            if (st != kDeviceStateListening && st != kDeviceStateConnecting &&
                st != kDeviceStateSpeaking) {
                return true;
            }
            quiet++;
            if (quiet >= 3) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return true;
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
            s_suppress_ended = true;
            s_playing = false;
            Application::GetInstance().Schedule([]() { Application::GetInstance().StopHttpAudio(); });
            if (!s_paused) {
                s_now_title[0] = '\0';
                s_now_url[0] = '\0';
                s_started_url[0] = '\0';
                s_now_key[0] = '\0';
                nodelio_panel_ui_media_now_playing("", false);
            }
            for (int i = 0; i < 80 && Application::GetInstance().IsHttpAudioBusy(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            s_suppress_ended = false;
            continue;
        }
        if (msg.op != MEDIA_OP_PLAY || msg.url[0] == '\0') {
            continue;
        }
        if (!nodelio_caps_has_media()) {
            nodelio_panel_ui_toast_defer("本机未启用播放");
            continue;
        }

        /* 只跳过「这条流已经在播」。入队前 s_now_url 已是新曲，不能拿它比较。 */
        if (should_skip_restart(msg.url)) {
            ESP_LOGI(TAG, "queue skip streaming title=%s tail=%s", msg.title, url_tail(msg.url));
            snprintf(s_now_title, sizeof(s_now_title), "%s", msg.title);
            s_playing = true;
            s_paused = false;
            nodelio_panel_ui_media_now_playing(s_now_title, true);
            continue;
        }
        ESP_LOGI(TAG, "worker play title=%s state=%s busy=%d tail=%s", msg.title,
                 DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()),
                 (int)Application::GetInstance().IsHttpAudioBusy(), url_tail(msg.url));

        if (!wait_out_voice_cue()) {
            ESP_LOGI(TAG, "play superseded title=%s", msg.title);
            continue;
        }

        /* 重试请求：延迟 2 秒后再启动，避免立即重试 */
        if (s_retry_count > 0) {
            ESP_LOGI(TAG, "retry delay 2s: %s", msg.title);
            vTaskDelay(pdMS_TO_TICKS(2000));
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
        /* 允许播放息屏时切歌不要唤醒。常亮才记活动，且必须丢到主任务：
         * 本任务栈在 PSRAM，息屏 RestoreBrightness 会读 NVS，关 cache 时栈不可用会断言重启。 */
        if (!nodelio_media_sleep_enabled()) {
            Application::GetInstance().Schedule([]() { Board::GetInstance().NoteUserActivity(); });
        }
        /* 上一首的下载任务没退出时，StartNotification 会丢弃本次播放 */
        release_previous_http();
        if (!same_url(s_now_url, url.c_str())) {
            ESP_LOGW(TAG, "stale play drop before start id=%lu", (unsigned long)id);
            continue;
        }

        auto try_start = std::make_shared<std::function<void(int)>>();
        *try_start = [url, title, device_key, id, try_start](int left) {
            auto &app = Application::GetInstance();
            if (!same_url(s_now_url, url.c_str())) {
                ESP_LOGW(TAG, "stale play drop id=%lu", (unsigned long)id);
                return;
            }
            DeviceState st = app.GetDeviceState();
            if (st == kDeviceStateNotifying || app.IsHttpAudioBusy()) {
                ESP_LOGW(TAG, "try_start busy left=%d state=%s http=%d title=%s", left,
                         DeviceStateMachine::GetStateName(st), (int)app.IsHttpAudioBusy(), title.c_str());
                app.StopHttpAudio();
                if (left > 0) {
                    app.Schedule([try_start, left]() { (*try_start)(left - 1); });
                } else {
                    ESP_LOGW(TAG, "play give up busy title=%s", title.c_str());
                    s_playing = false;
                    s_now_url[0] = '\0';
                    s_started_url[0] = '\0';
                    nodelio_panel_ui_media_now_playing("", false);
                }
                return;
            }
            if (st == kDeviceStateSpeaking || !app.GetAudioService().IsPlaybackIdle()) {
                /* 每次回到 worker 都会重新等一句；超过次数就开播，避免播放队列一直不空时卡死 */
                if (s_defer_n < 8) {
                    s_defer_n++;
                    ESP_LOGI(TAG, "defer play until speech ends n=%d title=%s", s_defer_n, title.c_str());
                    media_msg_t again{};
                    again.op = MEDIA_OP_PLAY;
                    snprintf(again.url, sizeof(again.url), "%s", url.c_str());
                    snprintf(again.title, sizeof(again.title), "%s", title.c_str());
                    snprintf(again.device_key, sizeof(again.device_key), "%s", device_key.c_str());
                    if (xQueueSend(s_q, &again, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "defer play queue full title=%s", title.c_str());
                    }
                    return;
                }
                ESP_LOGW(TAG, "speech still busy, start anyway title=%s", title.c_str());
            }
            s_defer_n = 0;
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.SetDeviceState(kDeviceStateIdle);
            }
            if (st == kDeviceStateListening || st == kDeviceStateConnecting) {
                app.YieldVoiceForMedia();
                app.SetDeviceState(kDeviceStateIdle);
            }
            if (app.GetDeviceState() != kDeviceStateIdle) {
                ESP_LOGW(TAG, "try_start not idle left=%d state=%s title=%s", left,
                         DeviceStateMachine::GetStateName(app.GetDeviceState()), title.c_str());
                if (left > 0) {
                    app.Schedule([try_start, left]() { (*try_start)(left - 1); });
                } else {
                    ESP_LOGW(TAG, "play give up title=%s", title.c_str());
                    s_playing = false;
                    s_now_url[0] = '\0';
                    s_started_url[0] = '\0';
                    nodelio_panel_ui_media_now_playing("", false);
                }
                return;
            }
            if (!app.PlayHttpAudio(url, device_key)) {
                ESP_LOGW(TAG, "PlayHttpAudio false left=%d state=%s busy=%d title=%s", left,
                         DeviceStateMachine::GetStateName(app.GetDeviceState()), (int)app.IsHttpAudioBusy(),
                         title.c_str());
                if (left > 0) {
                    app.Schedule([try_start, left]() { (*try_start)(left - 1); });
                } else {
                    ESP_LOGW(TAG, "play give up title=%s", title.c_str());
                    s_playing = false;
                    s_now_url[0] = '\0';
                    s_started_url[0] = '\0';
                    nodelio_panel_ui_media_now_playing("", false);
                }
                return;
            }
            snprintf(s_started_url, sizeof(s_started_url), "%s", url.c_str());
            ESP_LOGI(TAG, "play id=%lu title=%s tail=%s", (unsigned long)id, title.c_str(), url_tail(url.c_str()));
        };
        Application::GetInstance().Schedule([try_start]() { (*try_start)(20); });
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
    /* 本机曲库 URL 无 expires；外链才校验。墙钟未同步（时钟显示 --:--）时跳过，避免误报。 */
    const bool first_party =
        track->playback_url[0] && strstr(track->playback_url, "/panel/media/") != nullptr;
    if (!first_party && track->expires_at > 0) {
        const time_t now = time(nullptr);
        struct tm tm_now {};
        if (now > 0 && localtime_r(&now, &tm_now) && tm_now.tm_year >= (2025 - 1900) &&
            (int64_t)now > track->expires_at) {
            nodelio_panel_ui_toast_defer("链接已过期");
            return ESP_ERR_INVALID_STATE;
        }
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
        ESP_LOGI(TAG, "skip restart streaming title=%s tail=%s", msg.title, url_tail(msg.url));
        disarm_pause_close();
        snprintf(s_now_title, sizeof(s_now_title), "%s", msg.title);
        s_playing = true;
        s_paused = false;
        nodelio_panel_ui_media_now_playing(s_now_title, true);
        return ESP_OK;
    }

    /* 只入队 PLAY。worker 里先等上一首 HTTP 退出再开流，避免 STOP 与 PLAY 抢主循环把新曲掐掉。 */
    disarm_pause_close();
    s_paused = false;
    s_retry_count = 0;
    snprintf(s_now_title, sizeof(s_now_title), "%s", msg.title);
    snprintf(s_now_url, sizeof(s_now_url), "%s", msg.url);
    snprintf(s_now_key, sizeof(s_now_key), "%s", msg.device_key);
    ESP_LOGI(TAG, "enqueue title=%s playing=%d tail=%s", msg.title, (int)s_playing, url_tail(msg.url));

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
    disarm_pause_close();
    s_paused = false;
    s_resume_after_voice = false;
    media_msg_t msg{.op = MEDIA_OP_STOP};
    xQueueSend(s_q, &msg, 0);
}

void nodelio_media_pause(void)
{
    if (!s_playing || !s_now_url[0]) {
        return;
    }
    s_resume_after_voice = false;
    s_paused = true;
    s_playing = false;
    arm_pause_close();
    /* 先刷新 UI（保留条），再掐流。ended 回调见到 paused 不会清标题 */
    nodelio_panel_ui_media_now_playing(s_now_title, false);
    Application::GetInstance().Schedule([]() { Application::GetInstance().StopHttpAudio(); });
    ESP_LOGI(TAG, "paused title=%s", s_now_title);
}

void nodelio_media_pause_for_voice(void)
{
    if (!s_playing || !s_now_url[0]) {
        return;
    }
    s_paused = true;
    s_playing = false;
    s_resume_after_voice = true;
    disarm_pause_close();
    nodelio_panel_ui_media_now_playing(s_now_title, false);
    /* 主任务同步掐流，避免 Schedule 晚于 BeginWakeWordInvoke */
    Application::GetInstance().StopHttpAudio();
    ESP_LOGI(TAG, "paused for voice title=%s", s_now_title);
}

bool nodelio_media_resume_after_voice(void)
{
    if (!s_resume_after_voice) {
        return false;
    }
    s_resume_after_voice = false;
    if (!s_paused || !s_now_url[0]) {
        return false;
    }
    ESP_LOGI(TAG, "resume after voice title=%s", s_now_title);
    nodelio_media_resume();
    return true;
}

bool nodelio_media_preempt_for_heavy_app(void)
{
    auto &app = Application::GetInstance();
    const bool had =
        s_playing || s_paused || s_now_url[0] != '\0' || app.IsHttpAudioBusy() ||
        app.GetDeviceState() == kDeviceStateNotifying;
    if (!had) {
        return false;
    }
    s_resume_after_voice = false;
    nodelio_media_stop();
    /* STOP 入队后仍可能短暂 busy；等空闲再开 TLS */
    for (int i = 0; i < 100 && (app.IsHttpAudioBusy() || app.GetDeviceState() == kDeviceStateNotifying);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (app.IsHttpAudioBusy() || app.GetDeviceState() == kDeviceStateNotifying) {
        app.Schedule([]() { Application::GetInstance().StopHttpAudio(); });
        for (int i = 0; i < 40 && Application::GetInstance().IsHttpAudioBusy(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    return true;
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
    disarm_pause_close();
    s_paused = false;
    s_resume_after_voice = false;
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

bool nodelio_media_has_session(void)
{
    return s_playing;
}

void nodelio_media_on_playback_ended(bool success)
{
    /* 暂停或换曲掐流：保留当前曲目，不自动跳下一首 */
    if (s_paused || s_suppress_ended) {
        return;
    }
    /* 若已换曲进入新一轮 notifying 且播放成功：勿清新曲状态 */
    if (success && Application::GetInstance().GetDeviceState() == kDeviceStateNotifying) {
        return;
    }
    /* 播放失败且未超过重试次数：发送重试消息到队列（由 worker 处理延迟） */
    if (!success && s_playing && s_now_url[0] != '\0' && s_retry_count == 0 && s_q) {
        ESP_LOGW(TAG, "playback failed, will retry: %s", s_now_title);
        s_retry_count++;
        media_msg_t msg{};
        msg.op = MEDIA_OP_PLAY;
        snprintf(msg.url, sizeof(msg.url), "%s", s_now_url);
        snprintf(msg.title, sizeof(msg.title), "%s", s_now_title);
        snprintf(msg.device_key, sizeof(msg.device_key), "%s", s_now_key);
        xQueueSend(s_q, &msg, 0);
        return;
    }
    /* 自然播完：切到列表下一首。须先清 playing，否则同 URL 会被 should_skip_restart 吃掉。 */
    if (success) {
        s_playing = false;
        s_retry_count = 0;
        if (nodelio_skill_media_next()) {
            return;
        }
    }
    /* 没有下一首，或失败重试耗尽：关闭播放器 */
    s_playing = false;
    s_retry_count = 0;
    s_now_url[0] = '\0';
    s_started_url[0] = '\0';
    s_now_title[0] = '\0';
    s_now_key[0] = '\0';
    nodelio_panel_ui_media_now_playing("", false);
}
