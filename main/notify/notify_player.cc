#include "notify_player.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "application.h"
#include "board.h"
#include "esp_aac_dec.h"
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
#include "offline_cmd.h"
#endif
#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_mp3_dec.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace {
constexpr int kHttpTimeoutMs = 15000;
#if CONFIG_BOARD_TYPE_SILA_P4C5_62
/* P4 PSRAM 充裕：加大读块，少系统调用、直播更稳 */
constexpr size_t kHttpReadBufferSize = 8192;
#else
constexpr size_t kHttpReadBufferSize = 4096;
#endif
/* AAC/M4A 解码栈偏大；任务栈在 PSRAM */
constexpr uint32_t kNotifyTaskStackSize = 24576;
/* 调度铁律（SILA-86）：audio_output(6) 钉 Core1 是硬期限任务，I2S TX DMA 只有
 * 6×240 帧（~30ms）深度，解码/下载必须低于它。旧值 7 高于输出任务，非阻塞化后
 * 每个解码突发都会把输出饿穿 30ms → 听感就是周期性「卡一下」。 */
constexpr UBaseType_t kNotifyTaskPriority = 5;
/* Ogg 保留原预取；MP3/M4A 有独立 PCM 暖机，直播 96KB 预取会额外等约 12s。
 * 禁止按上限截断最后一次 Read：多出来的字节已从套接字拿走，丢掉会在预取边界跳针。 */
constexpr size_t kPrefetchBytes = 96 * 1024;
/* 压缩预取：8KB 偏紧（64kbps≈1s），半帧易被报 NOT_SUPPORT → 跳针 */
constexpr size_t kCompressedPrefetchBytes = 16 * 1024;
/* 播中把压缩缓冲维持到此水位，避免「解完 prefetch 才去 Read」→ 约 8–12s 见底跳一下。 */
constexpr size_t kPendingHighWatermark = 160 * 1024;
/* 压缩过低则必须 Read；高于此值时若播放侧正饿，先 flush/decode，禁止堵在 Read 上把喇叭抽空。 */
constexpr size_t kPendingLowWatermark = 32 * 1024;
/* 开声前攒够暖机 PCM；AFE 已停喂，略收延迟。 */
constexpr uint32_t kMp3WarmupMs = 4000;
constexpr uint32_t kMp3BatchMs = 2000;
/* stage 里最多再囤这么多，多了先去拉网/等播放，防解堵死下载。 */
constexpr uint32_t kMp3StageMaxMs = 10000;
/* 尾部不足此值时的解码失败多半是半帧，等网勿重同步 */
constexpr size_t kMp3ResyncMinAvail = 2048;
/** P4：加厚 MP3/M4A 垫片（电台/播客）；86 / host 测仍用上方保守常量。 */
static size_t Mp3CompressedPrefetchBytes()
{
#if defined(CONFIG_BOARD_TYPE_SILA_P4C5_62) && CONFIG_BOARD_TYPE_SILA_P4C5_62
    return 48 * 1024; /* ~6s @64kbps */
#else
    return kCompressedPrefetchBytes;
#endif
}
static size_t Mp3PendingHighWatermark()
{
#if defined(CONFIG_BOARD_TYPE_SILA_P4C5_62) && CONFIG_BOARD_TYPE_SILA_P4C5_62
    return 256 * 1024;
#else
    return kPendingHighWatermark;
#endif
}
static size_t Mp3PendingLowWatermark()
{
#if defined(CONFIG_BOARD_TYPE_SILA_P4C5_62) && CONFIG_BOARD_TYPE_SILA_P4C5_62
    return 48 * 1024;
#else
    return kPendingLowWatermark;
#endif
}
static uint32_t Mp3WarmupMs()
{
#if defined(CONFIG_BOARD_TYPE_SILA_P4C5_62) && CONFIG_BOARD_TYPE_SILA_P4C5_62
    return 5000;
#else
    return kMp3WarmupMs;
#endif
}
static uint32_t Mp3StageMaxMs()
{
#if defined(CONFIG_BOARD_TYPE_SILA_P4C5_62) && CONFIG_BOARD_TYPE_SILA_P4C5_62
    return 14000;
#else
    return kMp3StageMaxMs;
#endif
}
/* 解码任务优先级高于 AFE fetch，连续解码（含坏帧重同步）须给 fetch 运行机会。 */
class DecodeTimeslice {
public:
    void YieldIfNeeded() {
        if (esp_timer_get_time() - started_us_ >= 10000) {
            vTaskDelay(1);
            started_us_ = esp_timer_get_time();
        }
    }

private:
    int64_t started_us_ = esp_timer_get_time();
};
const char* TAG = "NotifyPlayer";
constexpr int kMaxRedirects = 4;

/** 大缓冲走 PSRAM。失败禁止 abort（无 C++ 异常时 new 失败会整机重启）。 */
static uint8_t s_psram_poison;
static bool s_psram_alloc_failed;

template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
        const size_t bytes = n * sizeof(T);
        /* 大缓冲只许 PSRAM：回落 INTERNAL 会打穿播中唤醒所需连续块 */
        void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "PSRAM alloc fail need=%u psram_free=%u largest=%u (no INTERNAL fallback)",
                     (unsigned)bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            s_psram_alloc_failed = true;
            return reinterpret_cast<T*>(&s_psram_poison);
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
        if (p == reinterpret_cast<T*>(&s_psram_poison)) {
            return;
        }
        heap_caps_free(p);
    }
    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept {
        return true;
    }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept {
        return false;
    }
};

using PsramBytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;
using PsramSamples = std::vector<int16_t, PsramAllocator<int16_t>>;

static bool PsramCanHold(size_t bytes) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes;
}

enum class StreamKind { kUnknown, kOgg, kMp3, kM4a, kWav };

std::string TrimHeaderValue(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

bool IsRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::string UrlHost(const std::string& url) {
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const auto host_begin = scheme + 3;
    auto host_end = url.find('/', host_begin);
    if (host_end == std::string::npos) {
        host_end = url.size();
    }
    auto colon = url.find(':', host_begin);
    if (colon != std::string::npos && colon < host_end) {
        host_end = colon;
    }
    return url.substr(host_begin, host_end - host_begin);
}

std::string ResolveLocation(const std::string& current, const std::string& raw) {
    const std::string loc = TrimHeaderValue(raw);
    if (loc.empty()) {
        return {};
    }
    if (loc.rfind("https://", 0) == 0 || loc.rfind("http://", 0) == 0) {
        return loc;
    }
    const auto scheme = current.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    if (loc.rfind("//", 0) == 0) {
        return current.substr(0, scheme + 1) + loc;
    }
    const auto host_begin = scheme + 3;
    const auto path = current.find('/', host_begin);
    const std::string origin = path == std::string::npos ? current : current.substr(0, path);
    if (loc[0] == '/') {
        return origin + loc;
    }
    if (path == std::string::npos) {
        return origin + "/" + loc;
    }
    const auto slash = current.rfind('/');
    if (slash == std::string::npos || slash < host_begin) {
        return origin + "/" + loc;
    }
    return current.substr(0, slash + 1) + loc;
}

bool IsMp3Frame(const uint8_t* p, size_t n) {
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return false;
    }
    const int ver = (p[1] >> 3) & 0x3;
    const int layer = (p[1] >> 1) & 0x3;
    const int bitrate = (p[2] >> 4) & 0xF;
    const int sample_rate = (p[2] >> 2) & 0x3;
    return ver != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && sample_rate != 3;
}

/** 合法帧长；0=无效。用于重同步时过滤假同步字。 */
size_t Mp3FrameLength(const uint8_t* p, size_t n)
{
    if (!IsMp3Frame(p, n)) {
        return 0;
    }
    const int ver = (p[1] >> 3) & 0x3;   /* 3=MPEG1 2=MPEG2 0=MPEG2.5 */
    const int layer = (p[1] >> 1) & 0x3; /* 1=III 2=II 3=I */
    const int br_idx = (p[2] >> 4) & 0xF;
    const int sr_idx = (p[2] >> 2) & 0x3;
    const int padding = (p[2] >> 1) & 0x1;
    /* bitrate kbps：按 MPEG1 LayerIII / MPEG2 LayerIII 常用表 */
    static const int kBrMpeg1L3[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBrMpeg2L3[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kSrMpeg1[] = {44100, 48000, 32000, 0};
    static const int kSrMpeg2[] = {22050, 24000, 16000, 0};
    static const int kSrMpeg25[] = {11025, 12000, 8000, 0};
    int bitrate = 0;
    int samplerate = 0;
    if (ver == 3) {
        bitrate = kBrMpeg1L3[br_idx];
        samplerate = kSrMpeg1[sr_idx];
    } else if (ver == 2) {
        bitrate = kBrMpeg2L3[br_idx];
        samplerate = kSrMpeg2[sr_idx];
    } else {
        bitrate = kBrMpeg2L3[br_idx];
        samplerate = kSrMpeg25[sr_idx];
    }
    if (bitrate <= 0 || samplerate <= 0) {
        return 0;
    }
    /* Layer I 少见；电台/播客几乎都是 Layer III */
    if (layer == 3) {
        return static_cast<size_t>((12 * bitrate * 1000 / samplerate + padding) * 4);
    }
    const int scale = (ver == 3) ? 144 : 72;
    return static_cast<size_t>(scale * bitrate * 1000 / samplerate + padding);
}

StreamKind SniffAudio(const uint8_t* p, size_t n) {
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WAVE", 4) == 0) {
        return StreamKind::kWav;
    }
    if (n >= 4 && std::memcmp(p, "OggS", 4) == 0) {
        return StreamKind::kOgg;
    }
    if (n >= 3 && std::memcmp(p, "ID3", 3) == 0) {
        return StreamKind::kMp3;
    }
    /* ISO BMFF：box size + "ftyp"（偶有前置 free/wide，扫前 1KB） */
    const size_t ftyp_limit = n > 1024 ? 1024 : n;
    for (size_t i = 0; i + 8 <= ftyp_limit; ++i) {
        if (std::memcmp(p + i + 4, "ftyp", 4) == 0) {
            return StreamKind::kM4a;
        }
    }
    const size_t limit = n > 512 ? 512 : n;
    for (size_t i = 0; i + 4 <= limit; ++i) {
        if (IsMp3Frame(p + i, n - i)) {
            return StreamKind::kMp3;
        }
    }
    return StreamKind::kUnknown;
}

bool LooksM4aHint(const std::string& url, const std::string& ctype) {
    auto has = [](const std::string& s, const char* needle) {
        return s.find(needle) != std::string::npos;
    };
    std::string c = ctype;
    std::string u = url;
    for (char& ch : c) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    for (char& ch : u) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    if (has(c, "audio/mp4") || has(c, "audio/aac") || has(c, "audio/x-m4a") || has(c, "audio/m4a") ||
        has(c, "video/mp4")) {
        return true;
    }
    return has(u, ".m4a") || has(u, ".aac") || has(u, ".mp4");
}

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

void EnsureM4aDecoderRegistered() {
    static bool once = false;
    if (!once) {
        /* m4a 只是容器；喜马拉雅等轨是 AAC，须先注册底层解码器 */
        (void)esp_aac_dec_register();
        (void)esp_m4a_dec_register();
        once = true;
    }
}

enum class HttpAppendResult { kOk, kEos, kFail };

using IcyState = NotifyIcyState;

/** 找下一个 MP3 帧同步字；优先「本帧长合法且下一帧也像同步」；找不到则保留尾部 ≤3 字节。 */
size_t FindNextMp3Sync(const uint8_t* p, size_t n, size_t from)
{
    if (!p || n < 4 || from >= n) {
        return n;
    }
    size_t weak = n;
    for (size_t i = from; i + 4 <= n; ++i) {
        const size_t len = Mp3FrameLength(p + i, n - i);
        if (len < 24 || len > 2881) {
            continue;
        }
        if (i + len + 4 <= n) {
            if (IsMp3Frame(p + i + len, n - (i + len))) {
                return i;
            }
            continue;
        }
        /* 缓冲末尾半帧：只作弱候选，中间假同步直接跳过 */
        if (n - i < kMp3ResyncMinAvail && weak == n) {
            weak = i;
        }
    }
    if (weak < n) {
        return weak;
    }
    return n >= 3 ? n - 3 : n;
}

int ParseIcyMetaint(const std::string& raw)
{
    if (raw.empty()) {
        return 0;
    }
    char* end = nullptr;
    long v = strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || v <= 0 || v > 1024 * 1024) {
        return 0;
    }
    return static_cast<int>(v);
}

HttpAppendResult HttpAppend(Http* http, PsramBytes& dest, std::array<char, kHttpReadBufferSize>& buffer,
                            IcyState* icy = nullptr)
{
    auto size = http->Read(buffer.data(), buffer.size());
    if (!size) {
        ESP_LOGE(TAG, "Notification HTTP read failed: %s", size.error().ToString().c_str());
        return HttpAppendResult::kFail;
    }
    if (*size == 0) {
        return HttpAppendResult::kEos;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer.data());
    size_t n = static_cast<size_t>(*size);
    if (!icy || icy->metaint <= 0) {
        dest.insert(dest.end(), p, p + n);
    } else {
        /* Icecast/Shoutcast：每 metaint 字节音频后 1 字节长度×16 的元数据块。 */
        while (n > 0) {
            if (icy->meta_left > 0) {
                const size_t skip = n < icy->meta_left ? n : icy->meta_left;
                p += skip;
                n -= skip;
                icy->meta_left -= skip;
                continue;
            }
            if (icy->await_len) {
                icy->meta_left = static_cast<size_t>(*p) * 16u;
                icy->await_len = false;
                ++p;
                --n;
                continue;
            }
            const size_t room = static_cast<size_t>(icy->metaint) - icy->audio_since_meta;
            const size_t take = n < room ? n : room;
            dest.insert(dest.end(), p, p + take);
            p += take;
            n -= take;
            icy->audio_since_meta += take;
            if (icy->audio_since_meta >= static_cast<size_t>(icy->metaint)) {
                icy->audio_since_meta = 0;
                icy->await_len = true;
            }
        }
    }
    if (s_psram_alloc_failed) {
        ESP_LOGE(TAG, "PSRAM allocator degraded, abort HTTP append");
        return HttpAppendResult::kFail;
    }
    return HttpAppendResult::kOk;
}
}  // namespace

NotifyPlayer::NotifyPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

NotifyPlayer::~NotifyPlayer() { Stop(); }

bool NotifyPlayer::Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                         uint32_t playback_id, SubtitleCallback subtitle_callback,
                         FinishedCallback finished_callback, std::string device_key) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        return false;
    }

    std::stable_sort(subtitles.begin(), subtitles.end(),
                     [](const NotifySubtitle& left, const NotifySubtitle& right) {
                         return left.start_ms < right.start_ms;
                     });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            return false;
        }
        audio_url_ = std::move(audio_url);
        device_key_ = std::move(device_key);
        subtitles_ = std::move(subtitles);
        displayed_text_.clear();
        subtitle_callback_ = std::move(subtitle_callback);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        last_playback_position_ms_ = 0;
        underrun_count_ = 0;
        next_subtitle_index_ = 0;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        stream_started_ = false;
        playback_started_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
    }

    if (task_stack_ == nullptr) {
        task_stack_ = static_cast<StackType_t*>(heap_caps_malloc(
            kNotifyTaskStackSize * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        task_tcb_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (task_stack_ != nullptr && task_tcb_ != nullptr) {
        /* Core1：与钉在 Core0 的 audio_input/AFE 分开，避免 feed 抢解码导致 underrun。 */
        task_handle_ = xTaskCreateStaticPinnedToCore(
            WorkerEntry, "notify_http", kNotifyTaskStackSize, this, kNotifyTaskPriority, task_stack_,
            task_tcb_, 1);
    } else {
        task_handle_ = nullptr;
    }
    if (task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create notification HTTP task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void NotifyPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    subtitles_.clear();
    subtitle_callback_ = nullptr;
    finished_callback_ = nullptr;
}

bool NotifyPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool NotifyPlayer::HasPlaybackStarted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && playback_started_;
}

bool NotifyPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

void NotifyPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    SubtitleCallback callback;
    std::string text;
    bool subtitle_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || playback_id != playback_id_) {
            return;
        }
        last_playback_position_ms_ = media_position_ms;
        playback_started_ = true;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= media_position_ms) {
            text = subtitles_[next_subtitle_index_].text;
            ++next_subtitle_index_;
            subtitle_changed = true;
        }
        if (!subtitle_changed || text == displayed_text_) {
            return;
        }
        displayed_text_ = text;
        callback = subtitle_callback_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    uint32_t underrun_count = 0;
    uint32_t media_position_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        } else if (stream_started_) {
            underrun_count = ++underrun_count_;
            media_position_ms = last_playback_position_ms_;
        }
    }
    /* 连续 underrun 时只打前几条和每 20 条，避免刷屏拖慢下载。 */
    if (underrun_count != 0 && (underrun_count <= 3 || (underrun_count % 20) == 0)) {
        ESP_LOGW(TAG, "Notification playback underrun #%lu at %lu ms",
                 static_cast<unsigned long>(underrun_count),
                 static_cast<unsigned long>(media_position_ms));
    }
    if (callback) {
        callback(playback_id, true);
    }
}

void NotifyPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<NotifyPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void NotifyPlayer::WorkerTask() {
    /* 每次播放重置 PSRAM 分配失败标志；播放中检测到即中止（emit/decode 路径）。 */
    s_psram_alloc_failed = false;
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    dram_epoch_ = nodelio_offline_cmd_dram_epoch();
    const bool yielded = nodelio_offline_cmd_yield_slot();
#else
    dram_epoch_ = 0;
    const bool yielded = false;
#endif
    std::string audio_url;
    std::string device_key;
    uint32_t playback_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        device_key = device_key_;
        playback_id = playback_id_;
    }

    bool success = false;
    ESP_LOGI(TAG, "notify GET %s", audio_url.c_str());
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (http) {
        http->SetTimeout(kHttpTimeoutMs);
        const std::string origin_host = UrlHost(audio_url);
        std::string url = audio_url;
        bool opened_ok = false;
        int status_code = 0;
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            http->SetHeader("Accept",
                            "audio/ogg, application/ogg, audio/mpeg, audio/wav, audio/x-wav, "
                            "audio/mp4, audio/aac, audio/x-m4a, */*");
            http->SetHeader("Accept-Encoding", "identity");
            /* 电台常夹 ICY 元数据；声明后按 icy-metaint 剥离，否则解码器当坏帧 → 跳针 */
            http->SetHeader("Icy-MetaData", "1");
            if (!device_key.empty() && UrlHost(url) == origin_host) {
                http->SetHeader("X-Device-Key", device_key);
            }
            auto opened = http->Open("GET", url);
            if (!opened) {
                ESP_LOGE(TAG, "Failed to open notification HTTP request: %s", opened.error().ToString().c_str());
                break;
            }
            opened_ok = true;
            auto status = http->GetStatusCode();
            if (!status) {
                ESP_LOGE(TAG, "Notification HTTP status failed: %s", status.error().ToString().c_str());
                opened_ok = false;
                break;
            }
            status_code = *status;
            if (!IsRedirectStatus(status_code)) {
                break;
            }
            const std::string next = ResolveLocation(url, http->GetResponseHeader("Location"));
            http->Close();
            opened_ok = false;
            if (next.empty() || next == url) {
                ESP_LOGE(TAG, "Notification HTTP redirect %d without next url", status_code);
                break;
            }
            if (hop == kMaxRedirects) {
                ESP_LOGE(TAG, "Notification HTTP redirect exceeded %d hops", kMaxRedirects);
                break;
            }
            ESP_LOGI(TAG, "HTTP %d redirect hop=%d", status_code, hop + 1);
            url = next;
        }
        if (!opened_ok || status_code < 200 || status_code >= 300) {
            ESP_LOGE(TAG, "notify HTTP failed status=%d url=%s", status_code, url.c_str());
        }
        if (opened_ok && status_code >= 200 && status_code < 300 && !IsCancelled(playback_id)) {
            const std::string ctype = http->GetResponseHeader("Content-Type");
            IcyState icy{};
            /* HttpClient 头查找已小写；顺带兼容大小写敏感实现 */
            icy.metaint = ParseIcyMetaint(http->GetResponseHeader("icy-metaint"));
            if (icy.metaint <= 0) {
                icy.metaint = ParseIcyMetaint(http->GetResponseHeader("Icy-MetaInt"));
            }
            if (icy.metaint > 0) {
                ESP_LOGI(TAG, "ICY metaint=%d", icy.metaint);
            }
            if (!PsramCanHold(kPrefetchBytes + kHttpReadBufferSize)) {
                ESP_LOGE(TAG, "prefetch skip: no %u contiguous heap",
                         (unsigned)(kPrefetchBytes + kHttpReadBufferSize));
            } else {
            PsramBytes prefetch;
            prefetch.reserve(kPrefetchBytes + kHttpReadBufferSize);
            std::array<char, kHttpReadBufferSize> buffer;
            StreamKind kind = StreamKind::kUnknown;
            bool prefetch_fail = false;
            IcyState* icy_ptr = icy.metaint > 0 ? &icy : nullptr;
            while (!IsCancelled(playback_id) && prefetch.size() < 8192 &&
                   kind == StreamKind::kUnknown) {
                const HttpAppendResult ar = HttpAppend(http.get(), prefetch, buffer, icy_ptr);
                if (ar == HttpAppendResult::kFail) {
                    prefetch_fail = true;
                    break;
                }
                if (ar == HttpAppendResult::kEos) {
                    break;
                }
                kind = SniffAudio(prefetch.data(), prefetch.size());
            }
            if (!prefetch_fail && kind == StreamKind::kUnknown && LooksM4aHint(url, ctype)) {
                kind = StreamKind::kM4a;
            }
            const size_t prefetch_target = kind == StreamKind::kMp3 || kind == StreamKind::kM4a
                                               ? Mp3CompressedPrefetchBytes()
                                               : kPrefetchBytes;
            while (!prefetch_fail && !IsCancelled(playback_id) && kind != StreamKind::kUnknown &&
                   prefetch.size() < prefetch_target) {
                const HttpAppendResult ar = HttpAppend(http.get(), prefetch, buffer, icy_ptr);
                if (ar == HttpAppendResult::kFail) {
                    prefetch_fail = true;
                    break;
                }
                if (ar == HttpAppendResult::kEos) {
                    break;
                }
            }
            {
                const char *kn = "unknown";
                if (kind == StreamKind::kOgg) {
                    kn = "ogg";
                } else if (kind == StreamKind::kMp3) {
                    kn = "mp3";
                } else if (kind == StreamKind::kM4a) {
                    kn = "m4a";
                } else if (kind == StreamKind::kWav) {
                    kn = "wav";
                }
                ESP_LOGI(TAG, "stream kind=%s ctype=%s prefetch=%u icy=%d", kn, ctype.c_str(),
                         (unsigned)prefetch.size(), icy.metaint);
            }
            if (!prefetch_fail && kind == StreamKind::kOgg) {
                success = PlayOgg(http.get(), playback_id, prefetch.data(), prefetch.size());
            } else if (!prefetch_fail && kind == StreamKind::kMp3) {
                success = PlayMp3(http.get(), playback_id, prefetch.data(), prefetch.size(), icy_ptr);
            } else if (!prefetch_fail && kind == StreamKind::kM4a) {
                success = PlayM4a(http.get(), playback_id, prefetch.data(), prefetch.size());
            } else if (!prefetch_fail && kind == StreamKind::kWav) {
                success = PlayWav(http.get(), playback_id, prefetch.data(), prefetch.size());
            } else if (!prefetch_fail && !IsCancelled(playback_id)) {
                ESP_LOGE(TAG, "Unsupported audio stream");
            }
            }
        } else if (opened_ok) {
            ESP_LOGE(TAG, "Notification HTTP request returned status %d", status_code);
        }
        http->Close();
        http.reset();
    }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    if (yielded) {
        nodelio_offline_cmd_finish_playback_dram(dram_epoch_);
        const int epoch = dram_epoch_;
        Application::GetInstance().Schedule([epoch]() {
            nodelio_offline_cmd_restore_after_media(epoch);
        });
    }
#else
    (void)yielded;
#endif

    FinishedCallback callback;
    uint32_t completed_playback_id = 0;
    bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            if (!success) {
                callback = CompleteLocked(completed_playback_id);
            } else {
                http_finished_ = true;
                if (playback_drained_) {
                    callback = CompleteLocked(completed_playback_id);
                    report_success = true;
                }
            }
        }
    }
    if (callback) {
        callback(completed_playback_id, report_success);
    }
}

bool NotifyPlayer::PlayOgg(Http* http, uint32_t playback_id, const uint8_t* first, size_t first_n) {
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    nodelio_offline_cmd_shield_up();
#endif
    auto demuxer = std::make_unique<OggDemuxer>();
    uint32_t media_position_ms = 0;
    bool packet_error = false;
    bool preskip_armed = false;
    demuxer->OnPacket([this, playback_id, &media_position_ms, &packet_error, &preskip_armed,
                       &demuxer](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                 size_t size) {
        if (packet_error || IsCancelled(playback_id)) {
            packet_error = true;
            return;
        }
        if (!preskip_armed) {
            audio_service_.ArmOpusPreSkip48k(demuxer->PreSkip48k());
            preskip_armed = true;
        }

        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->playback_id = playback_id;
        packet->media_position_ms = media_position_ms;
        packet->payload.assign(data, data + size);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !active_ || playback_id != playback_id_) {
                packet_error = true;
                return;
            }
            playback_drained_ = false;
        }

        if (!audio_service_.PushPacketToDecodeQueue(std::move(packet), true)) {
            packet_error = true;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_started_ = true;
        }
        media_position_ms += frame_duration_ms;
    });

    if (first_n > 0) {
        demuxer->Process(first, first_n);
        if (demuxer->HasError()) {
            return false;
        }
    }

    std::array<char, kHttpReadBufferSize> buffer;
    while (!packet_error && !IsCancelled(playback_id)) {
        auto size = http->Read(buffer.data(), buffer.size());
        if (!size) {
            ESP_LOGE(TAG, "Notification HTTP read failed: %s", size.error().ToString().c_str());
            return false;
        }
        if (*size == 0) {
            if (!demuxer->Finish()) {
                ESP_LOGE(TAG, "Notification Ogg stream ended before a complete audio stream");
                return false;
            }
            return true;
        }
        demuxer->Process(reinterpret_cast<const uint8_t*>(buffer.data()), *size);
        if (demuxer->HasError()) {
            return false;
        }
    }
    return false;
}

bool NotifyPlayer::PlayWav(Http* http, uint32_t playback_id, const uint8_t* first, size_t first_n) {
    /* 短 TTS：整文件进 PSRAM（SAPI wav 通常几十～几百 KB） */
    constexpr size_t kWavMax = 512 * 1024;
    const size_t want = std::min(kWavMax, first_n + 64 * 1024);
    if (!PsramCanHold(want)) {
        ESP_LOGE(TAG, "WAV skip: need ~%u contiguous", (unsigned)want);
        return false;
    }
    PsramBytes all;
    all.reserve(want);
    if (first_n > 0) {
        all.insert(all.end(), first, first + first_n);
    }
    std::array<char, kHttpReadBufferSize> buffer;
    while (!IsCancelled(playback_id) && all.size() < kWavMax) {
        auto size = http->Read(buffer.data(), buffer.size());
        if (!size) {
            ESP_LOGE(TAG, "WAV HTTP read failed: %s", size.error().ToString().c_str());
            return false;
        }
        if (*size == 0) {
            break;
        }
        const size_t need = all.size() + static_cast<size_t>(*size);
        if (need > kWavMax) {
            break;
        }
        if (need > all.capacity() && !PsramCanHold(need + kHttpReadBufferSize)) {
            ESP_LOGW(TAG, "WAV truncated at %u", (unsigned)all.size());
            break;
        }
        all.insert(all.end(), buffer.data(), buffer.data() + *size);
    }
    if (all.size() < 44 || std::memcmp(all.data(), "RIFF", 4) != 0 ||
        std::memcmp(all.data() + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "WAV header invalid size=%u", (unsigned)all.size());
        return false;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits = 0;
    size_t data_off = 0;
    size_t data_sz = 0;
    size_t pos = 12;
    while (pos + 8 <= all.size()) {
        const char* tag = reinterpret_cast<const char*>(all.data() + pos);
        uint32_t chunk_sz = static_cast<uint32_t>(all[pos + 4]) |
                            (static_cast<uint32_t>(all[pos + 5]) << 8) |
                            (static_cast<uint32_t>(all[pos + 6]) << 16) |
                            (static_cast<uint32_t>(all[pos + 7]) << 24);
        pos += 8;
        if (pos + chunk_sz > all.size()) {
            break;
        }
        if (std::memcmp(tag, "fmt ", 4) == 0 && chunk_sz >= 16) {
            audio_format = static_cast<uint16_t>(all[pos] | (all[pos + 1] << 8));
            channels = static_cast<uint16_t>(all[pos + 2] | (all[pos + 3] << 8));
            sample_rate = static_cast<uint32_t>(all[pos + 4]) | (static_cast<uint32_t>(all[pos + 5]) << 8) |
                          (static_cast<uint32_t>(all[pos + 6]) << 16) |
                          (static_cast<uint32_t>(all[pos + 7]) << 24);
            bits = static_cast<uint16_t>(all[pos + 14] | (all[pos + 15] << 8));
        } else if (std::memcmp(tag, "data", 4) == 0) {
            data_off = pos;
            data_sz = chunk_sz;
            break;
        }
        pos += chunk_sz + (chunk_sz & 1);
    }
    if (audio_format != 1 || (bits != 16 && bits != 8) || channels < 1 || channels > 2 ||
        sample_rate < 8000 || sample_rate > 48000 || data_off == 0 || data_sz == 0) {
        ESP_LOGE(TAG, "WAV unsupported fmt=%u ch=%u rate=%lu bits=%u data=%u", audio_format, channels,
                 (unsigned long)sample_rate, bits, (unsigned)data_sz);
        return false;
    }
    if (data_off + data_sz > all.size()) {
        data_sz = all.size() - data_off;
    }

    /* 短 WAV TTS：勿 shield 占满内部 DRAM（会饿死 HTTP/WiFi 缓冲，表现为连上 8790 却无声） */
    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    auto close_rs = [&]() {
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }
    };

    const uint8_t* raw = all.data() + data_off;
    const size_t frame_bytes = static_cast<size_t>(channels) * (bits / 8);
    if (frame_bytes == 0 || data_sz < frame_bytes) {
        close_rs();
        return false;
    }
    const size_t frames = data_sz / frame_bytes;
    if (!PsramCanHold(frames * sizeof(int16_t))) {
        ESP_LOGE(TAG, "WAV pcm skip frames=%u", (unsigned)frames);
        close_rs();
        return false;
    }
    PsramSamples mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* f = raw + i * frame_bytes;
        int32_t sum = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            int16_t s = 0;
            if (bits == 16) {
                s = static_cast<int16_t>(f[c * 2] | (f[c * 2 + 1] << 8));
            } else {
                s = static_cast<int16_t>((static_cast<int>(f[c]) - 128) << 8);
            }
            sum += s;
        }
        mono[i] = static_cast<int16_t>(sum / channels);
    }

    PsramSamples play = std::move(mono);
    if (static_cast<int>(sample_rate) != out_rate) {
        esp_ae_rate_cvt_cfg_t cfg = {
            .src_rate = sample_rate,
            .dest_rate = static_cast<uint32_t>(out_rate),
            .channel = 1,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK || resampler == nullptr) {
            ESP_LOGE(TAG, "WAV resampler open failed");
            close_rs();
            return false;
        }
        uint32_t target = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(resampler, play.size(), &target);
        PsramSamples resampled(target ? target : play.size() * 3);
        uint32_t actual = static_cast<uint32_t>(resampled.size());
        if (esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(play.data()),
                                    play.size(), reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                                    &actual) != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "WAV resample failed");
            close_rs();
            return false;
        }
        resampled.resize(actual);
        play = std::move(resampled);
    }

    const size_t batch = static_cast<size_t>(out_rate) * 400 / 1000; /* 400ms */
    uint32_t pos_ms = 0;
    size_t off = 0;
    bool played = false;
    while (off < play.size() && !IsCancelled(playback_id)) {
        size_t take = std::min(batch, play.size() - off);
        std::vector<int16_t> chunk(play.begin() + static_cast<std::ptrdiff_t>(off),
                                   play.begin() + static_cast<std::ptrdiff_t>(off + take));
        off += take;
        if (!audio_service_.EnqueuePlaybackPcm(std::move(chunk), playback_id, pos_ms)) {
            close_rs();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !active_ || playback_id != playback_id_) {
                close_rs();
                return false;
            }
            playback_drained_ = false;
            stream_started_ = true;
        }
        played = true;
        pos_ms += static_cast<uint32_t>(take * 1000 / static_cast<size_t>(out_rate));
    }
    close_rs();
    ESP_LOGI(TAG, "WAV play done frames_out played=%d", played ? 1 : 0);
    return played;
}

bool NotifyPlayer::PlayMp3(Http* http, uint32_t playback_id, const uint8_t* first, size_t first_n,
                           IcyState* icy) {
    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "MP3 decoder open failed");
        return false;
    }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    nodelio_offline_cmd_shield_up();
#endif

    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    const uint32_t warmup_ms = Mp3WarmupMs();
    const size_t pending_high = Mp3PendingHighWatermark();
    const size_t pending_low = Mp3PendingLowWatermark();
    const size_t warmup_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(warmup_ms) / 1000;
    const size_t batch_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(kMp3BatchMs) / 1000;
    const size_t stage_max_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(Mp3StageMaxMs()) / 1000;
    PsramBytes pending;
    if (first_n > 0) {
        pending.assign(first, first + first_n);
    }
    PsramBytes pcm_buf(1152 * 2 * sizeof(int16_t));
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_src = 0;
    uint32_t media_position_ms = 0;
    PsramSamples stage;
    stage.reserve(warmup_samples);
    /* stage/pending 改「游标消费、按需压实」：杜绝整块搬出再塞回的 O(n) PSRAM 搬运。 */
    size_t stage_off = 0;
    size_t pending_off = 0;
    auto stage_len = [&]() { return stage.size() - stage_off; };
    auto pending_len = [&]() { return pending.size() - pending_off; };
    PsramSamples mono_buf;
    PsramSamples resampled_buf;
    uint32_t stage_start_ms = 0;
    bool stage_open = false;
    bool primed = false;
    bool played = false;
    bool failed = false;

    auto close_all = [&]() {
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }
        if (http != nullptr) {
            http->Close();
        }
        if (decoder != nullptr) {
            esp_mp3_dec_close(decoder);
            decoder = nullptr;
        }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        nodelio_offline_cmd_finish_playback_dram(dram_epoch_);
#endif
    };

    auto flush_stage = [&](bool force) -> bool {
        while (stage_off < stage.size()) {
            const size_t avail = stage.size() - stage_off;
            if (!primed && !force && avail < warmup_samples) {
                return true;
            }
            if (primed && !force && avail < batch_samples) {
                return true;
            }
            /* 队列没空位时绝不动 stage：旧实现先整块拷出、入队失败再整块塞回，
             * 稳态轮询下每 20ms 白搬近 1MB PSRAM，还把高优先级突发拖得更长。 */
            if (!force && !audio_service_.PlaybackQueueHasRoom()) {
                return true;
            }
            size_t take = avail;
            if (take > batch_samples) {
                take = batch_samples;
            }
            std::vector<int16_t> chunk(stage.begin() + static_cast<std::ptrdiff_t>(stage_off),
                                       stage.begin() + static_cast<std::ptrdiff_t>(stage_off + take));
            const uint32_t pos = stage_start_ms;
            bool queue_full = false;
            if (!audio_service_.TryEnqueuePlaybackPcm(chunk, playback_id, pos, &queue_full)) {
                if (queue_full && force) {
                    if (!audio_service_.EnqueuePlaybackPcm(std::move(chunk), playback_id, pos)) {
                        return false;
                    }
                } else if (queue_full) {
                    /* 与输出任务竞态输掉：回滚游标即可，不再搬数据。 */
                    return true;
                } else {
                    return false;
                }
            }
            stage_off += take;
            if (stage_off < stage.size()) {
                stage_start_ms += static_cast<uint32_t>(take * 1000 / static_cast<size_t>(out_rate));
            } else {
                stage.clear();
                stage_off = 0;
                stage_open = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_ || !active_ || playback_id != playback_id_) {
                    return false;
                }
                playback_drained_ = false;
                stream_started_ = true;
            }
            if (!primed) {
                ESP_LOGI(TAG, "MP3 warmup play queue fill ~%u ms", (unsigned)warmup_ms);
            }
            primed = true;
            played = true;
        }
        return true;
    };

    auto emit = [&](const int16_t* samples, size_t sample_n, int channels, int sample_rate) -> bool {
        if (sample_n == 0 || channels <= 0 || sample_rate <= 0) {
            return true;
        }
        if (s_psram_alloc_failed) {
            ESP_LOGE(TAG, "PSRAM allocator degraded, abort MP3 playback");
            return false;
        }
        const int16_t* play = samples;
        size_t play_n = sample_n;
        if (channels != 1) {
            const size_t frames = sample_n / static_cast<size_t>(channels);
            if (mono_buf.size() < frames) {
                mono_buf.resize(frames);
            }
            for (size_t i = 0; i < frames; ++i) {
                int32_t sum = 0;
                for (int c = 0; c < channels; ++c) {
                    sum += samples[i * static_cast<size_t>(channels) + c];
                }
                mono_buf[i] = static_cast<int16_t>(sum / channels);
            }
            play = mono_buf.data();
            play_n = frames;
        }
        if (play_n == 0) {
            return true;
        }

        if (sample_rate != out_rate) {
            if (resampler == nullptr || resampler_src != sample_rate) {
                if (resampler != nullptr) {
                    esp_ae_rate_cvt_close(resampler);
                    resampler = nullptr;
                }
                esp_ae_rate_cvt_cfg_t cfg = {
                    .src_rate = static_cast<uint32_t>(sample_rate),
                    .dest_rate = static_cast<uint32_t>(out_rate),
                    .channel = 1,
                    .bits_per_sample = ESP_AUDIO_BIT16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK || resampler == nullptr) {
                    ESP_LOGE(TAG, "MP3 resampler open failed %d -> %d", sample_rate, out_rate);
                    return false;
                }
                resampler_src = sample_rate;
                ESP_LOGI(TAG, "MP3 %d Hz %d ch -> %d Hz", sample_rate, channels, out_rate);
            }
            uint32_t target = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, play_n, &target);
            if (resampled_buf.size() < target) {
                resampled_buf.resize(target);
            }
            uint32_t actual = target;
            if (esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(const_cast<int16_t*>(play)),
                                        play_n, reinterpret_cast<esp_ae_sample_t>(resampled_buf.data()),
                                        &actual) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "MP3 resample failed");
                return false;
            }
            play = resampled_buf.data();
            play_n = actual;
        } else if (!played && !primed) {
            ESP_LOGI(TAG, "MP3 %d Hz %d ch", sample_rate, channels);
        }

        /* 已消费前缀攒够一批（或过半）就压实一次：把 O(剩余) 搬运摊销成每批一次。 */
        if (stage_off > 0 && (stage_off >= batch_samples || stage_off * 2 >= stage.size())) {
            stage.erase(stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(stage_off));
            stage_off = 0;
        }
        if (!stage_open) {
            stage_start_ms = media_position_ms;
            stage_open = true;
        }
        stage.insert(stage.end(), play, play + play_n);
        media_position_ms += static_cast<uint32_t>(sample_n / static_cast<size_t>(channels) * 1000 /
                                                    static_cast<size_t>(sample_rate));
        return flush_stage(false);
    };

    DecodeTimeslice timeslice;
    auto decode_pending = [&](bool yield_for_net) -> bool {
        /* 前缀压实一次摊销：旧版每帧 erase 头部，128KB 的 pending 每秒白搬数 MB PSRAM。 */
        if (pending_off > 0) {
            if (pending_off >= pending.size()) {
                pending.clear();
                pending_off = 0;
            } else if (pending_off >= 16384) {
                pending.erase(pending.begin(),
                              pending.begin() + static_cast<std::ptrdiff_t>(pending_off));
                pending_off = 0;
            }
        }
        while (pending.size() - pending_off >= 4 && !IsCancelled(playback_id)) {
            timeslice.YieldIfNeeded();
            /* stage 已囤够：停解码，让主循环去补 HTTP。 */
            if (primed && stage_len() >= stage_max_samples) {
                return true;
            }
            /* 压缩垫片见底就回去 Read。64kbps 下 96KB≈12s，解光再拉流会在预取边界跳针。
             * EOS 收尾 yield_for_net=false，必须把剩余帧解完。 */
            if (yield_for_net && primed && pending_len() < pending_low &&
                (stage_len() >= batch_samples || !audio_service_.PlaybackQueueHasRoom())) {
                return true;
            }
            if (s_psram_alloc_failed) {
                ESP_LOGE(TAG, "PSRAM allocator degraded, abort MP3 playback");
                return false;
            }
            esp_audio_dec_in_raw_t raw = {
                .buffer = pending.data() + pending_off,
                .len = static_cast<uint32_t>(pending.size() - pending_off),
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };
            esp_audio_dec_out_frame_t frame = {
                .buffer = pcm_buf.data(),
                .len = static_cast<uint32_t>(pcm_buf.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            esp_audio_dec_info_t info = {};
            const esp_audio_err_t ret = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                const uint32_t need = frame.needed_size > 0 ? frame.needed_size : pcm_buf.size() * 2;
                pcm_buf.resize(need);
                continue;
            }
            if (ret == ESP_AUDIO_ERR_DATA_LACK) {
                const size_t avail = pending.size() - pending_off;
                pending_off += raw.consumed < avail ? raw.consumed : avail;
                return true;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                const size_t avail = pending.size() - pending_off;
                /* 半帧常被报 NOT_SUPPORT/FAIL；缓冲不够一帧半时等网，勿跳同步丢数据 */
                if (yield_for_net && avail < kMp3ResyncMinAvail) {
                    return true;
                }
                /* 坏帧/元数据残留：跳到下一可靠同步字 */
                const size_t from =
                    pending_off + (raw.consumed > 0 ? static_cast<size_t>(raw.consumed) : 1u);
                pending_off = FindNextMp3Sync(pending.data(), pending.size(), from);
                timeslice.YieldIfNeeded();
                continue;
            }
            if (raw.consumed == 0) {
                return false;
            }
            {
                const size_t avail = pending.size() - pending_off;
                pending_off += raw.consumed < avail ? raw.consumed : avail;
            }
            if (frame.decoded_size < sizeof(int16_t) || info.channel == 0) {
                continue;
            }
            const size_t sample_n = frame.decoded_size / sizeof(int16_t);
            if (!emit(reinterpret_cast<const int16_t*>(frame.buffer), sample_n, info.channel,
                      static_cast<int>(info.sample_rate))) {
                return false;
            }
            /* 队列刚满：先回主循环补网，勿继续把 pending 解光。 */
            if (primed && !audio_service_.PlaybackQueueHasRoom() && stage_len() >= batch_samples) {
                return true;
            }
        }
        return !IsCancelled(playback_id);
    };

    std::array<char, kHttpReadBufferSize> buffer;
    while (!IsCancelled(playback_id)) {
        /* 铁律：先喂喇叭。旧顺序「pending 不足就阻塞 Read」会在 stage 仍有数秒 PCM
         * 时把播放队列抽空 → 听感约 8–10s 处跳 1–2s（本机 16k→24k MP3 点歌复现）。 */
        if (!flush_stage(false)) {
            failed = true;
            break;
        }
        if (!decode_pending(true)) {
            failed = true;
            break;
        }
        if (pending_len() > 64 * 1024 && !played) {
            ESP_LOGE(TAG, "MP3 stream has no decodable frame");
            failed = true;
            break;
        }
        if (!flush_stage(false)) {
            failed = true;
            break;
        }

        const bool need_net = pending_len() < pending_high;
        const bool queue_full = !audio_service_.PlaybackQueueHasRoom();
        const bool play_cushion =
            !primed || queue_full || stage_len() >= batch_samples;
        const bool must_net = pending_len() < pending_low;
        if (need_net && (play_cushion || must_net)) {
            /* 每读一块就解码/喂播放队列；直播不能等 32 块凑满才处理。 */
            const HttpAppendResult ar = HttpAppend(http, pending, buffer, icy);
            if (ar != HttpAppendResult::kOk) {
                failed = ar == HttpAppendResult::kFail;
                break;
            }
        } else if (pending_len() >= pending_high && stage_len() >= batch_samples &&
                   !audio_service_.PlaybackQueueHasRoom()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (need_net && !play_cushion) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (!failed && !IsCancelled(playback_id)) {
        /* EOS 收尾必须循环到「pending 解光 + stage 清空」：旧版只跑一轮，
         * decode_pending 撞到 stage 上限即返回，低码率（~48kbps）曲子结尾
         * 最多 ~10s 压缩数据未解码就被丢弃，歌被提前掐断。 */
        while (true) {
            const size_t off_before = pending_off;
            const size_t stage_before = stage_len();
            if (!decode_pending(false)) {
                failed = true;
                break;
            }
            if (!flush_stage(true)) {
                failed = true;
                break;
            }
            if (pending_len() < 4) {
                break;
            }
            /* 无进展=本轮 pending 没消费且 stage 没被清走；stage 被清空说明有进展，继续解。 */
            if (pending_off == off_before && stage_len() == stage_before) {
                break; /* 尾部解不动的垃圾，勿空转 */
            }
        }
    }

    close_all();
    return played && !failed && !IsCancelled(playback_id);
}

bool NotifyPlayer::PlayM4a(Http* http, uint32_t playback_id, const uint8_t* first, size_t first_n) {
    EnsureM4aDecoderRegistered();
    esp_m4a_dec_cfg_t m4a_cfg = {
        .track_idx = 0,
        .aac_plus_enable = true,
    };
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,
        .dec_cfg = &m4a_cfg,
        .cfg_size = sizeof(m4a_cfg),
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "M4A decoder open failed");
        return false;
    }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
    nodelio_offline_cmd_shield_up();
#endif

    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    const uint32_t warmup_ms = Mp3WarmupMs();
    const size_t pending_high = Mp3PendingHighWatermark();
    const size_t pending_low = Mp3PendingLowWatermark();
    const size_t warmup_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(warmup_ms) / 1000;
    const size_t batch_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(kMp3BatchMs) / 1000;
    const size_t stage_max_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(Mp3StageMaxMs()) / 1000;
    PsramBytes pending;
    if (first_n > 0) {
        pending.assign(first, first + first_n);
    }
    PsramBytes pcm_buf(4096);
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_src = 0;
    uint32_t media_position_ms = 0;
    PsramSamples stage;
    stage.reserve(warmup_samples);
    /* 游标消费 + 按需压实，理由同 PlayMp3；mono/resampled 复用缓冲防每帧堆抖动。 */
    size_t stage_off = 0;
    auto stage_len = [&]() { return stage.size() - stage_off; };
    PsramSamples mono_buf;
    PsramSamples resampled_buf;
    uint32_t stage_start_ms = 0;
    bool stage_open = false;
    bool primed = false;
    bool played = false;
    bool failed = false;
    bool logged_info = false;

    auto close_all = [&]() {
        if (resampler != nullptr) {
            esp_ae_rate_cvt_close(resampler);
            resampler = nullptr;
        }
        if (http != nullptr) {
            http->Close();
        }
        if (decoder != nullptr) {
            esp_audio_simple_dec_close(decoder);
            decoder = nullptr;
        }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        nodelio_offline_cmd_finish_playback_dram(dram_epoch_);
#endif
    };

    auto flush_stage = [&](bool force) -> bool {
        while (stage_off < stage.size()) {
            const size_t avail = stage.size() - stage_off;
            if (!primed && !force && avail < warmup_samples) {
                return true;
            }
            if (primed && !force && avail < batch_samples) {
                return true;
            }
            /* 队列没空位时绝不动 stage：旧实现先整块拷出、入队失败再整块塞回，
             * 稳态轮询下每 20ms 白搬近 1MB PSRAM，还把高优先级突发拖得更长。 */
            if (!force && !audio_service_.PlaybackQueueHasRoom()) {
                return true;
            }
            size_t take = avail;
            if (take > batch_samples) {
                take = batch_samples;
            }
            std::vector<int16_t> chunk(stage.begin() + static_cast<std::ptrdiff_t>(stage_off),
                                       stage.begin() + static_cast<std::ptrdiff_t>(stage_off + take));
            const uint32_t pos = stage_start_ms;
            bool queue_full = false;
            if (!audio_service_.TryEnqueuePlaybackPcm(chunk, playback_id, pos, &queue_full)) {
                if (queue_full && force) {
                    if (!audio_service_.EnqueuePlaybackPcm(std::move(chunk), playback_id, pos)) {
                        return false;
                    }
                } else if (queue_full) {
                    /* 与输出任务竞态输掉：回滚游标即可，不再搬数据。 */
                    return true;
                } else {
                    return false;
                }
            }
            stage_off += take;
            if (stage_off < stage.size()) {
                stage_start_ms += static_cast<uint32_t>(take * 1000 / static_cast<size_t>(out_rate));
            } else {
                stage.clear();
                stage_off = 0;
                stage_open = false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_ || !active_ || playback_id != playback_id_) {
                    return false;
                }
                playback_drained_ = false;
                stream_started_ = true;
            }
            if (!primed) {
                ESP_LOGI(TAG, "M4A warmup play queue fill ~%u ms", (unsigned)warmup_ms);
            }
            primed = true;
            played = true;
        }
        return true;
    };

    auto emit = [&](const int16_t* samples, size_t sample_n, int channels, int sample_rate) -> bool {
        if (sample_n == 0 || channels <= 0 || sample_rate <= 0) {
            return true;
        }
        if (s_psram_alloc_failed) {
            ESP_LOGE(TAG, "PSRAM allocator degraded, abort M4A playback");
            return false;
        }
        const int16_t* play = samples;
        size_t play_n = sample_n;
        if (channels != 1) {
            const size_t frames = sample_n / static_cast<size_t>(channels);
            if (mono_buf.size() < frames) {
                mono_buf.resize(frames);
            }
            for (size_t i = 0; i < frames; ++i) {
                int32_t sum = 0;
                for (int c = 0; c < channels; ++c) {
                    sum += samples[i * static_cast<size_t>(channels) + c];
                }
                mono_buf[i] = static_cast<int16_t>(sum / channels);
            }
            play = mono_buf.data();
            play_n = frames;
        }
        if (play_n == 0) {
            return true;
        }
        if (sample_rate != out_rate) {
            if (resampler == nullptr || resampler_src != sample_rate) {
                if (resampler != nullptr) {
                    esp_ae_rate_cvt_close(resampler);
                    resampler = nullptr;
                }
                esp_ae_rate_cvt_cfg_t cfg = {
                    .src_rate = static_cast<uint32_t>(sample_rate),
                    .dest_rate = static_cast<uint32_t>(out_rate),
                    .channel = 1,
                    .bits_per_sample = ESP_AUDIO_BIT16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&cfg, &resampler) != ESP_AE_ERR_OK || resampler == nullptr) {
                    ESP_LOGE(TAG, "M4A resampler open failed %d -> %d", sample_rate, out_rate);
                    return false;
                }
                resampler_src = sample_rate;
                ESP_LOGI(TAG, "M4A %d Hz %d ch -> %d Hz", sample_rate, channels, out_rate);
            }
            uint32_t target = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, play_n, &target);
            if (resampled_buf.size() < target) {
                resampled_buf.resize(target);
            }
            uint32_t actual = target;
            if (esp_ae_rate_cvt_process(resampler,
                                        reinterpret_cast<esp_ae_sample_t>(const_cast<int16_t*>(play)),
                                        play_n, reinterpret_cast<esp_ae_sample_t>(resampled_buf.data()),
                                        &actual) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "M4A resample failed");
                return false;
            }
            play = resampled_buf.data();
            play_n = actual;
        } else if (!logged_info) {
            ESP_LOGI(TAG, "M4A %d Hz %d ch", sample_rate, channels);
            logged_info = true;
        }

        /* 已消费前缀攒够一批（或过半）就压实一次：把 O(剩余) 搬运摊销成每批一次。 */
        if (stage_off > 0 && (stage_off >= batch_samples || stage_off * 2 >= stage.size())) {
            stage.erase(stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(stage_off));
            stage_off = 0;
        }
        if (!stage_open) {
            stage_start_ms = media_position_ms;
            stage_open = true;
        }
        stage.insert(stage.end(), play, play + play_n);
        media_position_ms += static_cast<uint32_t>(sample_n / static_cast<size_t>(channels) * 1000 /
                                                    static_cast<size_t>(sample_rate));
        return flush_stage(false);
    };

    DecodeTimeslice timeslice;
    auto decode_chunk = [&](bool eos) -> bool {
        if (pending.empty() && !eos) {
            return true;
        }
        /* stage 囤满先回主循环；EOS 收尾同样分批，由外层循环驱动 flush 清空。 */
        if (primed && stage_len() >= stage_max_samples) {
            return true;
        }
        if (!eos && primed && pending.size() < pending_low &&
            (stage_len() >= batch_samples || !audio_service_.PlaybackQueueHasRoom())) {
            return true;
        }
        esp_audio_simple_dec_raw_t raw = {
            .buffer = pending.data(),
            .len = static_cast<uint32_t>(pending.size()),
            .eos = eos,
            .consumed = 0,
            .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
        };
        while ((raw.len > 0 || eos) && !IsCancelled(playback_id)) {
            timeslice.YieldIfNeeded();
            if (primed && stage_len() >= stage_max_samples) {
                break;
            }
            if (!eos && primed && raw.len < pending_low &&
                (stage_len() >= batch_samples || !audio_service_.PlaybackQueueHasRoom())) {
                break;
            }
            esp_audio_simple_dec_out_t frame = {
                .buffer = pcm_buf.data(),
                .len = static_cast<uint32_t>(pcm_buf.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            const esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                const uint32_t need = frame.needed_size > 0 ? frame.needed_size : pcm_buf.size() * 2;
                pcm_buf.resize(need);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "M4A decode failed ret=%d", static_cast<int>(ret));
                return false;
            }
            if (frame.decoded_size >= sizeof(int16_t)) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK || info.channel == 0 ||
                    info.sample_rate == 0) {
                    ESP_LOGE(TAG, "M4A info not ready");
                    return false;
                }
                const size_t sample_n = frame.decoded_size / sizeof(int16_t);
                if (!emit(reinterpret_cast<const int16_t*>(frame.buffer), sample_n, info.channel,
                          static_cast<int>(info.sample_rate))) {
                    return false;
                }
            }
            if (raw.consumed == 0) {
                break;
            }
            if (raw.consumed > raw.len) {
                return false;
            }
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
            raw.consumed = 0;
            if (eos && raw.len == 0 && frame.decoded_size == 0) {
                break;
            }
            if (primed && !audio_service_.PlaybackQueueHasRoom() &&
                stage_len() >= batch_samples) {
                break;
            }
        }
        if (raw.len <= pending.size()) {
            const size_t done = pending.size() - raw.len;
            if (done > 0) {
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(done));
            }
        } else {
            pending.clear();
        }
        return !IsCancelled(playback_id);
    };

    std::array<char, kHttpReadBufferSize> buffer;
    while (!IsCancelled(playback_id)) {
        /* 同 PlayMp3：先 flush/decode，有播放垫片再阻塞 Read。 */
        if (!flush_stage(false)) {
            failed = true;
            break;
        }
        if (!decode_chunk(false)) {
            failed = true;
            break;
        }
        if (pending.size() >= pending_high && !played) {
            ESP_LOGE(TAG, "M4A stream has no decodable frame (moov after mdat?)");
            failed = true;
            break;
        }
        if (!flush_stage(false)) {
            failed = true;
            break;
        }

        const bool need_net = pending.size() < pending_high;
        const bool queue_full = !audio_service_.PlaybackQueueHasRoom();
        const bool play_cushion =
            !primed || queue_full || stage_len() >= batch_samples;
        const bool must_net = pending.size() < pending_low;
        if (need_net && (play_cushion || must_net)) {
            /* 与 MP3 一致，慢速播客也须逐块解码，不能批量阻塞读。 */
            const HttpAppendResult ar = HttpAppend(http, pending, buffer);
            if (ar != HttpAppendResult::kOk) {
                failed = ar == HttpAppendResult::kFail;
                break;
            }
        } else if (pending.size() >= pending_high && stage_len() >= batch_samples &&
                   !audio_service_.PlaybackQueueHasRoom()) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else if (need_net && !play_cushion) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (!failed && !IsCancelled(playback_id)) {
        /* EOS 循环收尾（同 PlayMp3）：解光 pending、清空 stage，防低码率曲子被截尾。 */
        while (true) {
            const size_t size_before = pending.size();
            const size_t stage_before = stage_len();
            if (!decode_chunk(true)) {
                failed = true;
                break;
            }
            if (!flush_stage(true)) {
                failed = true;
                break;
            }
            if (pending.empty()) {
                break;
            }
            /* 无进展=本轮 pending 没消费且 stage 没被清走；stage 被清空说明有进展，继续解。 */
            if (pending.size() == size_before && stage_len() == stage_before) {
                break; /* 尾部解不动的垃圾，勿空转 */
            }
        }
    }

    close_all();
    return played && !failed && !IsCancelled(playback_id);
}
