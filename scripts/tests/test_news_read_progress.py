"""Exercise the firmware's news cursor and playback-drain continuation on the host."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class NewsReadProgressTests(unittest.TestCase):
    def test_sentence_alignment_and_actual_playback_completion(self):
        compiler = shlex.split(os.environ.get("CXX", ""))
        if not compiler:
            compiler = [shutil.which("c++") or shutil.which("clang++") or ""]
        if not compiler[0]:
            self.skipTest("requires a host C++ compiler (or CXX)")
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        finish = function(application, "void Application::FinishNewsReadPlayback()")
        cancel = function(application, "void Application::CancelNewsReadPlayback()")
        sentence = function(application, "void Application::OnNewsReadSentence(")
        idle_tick = function(application, "void Application::HandleListenIdleTick()")
        news_ui = (ROOT / "main/nodelio/skill/news_list_ui.inc").read_text(encoding="utf-8")
        chunk_end = function(news_ui, "static size_t news_utf8_chunk_end(")
        driver = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include "news_read_progress.h"
#define ESP_LOGI(...)
#define ESP_LOGW(...)
using DeviceState = int;
enum { kDeviceStateSpeaking, kDeviceStateListening, kDeviceStateIdle };
enum { kListeningModeManualStop, kListeningModeAutoStop };
NewsReadProgress progress;
bool current_article = true;
int idle_limit = 17;
int nodelio_listen_idle_seconds() { return idle_limit; }
void nodelio_panel_ui_toast(const char*) {}
namespace Lang { namespace Sounds { constexpr int OGG_POPUP = 1; } }
struct Display { void SetChatMessage(const char*, const char*) {} };
struct Board {
    Display display;
    static Board& GetInstance() { static Board board; return board; }
    Display* GetDisplay() { return &display; }
};
void nodelio_skill_news_sync_read_btn() {}
void nodelio_skill_news_cancel_read() { progress.Cancel(); }
uint32_t nodelio_skill_news_on_sentence(const char* text) {
    size_t offset = 0;
    return current_article && progress.Sentence(text, offset) ? progress.token() : 0;
}
bool nodelio_skill_news_finish_chunk(uint32_t token, bool played, bool* more) {
    return current_article && progress.Finish(token, played, *more);
}
struct Audio {
    bool idle = false;
    uint32_t count = 0;
    int popups = 0;
    void PlaySound(int) { ++popups; idle = false; }
    bool IsPlaybackIdle() const { return idle; }
    uint32_t GetPlaybackCount() const { return count; }
};
struct Protocol {
    bool open = true;
    int closes = 0;
    void CloseAudioChannel() { ++closes; open = false; }
    bool IsAudioChannelOpened() const { return open; }
};
class Application {
public:
    Audio audio_service_;
    Protocol protocol;
    Protocol* protocol_ = &protocol;
    uint32_t news_playback_token_ = 0, news_playback_start_count_ = 0;
    bool news_tts_stopped_ = false, aborted_ = false, news_read_continue_ = false;
    bool news_reading_ = false, listen_idle_pending_close_ = false;
    bool news_awaiting_mcp_ = false, suppressed = false;
    std::atomic<uint32_t> cloud_audio_rx_count_{0};
    uint32_t listen_idle_last_audio_rx_count_ = 0;
    int listen_idle_ticks_ = 0, listen_announce_hold_ticks_ = 0;
    int news_mcp_detect_retries_ = 0, state = kDeviceStateSpeaking;
    int listening_mode_ = kListeningModeAutoStop, requests = 0;
    int GetDeviceState() const { return state; }
    void SetDeviceState(int value) { state = value; }
    void SendNewsReadRequest(bool interrupt) { assert(!interrupt); ++requests; }
    void FinishNewsReadPlayback();
    void CancelNewsReadPlayback();
    void OnNewsReadSentence(const char* text);
    void HandleListenIdleTick();
    bool IsCloudTtsSuppressed() const { return suppressed; }
};
''' + finish + "\n" + cancel + "\n" + sentence + "\n" + idle_tick + "\n" + chunk_end + r'''
int main() {
    size_t offset = 999;
    bool more = false;
    const std::string article = "第一句话很短。第二句话比较长一些。第三句话在后面。";
    auto cut = news_utf8_chunk_end(article.c_str(), 0, 55);
    assert(article.substr(0, cut) == "第一句话很短。第二句话比较长一些。");
    assert(cut <= 55 && (static_cast<unsigned char>(article[cut]) & 0xc0) != 0x80);
    assert(news_utf8_chunk_end(article.c_str(), cut, 640) == article.size());
    const std::string body = "第一句话。第二句话，今天晴朗。结束！";
    progress.Begin(body, 100, true);
    const auto first = progress.token();
    assert(first && progress.waiting());
    // Tool dispatch, delay, acknowledgements and unrelated text must not scroll.
    assert(!progress.Sentence("好的，我来朗读。", offset));
    assert(offset == 999);
    assert(!progress.Finish(first, true, more));
    assert(progress.Sentence("标题：第一句话！", offset) && offset == 100);
    assert(!progress.Sentence("第一句话！", offset)); // duplicate subtitle
    assert(!progress.Finish(first, true, more)); // still has unread body
    assert(progress.Sentence("第二句话 今天晴朗", offset));
    assert(offset == 100 + std::string("第一句话").size());
    assert(progress.Sentence("结束。", offset)); // a short final sentence is valid
    assert(!progress.Finish(first, false, more)); // text without audio
    assert(progress.Finish(first, true, more) && more);
    assert(!progress.Finish(first, true, more)); // exactly once
    assert(!progress.unfinished());

    progress.Begin("最后一段正文。", 600, false);
    auto last = progress.token();
    assert(last != first);
    assert(!progress.Finish(first, true, more)); // stale generation
    assert(progress.Sentence("最后一段正文", offset) && offset == 600);
    assert(progress.Finish(last, true, more) && !more);

    progress.Begin(body, 0, true);
    assert(!progress.Sentence("好的，继续。", offset)); // no meaningful article anchor
    assert(!progress.Finish(progress.token(), true, more));
    progress.Cancel();
    assert(progress.unfinished() && !progress.waiting());
    assert(!progress.Sentence(body, offset));
    progress.Clear();
    assert(!progress.unfinished());

    const std::string english = "ESP32 supports WiFi, and UTF-8.";
    progress.Begin(english, 42, false);
    assert(progress.Sentence("esp32 SUPPORTS wifi and utf8", offset) && offset == 42);
    assert(progress.Finish(progress.token(), true, more));

    // A changed word used to poison the gap-free match cursor for all later
    // sentences. The final sentence from the supplied log must re-sync it.
    const std::string news =
        "10月28日，合肥，大众汽车集团办了自己的第一次技术日。"
        "这场发布会的主角名为「VEGA」，名字取自织女星，"
        "是大众在中国本土自研的一体化技术矩阵。"
        "它把整车平台、电子电气架构和高级驾驶辅助系统三大核心技术打包成了一套体系。"
        "按大众的说法，它由此成为中国市场唯一一家拥有本土开发自研技术矩阵的外资车企。";
    progress.Begin(news, 0, true);
    assert(progress.Sentence("10月28日，合肥，大众汽车集团 办了自己的第一次技术日。", offset));
    assert(progress.Sentence("这场发布会的主角叫「VEGA」，名字取自织女星，", offset));
    assert(progress.Sentence("是大众在中国本土自研的一体化技术矩阵。", offset));
    assert(progress.Sentence("它把整车平台、电子电气架构和高级驾驶辅助系统三大核心技术打包成了一套体系。", offset));
    assert(progress.Sentence("按大众的说法，它由此成为中国市场唯一一家拥有本土开发自研技术矩阵的外资车企。", offset));
    assert(progress.Finish(progress.token(), true, more) && more);
    progress.Cancel(); // a user's subsequent STT must not erase a completed cursor
    assert(progress.ResumeOffset() == news.size());

    // A rewrite in the final sentence itself: recognize its substantial ending
    // even when the unchanged prefix is longer than that ending.
    progress.Begin("前半段新闻正文有很长一串相同文字，企业将于周三发布最新车型以及相关技术方案。", 0, true);
    assert(progress.Sentence("前半段新闻正文有很长一串相同文字，企业准备于周四发布最新车型以及相关技术方案。", offset));
    assert(progress.Finish(progress.token(), true, more) && more);

    const std::string interrupted = "第一句话已经播完。第二句话正在播报。第三句话还没有读。";
    progress.Begin(interrupted, 600, true);
    assert(progress.Sentence("第一句话已经播完。", offset));
    assert(progress.Sentence("第二句话正在播报。", offset));
    progress.Cancel();
    assert(progress.ResumeOffset() == 600 + std::string("第一句话已经播完").size());
    assert(progress.ResumeOffset() > 600); // pause resumes current sentence, not the chunk

    progress.Begin(interrupted, 900, true);
    assert(progress.Sentence("第一句话已经播完。", offset));
    assert(!progress.Finish(progress.token(), true, more)); // naturally stopped early
    progress.Cancel();
    assert(progress.ResumeOffset() == 900 + std::string("第一句话已经播完").size());
    assert(progress.ResumeOffset() < 900 + interrupted.size()); // no skipped suffix

    progress.Begin(interrupted, 0, true);
    assert(progress.Sentence("第一句话已经播完。", offset));
    assert(!progress.Finish(progress.token(), false, more));
    progress.Cancel();
    assert(progress.ResumeOffset() == 0); // text-only reply cannot commit audio
    progress.Begin("据报道，今天晴朗。", 0, false);
    assert(progress.Sentence("据报道", offset)); // short initial clause
    assert(progress.Sentence("今天晴朗", offset));
    assert(progress.Finish(progress.token(), true, more));
    const std::string repeated(640, 'a');
    progress.Begin(repeated, 0, false);
    assert(progress.Sentence(repeated, offset)); // bounded worst-case matching
    assert(progress.Finish(progress.token(), true, more));

    auto arm = [&]() {
        progress.Begin("这是一段新闻正文。", 0, true);
        assert(progress.Sentence("这是一段新闻正文", offset));
        current_article = true;
    };
    arm();
    Application app;
    app.news_playback_token_ = progress.token();
    app.audio_service_.count = 10;
    app.audio_service_.idle = true;
    app.FinishNewsReadPlayback();
    assert(app.requests == 0); // queue gaps before TTS stop must not continue
    app.news_tts_stopped_ = true;
    app.audio_service_.idle = false;
    app.FinishNewsReadPlayback();
    assert(app.requests == 0 && app.state == kDeviceStateSpeaking);
    app.audio_service_.idle = true;
    app.FinishNewsReadPlayback();
    assert(app.requests == 1 && app.news_read_continue_);
    app.FinishNewsReadPlayback();
    assert(app.requests == 1);

    // Three chunks require two automatic requests, with no extra user "continue".
    Application continuous;
    size_t cursor = 0;
    for (int chunk = 0; chunk < 3; ++chunk) {
        const std::string part = "这是一段新闻正文。";
        progress.Begin(part, cursor, chunk < 2);
        continuous.state = kDeviceStateListening;
        continuous.OnNewsReadSentence(part.c_str());
        assert(continuous.state == kDeviceStateSpeaking);
        for (int second = 0; second < 40; ++second) continuous.HandleListenIdleTick();
        assert(continuous.protocol.closes == 0 && continuous.audio_service_.popups == 0);
        continuous.news_playback_start_count_ = continuous.audio_service_.count;
        ++continuous.audio_service_.count;
        continuous.news_tts_stopped_ = true;
        continuous.audio_service_.idle = true;
        continuous.FinishNewsReadPlayback();
        cursor += part.size();
        assert(progress.ResumeOffset() == cursor);
        assert(continuous.requests == (chunk < 2 ? chunk + 1 : 2));
    }

    for (int scenario = 0; scenario < 5; ++scenario) {
        arm();
        Application stopped;
        stopped.news_playback_token_ = progress.token();
        stopped.news_tts_stopped_ = true;
        stopped.audio_service_.idle = true;
        stopped.audio_service_.count = scenario == 0 ? 0 : 5;
        if (scenario == 1) stopped.CancelNewsReadPlayback();
        if (scenario == 2) current_article = false;
        if (scenario == 3) stopped.protocol.open = false;
        if (scenario == 4) stopped.aborted_ = true;
        stopped.FinishNewsReadPlayback();
        assert(stopped.requests == 0);
    }

    // Actual log: auto-continue enters Listening, start is gated before tools/call,
    // then body subtitles/audio arrive without another start. Read >17 seconds.
    current_article = true;
    progress.Begin("大众汽车集团董事长兼CEO贝瑞德在台上说。", 440, true);
    Application resumed;
    resumed.state = kDeviceStateListening;
    resumed.audio_service_.idle = true;
    resumed.news_awaiting_mcp_ = true;
    resumed.OnNewsReadSentence("大众汽车集团董事长兼CEO贝瑞德在台上说。");
    assert(resumed.state == kDeviceStateListening && !resumed.news_playback_token_);
    resumed.news_awaiting_mcp_ = false; // validated tools/call
    resumed.OnNewsReadSentence("大众汽车集团董事长兼CEO贝瑞德在台上说。");
    assert(resumed.state == kDeviceStateSpeaking && resumed.news_playback_token_);
    for (int second = 0; second < 40; ++second) resumed.HandleListenIdleTick();
    assert(resumed.protocol.closes == 0 && resumed.audio_service_.popups == 0);
    ++resumed.audio_service_.count;
    resumed.news_tts_stopped_ = true;
    resumed.FinishNewsReadPlayback();
    assert(resumed.requests == 1); // still continues only after stop + actual PCM

    // Incoming audio protects Listening even before a matching subtitle/start.
    // Sampling a drained queue between packets must not count as user silence.
    Application packets;
    packets.state = kDeviceStateListening;
    packets.audio_service_.idle = true;
    for (int second = 0; second < 40; ++second) {
        ++packets.cloud_audio_rx_count_;
        packets.HandleListenIdleTick();
    }
    assert(packets.protocol.closes == 0 && packets.listen_idle_ticks_ == 0);
    packets.audio_service_.idle = false;
    for (int second = 0; second < 20; ++second) packets.HandleListenIdleTick();
    assert(packets.audio_service_.popups == 0); // buffered tail is also protected
    packets.audio_service_.idle = true;
    packets.news_reading_ = true; // stale UI flag must NOT suppress genuine idle forever
    for (int second = 0; second < 17; ++second) packets.HandleListenIdleTick();
    assert(packets.listen_idle_pending_close_ && packets.protocol.closes == 0);
    packets.HandleListenIdleTick(); // our own farewell popup does not extend the timeout
    assert(packets.protocol.closes == 1);

    // Audio arriving during the farewell cancels the pending close immediately.
    Application late_audio;
    late_audio.state = kDeviceStateListening;
    late_audio.audio_service_.idle = true;
    for (int second = 0; second < 17; ++second) late_audio.HandleListenIdleTick();
    assert(late_audio.listen_idle_pending_close_);
    ++late_audio.cloud_audio_rx_count_;
    late_audio.HandleListenIdleTick();
    assert(!late_audio.listen_idle_pending_close_ && late_audio.protocol.closes == 0);

    // Late/mismatched subtitles cannot restart a paused/disconnected/read-gated turn.
    for (int scenario = 0; scenario < 6; ++scenario) {
        progress.Begin("这是一段新闻正文。", 0, true);
        Application ignored;
        ignored.state = kDeviceStateListening;
        if (scenario == 0) ignored.aborted_ = true;
        if (scenario == 1) ignored.protocol.open = false;
        if (scenario == 2) ignored.state = kDeviceStateIdle;
        if (scenario == 3) ignored.suppressed = true;
        if (scenario == 4) ignored.news_awaiting_mcp_ = true;
        ignored.OnNewsReadSentence(scenario == 5 ? "好的，我来朗读" : "这是一段新闻正文。");
        assert(ignored.state != kDeviceStateSpeaking && !ignored.news_playback_token_);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "news_test.cc"
            binary = Path(directory) / ("news_test.exe" if os.name == "nt" else "news_test")
            source.write_text(driver, encoding="utf-8")
            subprocess.run(
                [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main/nodelio/skill"), str(source), "-o", str(binary)],
                check=True, capture_output=True, text=True, timeout=240,
            )
            subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
