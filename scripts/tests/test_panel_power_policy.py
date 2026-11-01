"""Execute production sleep/wake policy functions with counted hardware effects."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest

from test_panel_ui_lifecycle import function

ROOT = Path(__file__).resolve().parents[2]


class PanelPowerPolicyTests(unittest.TestCase):
    def test_screen_sleep_legacy_preference_and_disabled_voice(self):
        compiler = shlex.split(os.environ.get("CXX", ""))
        if not compiler:
            compiler = [shutil.which("c++") or shutil.which("clang++") or ""]
        if not compiler[0]:
            self.skipTest("requires a host C++ compiler")
        source = r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#define CONFIG_NODELIO_PANEL 1
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ESP_LOGI(...)
#define ESP_LOGD(...)
#define ESP_ERROR_CHECK(x) (void)(x)
struct lv_timer_t {};
struct photo_rt_t { bool paused; int64_t next_ms; };
constexpr int NL_TAB_HOME=0, LV_OBJ_FLAG_HIDDEN=1;
int s_active_tab=0,s_photo_rt_n=1,advances=0;
void* s_home=(void*)1;
bool wifi_asleep=false,s_screen_on=false,hidden=false;
photo_rt_t s_photo_rt[1]={{false,1}};
bool lv_obj_has_flag(void*,int){return hidden;}
bool nodelio_wifi_is_asleep(){return wifi_asleep;}
int64_t esp_timer_get_time(){return 1000000;}
void photo_advance(photo_rt_t*){++advances;}
bool legacy_sleep=false;
struct Settings { Settings(const char*,bool){} bool GetBool(const char*,bool){return legacy_sleep;} };
int starts=0,stops=0,wakes=0;
int esp_timer_start_periodic(int,int){++starts;return 0;}
int esp_timer_stop(int){++stops;return 0;}
struct PowerSaveTimer {
 bool enabled_=false, respect_wifi_sleep_setting_=true;
 int ticks_=0,activity_ticks_=0,power_save_timer_=0;
 void WakeUp(){++wakes;} void SetEnabled(bool);
};
bool cloud=false, media=true;
int output_calls=0, engine_starts=0, paused=0, yields=0;
bool nodelio_xiaozhi_enabled(){return cloud;}
bool nodelio_media_is_playing(){return media;}
void nodelio_media_pause_for_voice(){++paused;}
void nodelio_offline_cmd_yield_for_cloud_audio(){++yields;}
constexpr int AS_EVENT_WAKE_WORD_RUNNING=1;
void xEventGroupClearBits(int* p,int b){*p&=~b;}
void xEventGroupSetBits(int* p,int b){*p|=b;}
void esp_ae_rate_cvt_reset(void*){}
struct Engine { bool on=false; bool HasWakeWord(){return true;}
 void EnableWakeWordDetection(bool b){on=b;} };
struct Codec { void EnableOutput(bool){++output_calls;} } codec;
struct AudioService {
 struct Callbacks { std::function<bool()> can_enable_wake_word; } callbacks_;
 int bits=0, encoded=0,discarded=0, voice_calls=0;
 int* event_group_=&bits;
 bool audio_engine_initialized_=false;
 Codec* codec_=&codec;
 std::unique_ptr<Engine> audio_engine_=std::make_unique<Engine>();
 std::mutex input_resampler_mutex_; void* input_resampler_=nullptr;
 bool InitializeAudioEngine(){++engine_starts;return audio_engine_initialized_=true;}
 void EnableWakeWordDetection(bool);
 void DiscardWakeWordAudio(){++discarded;}
 void EncodeWakeWord(){++encoded;}
 std::string GetLastWakeWord(){return "wake";}
 void EnableVoiceProcessing(bool){++voice_calls;}
 bool PopPacketFromSendQueue(){return false;}
 void ResetDecoder(){} void PlaySound(int){}
};
enum { kDeviceStateIdle,kDeviceStateNotifying,kDeviceStateSpeaking,kDeviceStateListening,
       kDeviceStateActivating,kDeviceStateConnecting,kAbortReasonWakeWordDetected };
namespace Lang { namespace Sounds { constexpr int OGG_POPUP=0; } }
struct Protocol { bool IsAudioChannelOpened(){return false;}
 void SendStartListening(int){} } protocol;
struct Application {
 AudioService audio_service_; Protocol* protocol_=&protocol;
 int state=kDeviceStateIdle, transitions=0,continued=0,aborts=0,notifications=0;
 bool play_popup_on_listening_=false;
 int GetDeviceState(){return state;}
 bool SetDeviceState(int s){state=s;++transitions;return true;}
 void Schedule(std::function<void()> cb){cb();}
 void ContinueWakeWordInvoke(const std::string&){++continued;}
 void StopNotification(){++notifications;}
 void AbortSpeaking(int){++aborts;}
 int GetDefaultListeningMode(){return 0;}
 void SetListeningMode(int){}
 void HandleWakeWordDetectedEvent(); void BeginWakeWordInvoke(const std::string&);
};
'''
        for path, names in {
            "main/nodelio/overlay_home_tiles.cc": ["on_photo_timer("],
            "main/boards/common/power_save_timer.cc": ["PowerSaveTimer::SetEnabled("],
            "main/audio/audio_service.cc": ["AudioService::EnableWakeWordDetection("],
            "main/application.cc": ["Application::HandleWakeWordDetectedEvent(",
                                    "Application::BeginWakeWordInvoke("],
        }.items():
            actual = (ROOT / path).read_text(encoding="utf-8")
            source += "\n".join(function(actual, name) for name in names) + "\n"
        source += r'''
int main(){
 on_photo_timer(nullptr); assert(advances==0); // P4/network awake but screen off
 s_screen_on=true; on_photo_timer(nullptr); assert(advances==1);
 hidden=true; on_photo_timer(nullptr); assert(advances==1);
 hidden=false; wifi_asleep=true; on_photo_timer(nullptr); assert(advances==1);
 wifi_asleep=false; s_active_tab=1; on_photo_timer(nullptr); assert(advances==1);
 s_active_tab=0; s_photo_rt[0].paused=true; on_photo_timer(nullptr); assert(advances==1);
 s_photo_rt[0].paused=false; on_photo_timer(nullptr); assert(advances==2);
 PowerSaveTimer legacy; legacy.SetEnabled(true); assert(!legacy.enabled_ && starts==0);
 PowerSaveTimer panel; panel.respect_wifi_sleep_setting_=false;
 panel.SetEnabled(true); panel.SetEnabled(true); assert(panel.enabled_ && starts==1);
 panel.SetEnabled(false); assert(!panel.enabled_ && stops==1 && wakes==1);
 legacy_sleep=true; legacy.SetEnabled(true); assert(legacy.enabled_ && starts==2);
 AudioService audio; audio.callbacks_.can_enable_wake_word=[](){return cloud;};
 audio.EnableWakeWordDetection(true);
 assert(!audio.audio_engine_initialized_ && !audio.bits && !engine_starts && !output_calls);
 cloud=true; audio.EnableWakeWordDetection(true);
 assert(audio.bits && audio.audio_engine_->on && engine_starts==1 && output_calls==2);
 cloud=false; audio.EnableWakeWordDetection(true);
 assert(!audio.bits && !audio.audio_engine_->on && engine_starts==1 && output_calls==2);
 audio.callbacks_.can_enable_wake_word={}; audio.EnableWakeWordDetection(true);
 assert(audio.bits && audio.audio_engine_->on); // other boards keep original behavior
 Application app;
 for(int state:{kDeviceStateIdle,kDeviceStateNotifying,kDeviceStateSpeaking,kDeviceStateListening}) {
   app.state=state; app.HandleWakeWordDetectedEvent();
   assert(app.state==state && !app.transitions && !app.continued && !paused && !yields);
   assert(!app.audio_service_.encoded && !app.aborts && !app.notifications);
 }
 app.BeginWakeWordInvoke("manual"); assert(!app.transitions && !app.audio_service_.encoded);
 cloud=true; app.state=kDeviceStateIdle; app.HandleWakeWordDetectedEvent();
 assert(app.state==kDeviceStateConnecting && app.continued==1 && app.transitions==1);
#if CONFIG_BOARD_TYPE_SILA_86
 assert(app.audio_service_.encoded==0);
#else
 assert(app.audio_service_.encoded==1);
#endif
}
'''
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.cleanup_directory, directory)
        work = Path(directory.name)
        (work / "power.cpp").write_text(source, encoding="utf-8")
        for s3 in (0, 1):
            with self.subTest(s3=s3):
                exe = work / (f"power-{s3}.exe" if os.name == "nt" else f"power-{s3}")
                result = subprocess.run(compiler + ["-std=c++17", f"-DCONFIG_BOARD_TYPE_SILA_86={s3}",
                                        str(work / "power.cpp"), "-o", str(exe)],
                                        capture_output=True, text=True, encoding="utf-8", timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(exe)], capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @staticmethod
    def cleanup_directory(directory):
        for attempt in range(6):
            try:
                directory.cleanup()
                return
            except PermissionError:
                if attempt == 5:
                    raise
                time.sleep(0.2 * (attempt + 1))
