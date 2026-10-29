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
        news_ui = (ROOT / "main/nodelio/skill/news_list_ui.inc").read_text(encoding="utf-8")
        chunk_end = function(news_ui, "static size_t news_utf8_chunk_end(")
        driver = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include "news_read_progress.h"
#define ESP_LOGI(...)
enum { kDeviceStateSpeaking, kDeviceStateListening, kDeviceStateIdle };
enum { kListeningModeManualStop, kListeningModeAutoStop };
NewsReadProgress progress;
bool current_article = true;
void nodelio_skill_news_cancel_read() { progress.Cancel(); }
bool nodelio_skill_news_finish_chunk(uint32_t token, bool played, bool* more) {
    return current_article && progress.Finish(token, played, *more);
}
struct Audio {
    bool idle = false;
    uint32_t count = 0;
    bool IsPlaybackIdle() const { return idle; }
    uint32_t GetPlaybackCount() const { return count; }
};
struct Protocol {
    bool open = true;
    bool IsAudioChannelOpened() const { return open; }
};
class Application {
public:
    Audio audio_service_;
    Protocol protocol;
    Protocol* protocol_ = &protocol;
    uint32_t news_playback_token_ = 0, news_playback_start_count_ = 0;
    bool news_tts_stopped_ = false, aborted_ = false, news_read_continue_ = false;
    int news_mcp_detect_retries_ = 0, state = kDeviceStateSpeaking;
    int listening_mode_ = kListeningModeAutoStop, requests = 0;
    int GetDeviceState() const { return state; }
    void SetDeviceState(int value) { state = value; }
    void SendNewsReadRequest(bool interrupt) { assert(!interrupt); ++requests; }
    void FinishNewsReadPlayback();
    void CancelNewsReadPlayback();
};
''' + finish + "\n" + cancel + "\n" + chunk_end + r'''
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
    assert(!progress.Sentence("第二句话，今天晴朗。结束", offset)); // summary skips prefix
    assert(!progress.Finish(progress.token(), true, more));
    progress.Cancel();
    assert(progress.unfinished() && !progress.waiting()); // manual resume replays chunk
    assert(!progress.Sentence(body, offset));
    progress.Clear();
    assert(!progress.unfinished());

    const std::string english = "ESP32 supports WiFi, and UTF-8.";
    progress.Begin(english, 42, false);
    assert(progress.Sentence("esp32 SUPPORTS wifi and utf8", offset) && offset == 42);
    assert(progress.Finish(progress.token(), true, more));
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
