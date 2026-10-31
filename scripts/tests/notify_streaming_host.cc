#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
using PsramBytes = std::vector<uint8_t>;
using PsramSamples = std::vector<int16_t>;
bool s_psram_alloc_failed = false;
int64_t clock_us = 0;
int yields = 0, read_run = 0, max_read_run = 0;
bool stalled = false;
int64_t esp_timer_get_time() { return clock_us; }
void vTaskDelay(int ticks) {
    clock_us += ticks * 1000;
    ++yields;
}
int pdMS_TO_TICKS(int ms) { return ms; }
constexpr size_t kHttpReadBufferSize = 4096;
// INSERT_CONSTANTS

void nodelio_offline_cmd_shield_up() {}
void nodelio_offline_cmd_finish_playback_dram(int) {}
void EnsureM4aDecoderRegistered() {}
struct Codec {
    int output_sample_rate() { return 24000; }
};
struct Board {
    static Board& GetInstance() {
        static Board board;
        return board;
    }
    Codec* GetAudioCodec() { return &codec; }
    Codec codec;
};

enum {
    ESP_AUDIO_ERR_OK,
    ESP_AUDIO_ERR_BUFF_NOT_ENOUGH,
    ESP_AUDIO_ERR_DATA_LACK,
    ESP_AUDIO_DEC_RECOVERY_NONE,
    ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    ESP_AUDIO_SIMPLE_DEC_TYPE_M4A,
    ESP_AUDIO_BIT16,
    ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
    ESP_AE_ERR_OK
};
using esp_audio_err_t = int;
struct esp_audio_dec_in_raw_t {
    uint8_t* buffer;
    uint32_t len, consumed;
    int frame_recover;
};
struct esp_audio_simple_dec_raw_t {
    uint8_t* buffer;
    uint32_t len;
    bool eos;
    uint32_t consumed;
    int frame_recover;
};
struct esp_audio_dec_out_frame_t {
    uint8_t* buffer;
    uint32_t len, needed_size, decoded_size;
};
using esp_audio_simple_dec_out_t = esp_audio_dec_out_frame_t;
struct esp_audio_dec_info_t {
    uint32_t channel, sample_rate;
};
using esp_audio_simple_dec_info_t = esp_audio_dec_info_t;
using esp_audio_simple_dec_handle_t = void*;
struct esp_m4a_dec_cfg_t {
    int track_idx;
    bool aac_plus_enable;
};
struct esp_audio_simple_dec_cfg_t {
    int dec_type;
    void* dec_cfg;
    size_t cfg_size;
    bool use_frame_dec;
};
int esp_mp3_dec_open(void*, int, void** decoder) {
    *decoder = &stalled;
    return ESP_AUDIO_ERR_OK;
}
int esp_audio_simple_dec_open(void*, void** decoder) {
    return esp_mp3_dec_open(nullptr, 0, decoder);
}
void esp_mp3_dec_close(void*) {}
void esp_audio_simple_dec_close(void*) {}

// Each 256 compressed bytes produces 32 ms PCM (64 kbit/s, 24 kHz mono).
template <typename Raw>
int Decode(Raw* raw, esp_audio_dec_out_frame_t* frame) {
    read_run = 0;
    clock_us += 500;
    raw->consumed = 0;
    frame->decoded_size = 0;
    if (stalled || raw->len < 256)
        return ESP_AUDIO_ERR_DATA_LACK;
    if (raw->buffer[0] == 0xee)
        return -1;  // Force bytewise MP3 resynchronization.
    raw->consumed = 256;
    frame->decoded_size = 768 * sizeof(int16_t);
    assert(frame->len >= frame->decoded_size);
    std::fill_n(reinterpret_cast<int16_t*>(frame->buffer), 768, 123);
    return ESP_AUDIO_ERR_OK;
}
int esp_mp3_dec_decode(void*, esp_audio_dec_in_raw_t* raw, esp_audio_dec_out_frame_t* frame,
                       esp_audio_dec_info_t* info) {
    *info = {1, 24000};
    return Decode(raw, frame);
}
int esp_audio_simple_dec_process(void*, esp_audio_simple_dec_raw_t* raw,
                                 esp_audio_simple_dec_out_t* frame) {
    const int result = Decode(raw, frame);
    return result == ESP_AUDIO_ERR_DATA_LACK ? ESP_AUDIO_ERR_OK : result;
}
int esp_audio_simple_dec_get_info(void*, esp_audio_simple_dec_info_t* info) {
    *info = {1, 24000};
    return ESP_AUDIO_ERR_OK;
}

using esp_ae_rate_cvt_handle_t = void*;
using esp_ae_sample_t = void*;
struct esp_ae_rate_cvt_cfg_t {
    uint32_t src_rate, dest_rate;
    int channel, bits_per_sample, complexity, perf_type;
};
int esp_ae_rate_cvt_open(void*, void**) {
    assert(false);
    return -1;
}
void esp_ae_rate_cvt_close(void*) {}
void esp_ae_rate_cvt_get_max_out_sample_num(void*, size_t, uint32_t*) { assert(false); }
int esp_ae_rate_cvt_process(void*, void*, size_t, void*, uint32_t*) {
    assert(false);
    return -1;
}

struct AudioService {
    size_t samples = 0;
    int64_t first_pcm_us = -1;
    bool backpressure = false;
    std::deque<int64_t> queue;
    bool PlaybackQueueHasRoom() {
        while (!queue.empty() && queue.front() <= clock_us)
            queue.pop_front();
        return !backpressure || queue.size() < 2;
    }
    bool TryEnqueuePlaybackPcm(std::vector<int16_t>& pcm, uint32_t, uint32_t, bool* full) {
        if (!PlaybackQueueHasRoom()) {
            if (full)
                *full = true;
            return false;
        }
        if (first_pcm_us < 0)
            first_pcm_us = clock_us;
        samples += pcm.size();
        if (backpressure) {
            const int64_t start = queue.empty() ? clock_us : queue.back();
            queue.push_back(start + pcm.size() * 1000000 / 24000);
        }
        return true;
    }
    bool EnqueuePlaybackPcm(std::vector<int16_t>&& pcm, uint32_t id, uint32_t position) {
        while (!PlaybackQueueHasRoom())
            vTaskDelay(20);
        return TryEnqueuePlaybackPcm(pcm, id, position, nullptr);
    }
};
struct Http {
    size_t remaining = 0;
    int reads = 0, fail_at = -1;
    int byte_us = 125;
    std::function<void()> on_read;
    void Close() {}
};
enum class HttpAppendResult { kOk, kEos, kFail };
struct NotifyIcyState {
    int metaint = 0;
    size_t audio_since_meta = 0;
    size_t meta_left = 0;
    bool await_len = false;
};
HttpAppendResult HttpAppend(Http* http, PsramBytes& pending,
                            std::array<char, kHttpReadBufferSize>&,
                            NotifyIcyState* = nullptr) {
    max_read_run = std::max(max_read_run, ++read_run);
    if (http->on_read)
        http->on_read();
    if (http->reads++ == http->fail_at)
        return HttpAppendResult::kFail;
    const size_t bytes = std::min(http->remaining, kHttpReadBufferSize);
    http->remaining -= bytes;
    clock_us += bytes * http->byte_us;  // Rate-limited live HTTP or a fast file download.
    pending.insert(pending.end(), bytes, 0);
    return bytes ? HttpAppendResult::kOk : HttpAppendResult::kEos;
}
size_t FindNextMp3Sync(const uint8_t*, size_t n, size_t from) {
    return from < n ? from : n;
}
struct NotifySubtitle {
    uint32_t start_ms;
    std::string text;
};
struct NotifyPlayer {
    using SubtitleCallback = std::function<void(uint32_t, const std::string&)>;
    using IcyState = NotifyIcyState;
    AudioService audio_service_;
    mutable std::mutex mutex_;
    bool active_ = true, cancelled_ = false, playback_drained_ = false;
    bool stream_started_ = false, playback_started_ = false;
    uint32_t playback_id_ = 1, last_playback_position_ms_ = 0;
    size_t next_subtitle_index_ = 0;
    int dram_epoch_ = 0;
    std::vector<NotifySubtitle> subtitles_;
    std::string displayed_text_;
    SubtitleCallback subtitle_callback_, finished_callback_;
    bool PlayMp3(Http*, uint32_t, const uint8_t*, size_t, IcyState* = nullptr);
    bool PlayM4a(Http*, uint32_t, const uint8_t*, size_t);
    bool HasPlaybackStarted() const;
    bool IsCancelled(uint32_t) const;
    void OnPlaybackProgress(uint32_t, uint32_t);
    void Stop();
};
// INSERT_METHODS

void Reset(size_t prefetched) {
    clock_us = prefetched * 125;
    yields = read_run = max_read_run = 0;
    stalled = false;
}
int main() {
    using Play = bool (NotifyPlayer::*)(Http*, uint32_t, const uint8_t*, size_t);
    for (Play play : {&NotifyPlayer::PlayMp3, &NotifyPlayer::PlayM4a}) {
        for (const size_t frames : {2u, 625u}) {
            const size_t bytes = frames * 256;
            std::vector<uint8_t> first(std::min(bytes, kCompressedPrefetchBytes), 0);
            Reset(first.size());
            Http http{.remaining = bytes - first.size()};
            NotifyPlayer player;
            assert((player.*play)(&http, 1, first.data(), first.size()));
            assert(player.audio_service_.samples == frames * 768);  // Includes short EOF tail.
            assert(player.audio_service_.first_pcm_us < 5000000);
            assert(max_read_run <= 1);  // No multi-block live download before decoding.
            if (frames > 2)
                assert(yields > 0);
        }
        Reset(0);
        NotifyPlayer pressure;
        pressure.audio_service_.backpressure = true;
        Http fast_http{.remaining = 2000 * 256, .byte_us = 0};
        assert((pressure.*play)(&fast_http, 1, nullptr, 0));
        assert(pressure.audio_service_.samples == 2000 * 768);

        Reset(0);
        NotifyPlayer cancelled;
        Http cancel_http{.remaining = 160000, .on_read = [&]() { cancelled.Stop(); }};
        assert(!(cancelled.*play)(&cancel_http, 1, nullptr, 0));
        assert(cancel_http.reads == 1 && cancelled.audio_service_.samples == 0);

        Reset(0);
        NotifyPlayer failed;
        Http fail_http{.remaining = 160000, .fail_at = 2};
        assert(!(failed.*play)(&fail_http, 1, nullptr, 0));
    }
    // Container never supplies a frame: must fail at the bounded buffer, not spin forever.
    Reset(0);
    stalled = true;
    NotifyPlayer blocked;
    Http blocked_http{.remaining = 1024 * 1024};
    assert(!blocked.PlayM4a(&blocked_http, 1, nullptr, 0));
    assert(blocked_http.reads == kPendingHighWatermark / kHttpReadBufferSize);

    // Corrupt MP3 input must also yield while searching for sync.
    Reset(0);
    std::vector<uint8_t> bad(4096, 0xee);
    NotifyPlayer corrupt;
    Http empty;
    assert(!corrupt.PlayMp3(&empty, 1, bad.data(), bad.size()));
    assert(yields > 0);

    NotifyPlayer readiness;
    assert(!readiness.HasPlaybackStarted());
    readiness.OnPlaybackProgress(99, 0);  // Old track cannot arm the new one.
    assert(!readiness.HasPlaybackStarted());
    readiness.OnPlaybackProgress(1, 0);  // First PCM starts at zero, with no subtitles.
    assert(readiness.HasPlaybackStarted());
    readiness.Stop();
    assert(!readiness.HasPlaybackStarted());
}
