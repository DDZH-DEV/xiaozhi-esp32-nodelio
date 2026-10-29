#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

/* 帧长用 INVALID：音乐 Ogg 包 TOC 常混 2.5/5/10/20ms，写死再建解码器会整段 skip。 */
#define OPUS_DEC_CFG(_sample_rate)                                                   \
    (esp_opus_dec_cfg_t) {                                                           \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,           \
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID, .self_delimited = false, \
    }

static bool OpusTocTiming(const uint8_t* data, size_t size, int* frame_us, int* frame_count) {
    if (data == nullptr || size == 0 || frame_us == nullptr || frame_count == nullptr) {
        return false;
    }
    const uint8_t toc = data[0];
    const uint8_t config = toc >> 3;
    if (config < 12) {
        static constexpr int kSilk[] = {10000, 20000, 40000, 60000};
        *frame_us = kSilk[config & 0x03];
    } else if (config < 16) {
        *frame_us = (config & 0x01) ? 20000 : 10000;
    } else {
        static constexpr int kCelt[] = {2500, 5000, 10000, 20000};
        *frame_us = kCelt[config & 0x03];
    }
    switch (toc & 0x03) {
        case 0:
            *frame_count = 1;
            break;
        case 1:
        case 2:
            *frame_count = 2;
            break;
        default:
            if (size < 2) {
                return false;
            }
            *frame_count = data[1] & 0x3f;
            if (*frame_count == 0) {
                *frame_count = 1;
            }
            break;
    }
    return *frame_us > 0;
}

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    /* 云端/提示音 Opus 均为 16k。墙板输出 24k：开机按 16k 建解码器 + 重采样，
     * 避免首包唤醒问候在 AFE 占满 DRAM 时 close/open 失败 → 解码器变空 → 全程无声。 */
    constexpr int kNetOpusRate = 16000;
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(kNetOpusRate);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = kNetOpusRate;
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
        if (decoder_sample_rate_ != codec->output_sample_rate()) {
            esp_ae_rate_cvt_cfg_t output_resampler_cfg =
                RATE_CVT_CFG(decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
            auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
            if (output_resampler_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
            } else {
                ESP_LOGI(TAG, "Opus decode %d → out %d (resampler ready)", decoder_sample_rate_,
                         codec->output_sample_rate());
            }
        }
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_.store(false);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                           AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                                           AS_EVENT_LOCAL_COMMAND_RUNNING |
                                           AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_BOARD_TYPE_SILA_86
    /* RGB full_refresh + AFE 时原 opus=2 易饿死 → 播音断续 */
    constexpr UBaseType_t kOutPrio = 6;
    constexpr UBaseType_t kOpusPrio = 6;
#else
    constexpr UBaseType_t kOutPrio = 4;
    constexpr UBaseType_t kOpusPrio = 2;
#endif

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 3, this, 6, &audio_input_task_handle_, 0);

    /* Start the audio output task */
#if CONFIG_BOARD_TYPE_SILA_86
    /* 输出任务喂 6×240 帧（~30ms）的 I2S DMA，是硬期限任务：钉 Core1，
     * Core0 上 WiFi(23)/lwIP 的突发曾把它拖过 30ms → 爆音；同核 notify_http
     * 已降到 5，解码突发随时可被输出抢占。 */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, kOutPrio, &audio_output_task_handle_, 1);
#else
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, kOutPrio, &audio_output_task_handle_);
#endif
#else
    /* Start the audio input task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048, this, kOutPrio, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->OpusCodecTask();
            vTaskDelete(NULL);
        },
        "opus_codec", 2048 * 12, this, kOpusPrio, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_.store(true);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                         AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                                   &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                    (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits =
        AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_LOCAL_COMMAND_RUNNING;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            bool testing_queue_full = false;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                testing_queue_full = audio_testing_queue_.full();
            }
            if (testing_queue_full) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine (含 K4 本地口令窗) */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                    AS_EVENT_LOCAL_COMMAND_RUNNING)) {
            int samples = 160;  // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
                /* taskYIELD 调度不到 IDLE（优先级更低）。播放时重采样会占满 CPU0，
                 * IDLE0 喂不了看门狗，屏会闪。让出 1ms 给 IDLE。 */
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(
            lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_.load(); });
        if (service_stopped_.load()) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        output_in_flight_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        if (task.playback_id != 0 && callbacks_.on_playback_progress) {
            callbacks_.on_playback_progress(task.playback_id, task.media_position_ms);
        }

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        if (debug_statistics_.playback_count < 3) {
            int16_t peak = 0;
            for (int16_t s : task.pcm) {
                int16_t a = s < 0 ? (int16_t)(-s) : s;
                if (a > peak) {
                    peak = a;
                }
            }
            ESP_LOGI(TAG, "pcm out #%u samples=%u peak=%d out_en=%d vol=%d",
                     (unsigned)debug_statistics_.playback_count, (unsigned)task.pcm.size(),
                     (int)peak, codec_->output_enabled() ? 1 : 0, codec_->output_volume());
        }
#endif
        codec_->OutputData(task.pcm);
        completed_playback_count_.fetch_add(1);

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        bool notify_drained = false;
        lock.lock();
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task.timestamp > 0) {
            if (timestamp_queue_.full()) {
                timestamp_queue_.pop_front();
            }
            timestamp_queue_.push_back(task.timestamp);
        }
#endif
        output_in_flight_ = false;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_encode_queue_.empty() ||
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            AudioTask task;
            task.type = kAudioTaskTypeDecodeToPlaybackQueue;
            task.timestamp = packet->timestamp;
            task.playback_id = packet->playback_id;
            task.media_position_ms = packet->media_position_ms;

            int toc_frame_us = 20000;
            int toc_frames = 1;
            if (!OpusTocTiming(packet->payload.data(), packet->payload.size(), &toc_frame_us,
                               &toc_frames)) {
                toc_frame_us = packet->frame_duration > 0 ? packet->frame_duration * 1000 : 20000;
                toc_frames = 1;
            }
            SetDecodeSampleRate(packet->sample_rate, toc_frame_us, toc_frames);
            bool decoded = false;
            if (opus_decoder_ != nullptr) {
                task.pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t*)(task.pcm.data()),
                    .len = (uint32_t)(task.pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    const size_t need =
                        (size_t)decoder_sample_rate_ * (size_t)toc_frame_us * (size_t)toc_frames /
                            1000000u +
                        (size_t)decoder_sample_rate_ / 50u;
                    task.pcm.resize(need);
                    out_frame.buffer = (uint8_t*)(task.pcm.data());
                    out_frame.len = (uint32_t)(task.pcm.size() * sizeof(int16_t));
                    out_frame.decoded_size = 0;
                    raw.consumed = 0;
                    ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                }
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task.pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    /* OpusHead pre-skip：按解码采样率折算后丢掉开头（每首 Ogg 开头怪声） */
                    if (opus_preskip_48k_ > 0 && opus_preskip_remain_ == 0 &&
                        decoder_sample_rate_ > 0) {
                        opus_preskip_remain_ = static_cast<uint32_t>(
                            (static_cast<uint64_t>(opus_preskip_48k_) *
                             static_cast<uint32_t>(decoder_sample_rate_)) /
                            48000u);
                        opus_preskip_48k_ = 0;
                    }
                    if (opus_preskip_remain_ > 0 && !task.pcm.empty()) {
                        const size_t drop =
                            task.pcm.size() < opus_preskip_remain_ ? task.pcm.size()
                                                                   : (size_t)opus_preskip_remain_;
                        task.pcm.erase(task.pcm.begin(), task.pcm.begin() + static_cast<long>(drop));
                        opus_preskip_remain_ -= static_cast<uint32_t>(drop);
                    }
                    if (task.pcm.empty()) {
                        decoded = false;
                    } else if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                               output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task.pcm.size(),
                                                               &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task.pcm.data(),
                                                task.pcm.size(), (esp_ae_sample_t)resampled.data(),
                                                &actual_output);
                        resampled.resize(actual_output);
                        task.pcm = std::move(resampled);
                        decoded = true;
                    } else {
                        decoded = true;
                    }
                } else {
                    ESP_LOGW(TAG, "skip opus packet len=%u err=%d toc=%uus×%d",
                             (unsigned)packet->payload.size(), (int)ret, toc_frame_us, toc_frames);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            } else if (decoded && generation != playback_generation_) {
                ESP_LOGW(TAG, "drop decoded pcm: generation mismatch %u→%u (ResetDecoder raced)",
                         (unsigned)generation, (unsigned)playback_generation_);
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task.timestamp;

            if (opus_encoder_ != nullptr && task.pcm.size() == encoder_frame_size_) {
                packet->payload.resize(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task.pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = packet->payload.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.resize(out.encoded_bytes);

                    if (task.type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* Never let a full send queue stall encoding: stale realtime
                             * audio is useless to the server, so drop the oldest packet. */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task.type == kAudioTaskTypeEncodeToTestingQueue) {
                        bool testing_queue_full = false;
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            testing_queue_full = !audio_testing_queue_.push_back(std::move(packet));
                        }
                        if (testing_queue_full && callbacks_.on_audio_testing_queue_full) {
                            callbacks_.on_audio_testing_queue_full();
                        }
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG,
                         "Failed to encode audio: encoder not configured or invalid frame size "
                         "(got %u, expected %u)",
                         task.pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    // 兼容旧调用：frame_duration 当作单帧毫秒
    int frame_us = frame_duration > 0 ? frame_duration * 1000 : 20000;
    if (frame_duration == 3) {
        frame_us = 2500; // demuxer 对 2.5ms 的近似
    }
    SetDecodeSampleRate(sample_rate, frame_us, 1);
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_us, int frame_count) {
    if (frame_us <= 0) {
        frame_us = 20000;
    }
    if (frame_count <= 0) {
        frame_count = 1;
    }
    /* INVALID 帧长：PCM 按文档按 60ms 计；缓冲再按本包 TOC 扩到 ≤120ms */
    int packet_ms = (frame_us * frame_count + 500) / 1000;
    if (packet_ms < 60) {
        packet_ms = 60;
    }
    if (packet_ms > 120) {
        packet_ms = 120;
    }
    /* 只随采样率重建；勿因 TOC 帧长变化反复 open/close（音乐 Ogg 会炸） */
    if (decoder_sample_rate_ == sample_rate && opus_decoder_ != nullptr) {
        decoder_duration_ms_ = packet_ms;
        decoder_frame_us_ = frame_us;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * packet_ms;
        if (decoder_frame_size_ < 1) {
            decoder_frame_size_ = decoder_sample_rate_ / 50;
        }
        return;
    }
    /* 先开新再关旧：open 失败时勿留下空解码器（否则 popup/TTS 全程无声） */
    void* old_dec = nullptr;
    const int old_rate = decoder_sample_rate_;
    {
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        old_dec = opus_decoder_;
        opus_decoder_ = nullptr;
    }
    void* new_dec = nullptr;
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &new_dec);
    if (new_dec == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder rate=%d err=%d — keep %d", sample_rate,
                 ret, old_rate);
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        opus_decoder_ = old_dec;
        return;
    }
    if (old_dec != nullptr) {
        esp_opus_dec_close(old_dec);
    }
    {
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        opus_decoder_ = new_dec;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = packet_ms;
    decoder_frame_us_ = frame_us;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * packet_ms;
    if (decoder_frame_size_ < 1) {
        decoder_frame_size_ = decoder_sample_rate_ / 50; // ≥20ms
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_,
                 codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    } else if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
        output_resampler_ = nullptr;
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    AudioTask task;
    task.type = type;
    task.pcm = std::move(pcm);

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            task.timestamp = timestamp_queue_.front();
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            audio_encode_queue_.pop_front();
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t generation = playback_generation_;
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this, generation]() {
                return service_stopped_.load() || generation != playback_generation_ ||
                       audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            return false;
        }
    }
    if (service_stopped_.load() || generation != playback_generation_) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

bool AudioService::EnqueuePlaybackPcm(std::vector<int16_t> pcm, uint32_t playback_id,
                                      uint32_t media_position_ms) {
    if (pcm.empty()) {
        return true;
    }
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    const uint32_t generation = playback_generation_;
    audio_queue_cv_.wait(lock, [this, generation]() {
        return service_stopped_.load() || generation != playback_generation_ ||
               audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE;
    });
    if (service_stopped_.load() || generation != playback_generation_) {
        return false;
    }
    AudioTask task;
    task.type = kAudioTaskTypeDecodeToPlaybackQueue;
    task.pcm = std::move(pcm);
    task.playback_id = playback_id;
    task.media_position_ms = media_position_ms;
    playback_drained_notified_ = false;
    if (!audio_playback_queue_.push_back(std::move(task))) {
        return false;
    }
    audio_queue_cv_.notify_all();
    return true;
}

bool AudioService::TryEnqueuePlaybackPcm(std::vector<int16_t>& pcm, uint32_t playback_id,
                                         uint32_t media_position_ms, bool* full) {
    if (full) {
        *full = false;
    }
    if (pcm.empty()) {
        return true;
    }
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_.load()) {
        return false;
    }
    if (audio_playback_queue_.size() >= MAX_PLAYBACK_TASKS_IN_QUEUE) {
        if (full) {
            *full = true;
        }
        return false;
    }
    AudioTask task;
    task.type = kAudioTaskTypeDecodeToPlaybackQueue;
    task.pcm = std::move(pcm);
    task.playback_id = playback_id;
    task.media_position_ms = media_position_ms;
    playback_drained_notified_ = false;
    if (!audio_playback_queue_.push_back(std::move(task))) {
        pcm = std::move(task.pcm);
        if (full) {
            *full = true;
        }
        return false;
    }
    audio_queue_cv_.notify_all();
    return true;
}

bool AudioService::PlaybackQueueHasRoom() const {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

void AudioService::DiscardWakeWordAudio() {
    if (audio_engine_) {
        audio_engine_->DiscardWakeWordAudio();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        /* AFE 创建后 INTERNAL largest 常只剩 ~4KB。若此时才首次开麦，
         * duplex 重配 TX DMA 会失败并在 i2s_channel_enable 空指针崩。
         * 先开喇叭占住 TX（随即 mute 保 DMA），再建 AFE。
         * 引擎已建好后再做 true→false：speaking 入队的首包 TTS 会在
         * output_enabled_=false 时被 Write 瞬间丢光 → 唤醒首句无声。 */
        if (codec_ && !audio_engine_initialized_) {
            codec_->EnableOutput(true);
            codec_->EnableOutput(false);
        }
        if (!InitializeAudioEngine()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
        auto* lite_engine = static_cast<LiteAudioEngine*>(audio_engine_.get());
        if (!lite_engine->RestoreWakeWordResources()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
#endif
        if (!audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::ReleaseWakeWordResources() {
#if !(CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31)
    if (!audio_engine_initialized_) {
        return;
    }
    if (xEventGroupGetBits(event_group_) &
        (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
        ESP_LOGW(TAG, "Cannot release WakeNet while the audio engine is active");
        return;
    }
    static_cast<LiteAudioEngine*>(audio_engine_.get())->ReleaseWakeWordResources();
#endif
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        /* 同 EnableWakeWordDetection：仅首次建引擎时占 TX；已初始化勿 mute。 */
        if (codec_ && !audio_engine_initialized_) {
            codec_->EnableOutput(true);
            codec_->EnableOutput(false);
        }
        if (!InitializeAudioEngine()) {
            return;
        }
#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL
        /* 云端问候 UDP 可能已在队列/解码中；ResetDecoder 会 ++generation 丢掉在途 PCM → 有字无声 */
        bool playback_busy = false;
        {
            std::lock_guard<std::mutex> lock(audio_queue_mutex_);
            playback_busy = !audio_decode_queue_.empty() || !audio_playback_queue_.empty() ||
                            decode_in_flight_ || output_in_flight_;
        }
        if (playback_busy) {
            ESP_LOGW(TAG, "EnableVoiceProcessing: skip ResetDecoder (cloud TTS in flight)");
        } else {
            ResetDecoder();
        }
#else
        ResetDecoder();
#endif
        audio_input_need_warmup_ = true;
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableLocalCommandMode(bool enable) {
    ESP_LOGI(TAG, "%s local command mode", enable ? "Enabling" : "Disabling");
    if (!audio_engine_initialized_ && enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
    }
    if (audio_engine_) {
        if (enable) {
            audio_engine_->EnableWakeWordDetection(false);
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            {
                std::lock_guard<std::mutex> lock(input_resampler_mutex_);
                if (input_resampler_ != nullptr) {
                    esp_ae_rate_cvt_reset(input_resampler_);
                }
            }
            audio_engine_->EnableLocalCommandMode(true);
            xEventGroupSetBits(event_group_, AS_EVENT_LOCAL_COMMAND_RUNNING);
        } else {
            audio_engine_->EnableLocalCommandMode(false);
            xEventGroupClearBits(event_group_, AS_EVENT_LOCAL_COMMAND_RUNNING);
        }
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_.clear();
        audio_decode_queue_.swap(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnPacket(
        [this](const uint8_t* data, int sample_rate, int frame_duration_ms, size_t size) {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration_ms;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            PushPacketToDecodeQueue(std::move(packet), true);
        });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ArmOpusPreSkip48k(uint32_t pre_skip_48k)
{
    opus_preskip_48k_ = pre_skip_48k;
    opus_preskip_remain_ = 0;
}

void AudioService::ResetDecoder() {
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        opus_preskip_48k_ = 0;
        opus_preskip_remain_ = 0;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::IsPlaybackDrainedLocked() const {
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() && !decode_in_flight_ &&
           !output_in_flight_;
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked() || playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        const EventBits_t bits = xEventGroupGetBits(event_group_);
        const bool input_wanted =
            (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                     AS_EVENT_LOCAL_COMMAND_RUNNING | AS_EVENT_AUDIO_TESTING_RUNNING)) != 0;
        /* 唤醒词还开着就不要因超时关麦，否则播歌久了唤不醒。 */
        if (!input_wanted) {
            // ADC continuous start/stop must run in the same task. Wake the audio
            // input task instead of closing the codec from the esp_timer task.
            xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
        }
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}
