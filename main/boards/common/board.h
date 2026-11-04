#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

/**
 * Optional SD/TF slot wiring, exposed by boards that can mount a card.
 * Core code never includes a board config.h; it asks the Board for this.
 * Any pin left at -1 means "not wired". Power enable is usually active-low
 * (PWRn), and is driven high (off) whenever the card is not mounted.
 */
struct SdCardPins {
    int clk = -1;
    int cmd = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int d3 = -1;
    int power_en = -1;
    bool power_active_low = true;
    int card_detect = -1;
    bool cd_active_low = true;
    int max_freq_khz = 20000;
    bool four_bit = true;
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    /** 触控/按键交互时重置息屏计时（默认空操作）。 */
    virtual void NoteUserActivity() {}
    /** 墙板背光已灭（PowerSaveTimer 息屏中）；默认 false。 */
    virtual bool IsInSleepMode() const { return false; }
    /** 运行时更新空闲息屏秒数（墙板省电设置）；默认空操作。 */
    virtual void SetIdleSleepSeconds(int seconds) { (void)seconds; }
    /** 运行时更新空闲断网秒数（墙板，与息屏独立）；默认空操作。 */
    virtual void SetIdleWifiSeconds(int seconds) { (void)seconds; }
    /** 运行时开关空闲断网省电（墙板）；息屏仍走 idle_sleep。默认空操作。 */
    virtual void SetPowerSaveEnabled(bool enabled) { (void)enabled; }
    /** 对话通道关闭后的短延迟息屏（墙板）；默认空。勿在开机激活路径调用。 */
    virtual void RequestGoodbyeSleep() {}
    /** 板级 SD/TF 卡接线；未接卡的板保持默认（全部 -1）即可。 */
    virtual SdCardPins GetSdCardPins() { return SdCardPins{}; }
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
