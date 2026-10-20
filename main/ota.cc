#include "ota.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
#if __has_include("nodelio_build_stamp.h")
#include "nodelio_build_stamp.h"
#endif
#ifndef NODELIO_BUILD_STAMP
#define NODELIO_BUILD_STAMP "0.0.0"
#endif
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstring>
#include <expected>
#include <system_error>
#include <vector>

#define TAG "Ota"

/* OTA 检查/激活的 HTTP 读超时。明文 TCP 建连在 vendor 的 EspTcp 里无超时，
 * https 建连由 esp_tls 以 10s 兜底；这里限住「等响应」这一段。 */
static constexpr int kOtaHttpTimeoutMs = 15000;

Ota::Ota() {
    /* 墙板开机跳过 CheckVersion，current_version_ 只能在这里取，供「版本」提示用。 */
#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
    /* 升级比较以构建号 YYYYMMDD.NNN 为准（改代码才涨）；小智 PROJECT_VER 仍在 app_desc 里不动。 */
    current_version_ = NODELIO_BUILD_STAMP;
#else
    if (const esp_app_desc_t* app_desc = esp_app_get_description(); app_desc != nullptr) {
        current_version_ = app_desc->version;
    }
#endif
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // USER_DATA may contain board metadata instead of a textual serial number.
    uint8_t serial_number[32] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        const auto data_end = serial_number + sizeof(serial_number);
        const auto text_end = std::find(serial_number, data_end, uint8_t{0});
        const bool printable = std::all_of(
            serial_number, text_end, [](uint8_t byte) { return byte >= 0x20 && byte <= 0x7E; });
        const bool non_blank =
            std::any_of(serial_number, text_end, [](uint8_t byte) { return byte != ' '; });
        const bool zero_padding =
            std::all_of(text_end, data_end, [](uint8_t byte) { return byte == 0; });
        if (printable && non_blank && zero_padding) {
            serial_number_.assign(reinterpret_cast<const char*>(serial_number),
                                  text_end - serial_number);
            has_serial_number_ = true;
        } else if (std::any_of(serial_number, data_end, [](uint8_t byte) { return byte != 0; })) {
            ESP_LOGW(TAG, "Ignoring non-textual or blank eFuse serial number; using activation v1");
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
        /* 墙板走自建清单，不走 api.tenclass.net */
#ifdef CONFIG_NODELIO_PANEL_OTA_URL
        url = CONFIG_NODELIO_PANEL_OTA_URL;
#else
        url = "http://z-y.site/works/esp32s3-slia-86-update.json";
#endif
#else
        url = CONFIG_OTA_URL;
#endif
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    /*
     * HttpClient 默认 30s 的「等响应头/等正文」超时。OTA 用的是 https，建连段由
     * esp_tls 兜底（cfg.timeout_ms 未设时为 10s），但读响应没有额外上限；弱信号下
     * 一次检查要 4.98s 建连 + 4.49s 响应，30s 会让一次卡顿拖住整段激活流程。
     * 收紧到 15s：够 -77dBm 下完成 TLS + 小 JSON，又不至于长时间占着 Activating。
     * 注意：明文 http 的 TCP 路径走 EspTcp::Connect，那里没有超时（vendor 组件，
     * 本仓库不改），所以 OTA 一旦配成 http:// 仍可能长阻塞——保持 https。
     */
    http->SetTimeout(kOtaHttpTimeoutMs);
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
NetworkResult<> Ota::CheckVersion() {
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
    current_version_ = NODELIO_BUILD_STAMP;
    ESP_LOGI(TAG, "Current build stamp: %s (app_desc=%s)", current_version_.c_str(),
             app_desc ? app_desc->version : "?");
#else
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());
#endif

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return std::unexpected(NetworkError::InvalidArgument());
    }

    auto http = SetupHttp();
    std::string data;

#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
    /* 静态清单：只 GET JSON，不 POST 小智系统信息 */
    ESP_LOGI(TAG, "Panel OTA manifest: %s", url.c_str());
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return opened;
    }
#else
    {
        auto& board = Board::GetInstance();
        data = board.GetSystemInfoJson();
        std::string method = data.length() > 0 ? "POST" : "GET";
        http->SetContent(std::move(data));
        if (auto opened = http->Open(method, url); !opened) {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
            return opened;
        }
    }
#endif

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        return std::unexpected(status_code.error());
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", *status_code);
        return std::unexpected(NetworkError::HttpFailed(*status_code));
    }

    data = http->ReadAll();
    http->Close();

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return std::unexpected(NetworkError::ProtocolError());
    }

#if defined(CONFIG_BOARD_TYPE_SILA_86) && defined(CONFIG_NODELIO_PANEL)
    /* 墙板清单只关心 firmware.version / url；忽略小智 mqtt/activation 段 */
    has_activation_code_ = false;
    has_activation_challenge_ = false;
    has_mqtt_config_ = false;
    has_websocket_config_ = false;
    has_server_time_ = false;
    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        cJSON *fw_url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        if (cJSON_IsString(fw_url)) {
            firmware_url_ = fw_url->valuestring;
        }
        if (cJSON_IsString(version) && cJSON_IsString(fw_url) && firmware_url_.size() > 8) {
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
            ESP_LOGI(TAG, "manifest version=%s current=%s newer=%d url=%s", firmware_version_.c_str(),
                     current_version_.c_str(), has_new_version_ ? 1 : 0, firmware_url_.c_str());
        } else {
            ESP_LOGW(TAG, "Panel OTA manifest missing firmware.version/url");
        }
    } else {
        ESP_LOGW(TAG, "Panel OTA manifest has no firmware object");
    }
    cJSON_Delete(root);
    return {};
#else

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return {};
#endif
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (auto opened = http->Open("GET", firmware_url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        return false;
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", *status_code);
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        auto ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (!ret) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", ret.error().ToString().c_str());
            heap_caps_free(buffer);
            return false;
        }
        int n = *ret;

        // Calculate speed and progress every second
        recent_read += n;
        total_read += n;
        buffer_offset += n;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || n == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (n == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


// Versions are compared as dot-separated decimal numbers. Parsing must never throw: exceptions are
// disabled in this project, so a malformed string from the server would otherwise terminate the
// firmware instead of being reported as an error.
std::expected<std::vector<int>, std::string> Ota::ParseVersion(const std::string& version) {
    std::vector<int> version_numbers;
    size_t start = 0;

    while (true) {
        size_t end = version.find('.', start);
        std::string segment =
            version.substr(start, end == std::string::npos ? std::string::npos : end - start);

        int value = 0;
        auto [ptr, ec] = std::from_chars(segment.data(), segment.data() + segment.size(), value);
        if (segment.empty() || ec != std::errc() || ptr != segment.data() + segment.size()) {
            return std::unexpected("invalid version segment \"" + segment + "\"");
        }
        version_numbers.push_back(value);

        if (end == std::string::npos) {
            return version_numbers;
        }
        start = end + 1;
    }
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    auto current = ParseVersion(currentVersion);
    if (!current) {
        ESP_LOGW(TAG, "Cannot parse current version \"%s\": %s", currentVersion.c_str(),
                 current.error().c_str());
        return false;
    }

    auto newer = ParseVersion(newVersion);
    if (!newer) {
        // Never report an update we cannot compare, otherwise a malformed version string would
        // trigger a firmware downgrade or an endless upgrade loop.
        ESP_LOGW(TAG, "Ignoring firmware version \"%s\": %s", newVersion.c_str(),
                 newer.error().c_str());
        return false;
    }

    for (size_t i = 0; i < std::min(current->size(), newer->size()); ++i) {
        if ((*newer)[i] > (*current)[i]) {
            return true;
        } else if ((*newer)[i] < (*current)[i]) {
            return false;
        }
    }

    return newer->size() > current->size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", opened.error().ToString().c_str());
        return ESP_FAIL;
    }

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        return ESP_FAIL;
    }
    if (*status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", *status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}
