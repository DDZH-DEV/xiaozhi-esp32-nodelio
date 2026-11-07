#ifndef _OTA_H
#define _OTA_H

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <esp_err.h>
#include <sdkconfig.h>
#include "board.h"
#include "network_error.h"

class Ota {
public:
    Ota();
    ~Ota();

    NetworkResult<> CheckVersion();
    /**
     * 墙板：向小智语音 OTA 地址 POST 设备信息，只写入 mqtt/websocket（及激活信息）。
     * 忽略响应里的 firmware；不碰墙板固件清单。url 空则用官方 CONFIG_OTA_URL。
     */
    NetworkResult<> FetchVoiceServer(const std::string& url = {});
    esp_err_t Activate();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() {
#if CONFIG_FIRMWARE_OTA_ENABLED
        return has_new_version_;
#else
        return false;
#endif
    }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();
    /** 语音云 OTA：wifi/ota_url，空则官方 CONFIG_OTA_URL（不是墙板固件清单）。 */
    static std::string GetVoiceOtaUrl();

private:
    void ClearVoiceTransportNvs();
    NetworkResult<> ParseVoiceServerResponse(cJSON* root);
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::expected<std::vector<int>, std::string> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
