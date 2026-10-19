#include "notify_player.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "board.h"
#include "esp_ae_rate_cvt.h"
#include "esp_mp3_dec.h"
#include "http.h"
#include "ogg_demuxer.h"

namespace {
constexpr int kHttpTimeoutMs = 15000;
/* 1KB 读 + 播放队列仅 2 格时，WiFi 稍卡就会 underrun；加大读块并提高任务优先级。 */
constexpr size_t kHttpReadBufferSize = 4096;
constexpr uint32_t kNotifyTaskStackSize = 12288;
constexpr UBaseType_t kNotifyTaskPriority = 5;
/* MP3 经 EnqueuePlaybackPcm，队列深只有 2。先攒起播、再按片送，避免几十毫秒就见底。 */
constexpr uint32_t kMp3PrimeMs = 800;
constexpr uint32_t kMp3BatchMs = 240;
const char* TAG = "NotifyPlayer";
constexpr int kMaxRedirects = 4;

enum class StreamKind { kUnknown, kOgg, kMp3 };

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

StreamKind SniffAudio(const uint8_t* p, size_t n) {
    if (n >= 4 && std::memcmp(p, "OggS", 4) == 0) {
        return StreamKind::kOgg;
    }
    if (n >= 3 && std::memcmp(p, "ID3", 3) == 0) {
        return StreamKind::kMp3;
    }
    const size_t limit = n > 512 ? 512 : n;
    for (size_t i = 0; i + 4 <= limit; ++i) {
        if (IsMp3Frame(p + i, n - i)) {
            return StreamKind::kMp3;
        }
    }
    return StreamKind::kUnknown;
}

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
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
        task_handle_ = xTaskCreateStatic(WorkerEntry, "notify_http", kNotifyTaskStackSize, this,
                                          kNotifyTaskPriority, task_stack_, task_tcb_);
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
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (http) {
        http->SetTimeout(kHttpTimeoutMs);
        const std::string origin_host = UrlHost(audio_url);
        std::string url = audio_url;
        bool opened_ok = false;
        int status_code = 0;
        for (int hop = 0; hop <= kMaxRedirects; ++hop) {
            http->SetHeader("Accept", "audio/ogg, application/ogg, audio/mpeg, */*");
            http->SetHeader("Accept-Encoding", "identity");
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
        if (opened_ok && status_code >= 200 && status_code < 300 && !IsCancelled(playback_id)) {
            std::vector<uint8_t> prefetch;
            prefetch.reserve(4096);
            std::array<char, kHttpReadBufferSize> buffer;
            StreamKind kind = StreamKind::kUnknown;
            while (!IsCancelled(playback_id) && prefetch.size() < 4096 &&
                   kind == StreamKind::kUnknown) {
                auto size = http->Read(buffer.data(), buffer.size());
                if (!size) {
                    ESP_LOGE(TAG, "Notification HTTP read failed: %s",
                             size.error().ToString().c_str());
                    break;
                }
                if (*size == 0) {
                    break;
                }
                prefetch.insert(prefetch.end(), buffer.data(), buffer.data() + *size);
                kind = SniffAudio(prefetch.data(), prefetch.size());
            }
            if (kind == StreamKind::kOgg) {
                success = PlayOgg(http.get(), playback_id, prefetch.data(), prefetch.size());
            } else if (kind == StreamKind::kMp3) {
                success = PlayMp3(http.get(), playback_id, prefetch.data(), prefetch.size());
            } else if (!IsCancelled(playback_id)) {
                ESP_LOGE(TAG, "Unsupported audio stream");
            }
        } else if (opened_ok) {
            ESP_LOGE(TAG, "Notification HTTP request returned status %d", status_code);
        }
        http->Close();
        http.reset();
    }

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
    auto demuxer = std::make_unique<OggDemuxer>();
    uint32_t media_position_ms = 0;
    bool packet_error = false;
    demuxer->OnPacket([this, playback_id, &media_position_ms, &packet_error](
                          const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
        if (packet_error || IsCancelled(playback_id)) {
            packet_error = true;
            return;
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

bool NotifyPlayer::PlayMp3(Http* http, uint32_t playback_id, const uint8_t* first, size_t first_n) {
    void* decoder = nullptr;
    if (esp_mp3_dec_open(nullptr, 0, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "MP3 decoder open failed");
        return false;
    }

    const int out_rate = Board::GetInstance().GetAudioCodec()->output_sample_rate();
    const size_t prime_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(kMp3PrimeMs) / 1000;
    const size_t batch_samples =
        static_cast<size_t>(out_rate) * static_cast<size_t>(kMp3BatchMs) / 1000;
    std::vector<uint8_t> pending;
    if (first_n > 0) {
        pending.assign(first, first + first_n);
    }
    std::vector<uint8_t> pcm_buf(1152 * 2 * sizeof(int16_t));
    esp_ae_rate_cvt_handle_t resampler = nullptr;
    int resampler_src = 0;
    uint32_t media_position_ms = 0;
    std::vector<int16_t> stage;
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
        esp_mp3_dec_close(decoder);
        decoder = nullptr;
    };

    auto flush_stage = [&](bool force) -> bool {
        while (!stage.empty()) {
            const size_t need = primed ? batch_samples : prime_samples;
            if (!force && stage.size() < need) {
                return true;
            }
            size_t take = stage.size();
            if (!force && take > batch_samples && primed) {
                take = batch_samples;
            } else if (!primed && !force && take > prime_samples) {
                take = prime_samples;
            }
            std::vector<int16_t> chunk(stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(take));
            stage.erase(stage.begin(), stage.begin() + static_cast<std::ptrdiff_t>(take));
            const uint32_t pos = stage_start_ms;
            if (!stage.empty()) {
                stage_start_ms += static_cast<uint32_t>(take * 1000 / static_cast<size_t>(out_rate));
            } else {
                stage_open = false;
            }
            if (!audio_service_.EnqueuePlaybackPcm(std::move(chunk), playback_id, pos)) {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cancelled_ || !active_ || playback_id != playback_id_) {
                    return false;
                }
                playback_drained_ = false;
                stream_started_ = true;
            }
            primed = true;
            played = true;
            if (force && stage.empty()) {
                break;
            }
            if (!force && stage.size() < (primed ? batch_samples : prime_samples)) {
                break;
            }
        }
        return true;
    };

    auto emit = [&](const int16_t* samples, size_t sample_n, int channels, int sample_rate) -> bool {
        if (sample_n == 0 || channels <= 0 || sample_rate <= 0) {
            return true;
        }
        std::vector<int16_t> mono;
        if (channels == 1) {
            mono.assign(samples, samples + sample_n);
        } else {
            const size_t frames = sample_n / static_cast<size_t>(channels);
            mono.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                int32_t sum = 0;
                for (int c = 0; c < channels; ++c) {
                    sum += samples[i * static_cast<size_t>(channels) + c];
                }
                mono[i] = static_cast<int16_t>(sum / channels);
            }
        }
        if (mono.empty()) {
            return true;
        }

        std::vector<int16_t> play = std::move(mono);
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
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, play.size(), &target);
            std::vector<int16_t> resampled(target);
            uint32_t actual = target;
            if (esp_ae_rate_cvt_process(resampler, reinterpret_cast<esp_ae_sample_t>(play.data()),
                                        play.size(), reinterpret_cast<esp_ae_sample_t>(resampled.data()),
                                        &actual) != ESP_AE_ERR_OK) {
                ESP_LOGE(TAG, "MP3 resample failed");
                return false;
            }
            resampled.resize(actual);
            play = std::move(resampled);
        } else if (!played && !primed) {
            ESP_LOGI(TAG, "MP3 %d Hz %d ch", sample_rate, channels);
        }

        if (!stage_open) {
            stage_start_ms = media_position_ms;
            stage_open = true;
        }
        stage.insert(stage.end(), play.begin(), play.end());
        media_position_ms += static_cast<uint32_t>(sample_n / static_cast<size_t>(channels) * 1000 /
                                                    static_cast<size_t>(sample_rate));
        return flush_stage(false);
    };

    auto decode_pending = [&]() -> bool {
        while (pending.size() >= 4 && !IsCancelled(playback_id)) {
            esp_audio_dec_in_raw_t raw = {
                .buffer = pending.data(),
                .len = static_cast<uint32_t>(pending.size()),
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
                if (raw.consumed > 0 && raw.consumed <= pending.size()) {
                    pending.erase(pending.begin(), pending.begin() + raw.consumed);
                }
                return true;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                const size_t drop = raw.consumed > 0 ? raw.consumed : 1;
                if (drop > pending.size()) {
                    pending.clear();
                } else {
                    pending.erase(pending.begin(), pending.begin() + drop);
                }
                continue;
            }
            if (raw.consumed == 0 || raw.consumed > pending.size()) {
                return raw.consumed == 0;
            }
            pending.erase(pending.begin(), pending.begin() + raw.consumed);
            if (frame.decoded_size < sizeof(int16_t) || info.channel == 0) {
                continue;
            }
            const size_t sample_n = frame.decoded_size / sizeof(int16_t);
            if (!emit(reinterpret_cast<const int16_t*>(frame.buffer), sample_n, info.channel,
                      static_cast<int>(info.sample_rate))) {
                return false;
            }
        }
        return !IsCancelled(playback_id);
    };

    std::array<char, kHttpReadBufferSize> buffer;
    while (!IsCancelled(playback_id)) {
        if (!decode_pending()) {
            failed = true;
            break;
        }
        if (pending.size() > 64 * 1024 && !played) {
            ESP_LOGE(TAG, "MP3 stream has no decodable frame");
            failed = true;
            break;
        }
        auto size = http->Read(buffer.data(), buffer.size());
        if (!size) {
            ESP_LOGE(TAG, "Notification HTTP read failed: %s", size.error().ToString().c_str());
            failed = true;
            break;
        }
        if (*size == 0) {
            break;
        }
        pending.insert(pending.end(), buffer.data(), buffer.data() + *size);
    }

    if (!failed && !IsCancelled(playback_id)) {
        if (!decode_pending()) {
            failed = true;
        } else if (!flush_stage(true)) {
            failed = true;
        }
    }

    close_all();
    return played && !failed && !IsCancelled(playback_id);
}
