#include "box_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>

#define TAG "BoxAudioCodec"

BoxAudioCodec::BoxAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
                             gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout,
                             gpio_num_t din, gpio_num_t pa_pin, uint8_t es8311_addr,
                             uint8_t es7210_addr, bool input_reference, float input_gain,
                             int reference_gain_channel, float reference_gain) {
    duplex_ = true;                              // 是否双工
    input_reference_ = input_reference;          // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1;  // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = input_gain;
    reference_gain_channel_ = reference_gain_channel;
    reference_gain_ = reference_gain;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = out_ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8311_codec_new(&es8311_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "BoxAudioDevice initialized");
}

BoxAudioCodec::~BoxAudioCodec() {
    if (output_device_opened_) {
        ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
        output_device_opened_ = false;
    }
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void BoxAudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void BoxAudioCodec::SetOutputVolumeTransient(int volume) {
    if (output_device_opened_) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    }
    AudioCodec::SetOutputVolumeTransient(volume);
}

void BoxAudioCodec::SetOutputVolume(int volume) {
    if (output_device_opened_) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    }
    AudioCodec::SetOutputVolume(volume);
}

void BoxAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        /* duplex 开麦会重配共享 TX；TX 未先占 DMA 时此处易 OOM 并留下坏句柄。 */
        if (!output_device_opened_) {
            esp_codec_dev_sample_info_t ofs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = (uint32_t)output_sample_rate_,
                .mclk_multiple = 0,
            };
            esp_err_t oerr = esp_codec_dev_open(output_dev_, &ofs);
            if (oerr != ESP_OK) {
                ESP_LOGE(TAG, "EnableInput: prime TX failed %s", esp_err_to_name(oerr));
                return;
            }
            output_device_opened_ = true;
            (void)esp_codec_dev_set_out_vol(output_dev_, output_volume_);
            (void)esp_codec_dev_set_out_mute(output_dev_, true);
            AudioCodec::EnableOutput(false);
        }
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        esp_err_t err = esp_codec_dev_open(input_dev_, &fs);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "EnableInput open failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_set_in_channel_gain(
            input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_));
        if (input_reference_ && reference_gain_channel_ >= 0) {
            // ES7210 gain masks use physical MIC numbering, which differs
            // from the TDM slot order (MIC1, MIC3, MIC2, MIC4).
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_set_in_channel_gain(
                input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(reference_gain_channel_),
                reference_gain_));
        }
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
    /* 开麦会重配共享 I2S TX；若喇叭逻辑已开，重刷 DAC */
    if (enable && output_enabled_ && output_device_opened_) {
        (void)esp_codec_dev_set_out_vol(output_dev_, output_volume_);
        (void)esp_codec_dev_set_out_mute(output_dev_, false);
    }
}

void BoxAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable) {
        /* Play 16bit 1 channel. Open once；即便已 enable 也重刷 vol+unmute
         * （AFE 开麦会重配 I2S，仅靠旗标 early-return 会留下哑音）。 */
        if (!output_device_opened_) {
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = (uint32_t)output_sample_rate_,
                .mclk_multiple = 0,
            };
            esp_err_t err = esp_codec_dev_open(output_dev_, &fs);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "EnableOutput open failed: %s", esp_err_to_name(err));
                return;
            }
            output_device_opened_ = true;
        }
        (void)esp_codec_dev_set_out_vol(output_dev_, output_volume_);
        esp_err_t mu = esp_codec_dev_set_out_mute(output_dev_, false);
        if (mu != ESP_OK) {
            ESP_LOGW(TAG, "set_out_mute(false) failed: %s", esp_err_to_name(mu));
        }
        if (output_enabled_) {
            return;
        }
        AudioCodec::EnableOutput(true);
        return;
    }
    if (!output_enabled_ && output_device_opened_) {
        /* 已 mute 占坑：保持 TX DMA，勿重复操作 */
        return;
    }
    if (!output_enabled_) {
        return;
    }
    /* Keep TX open: ES7210 RX shares I2S clock. Close+reopen after sleep
     * leaves playback silent on ESP-IDF 6 (same fix as Es8388AudioCodec).
     * Mute still cuts hiss; board sleep may call this while wake RX runs. */
    if (output_device_opened_) {
        (void)esp_codec_dev_set_out_mute(output_dev_, true);
    }
    AudioCodec::EnableOutput(false);
}

int BoxAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int BoxAudioCodec::Write(const int16_t* data, int samples) {
    /* TX 已 open 时始终按 I2S 节拍写。EnableOutput(false) 只做 set_out_mute，
     * 若此处因 output_enabled_ 直接 return，播放队列会以 CPU 速度抽干，
     * speaking 入口的 mute 舞步会把唤醒首句 TTS 整段丢光。 */
    if (output_device_opened_ && output_dev_ != nullptr && data != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
