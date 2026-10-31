#include "overlay_priv.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "board_info.h"
#include "ui_widgets.h"

#include <new>

namespace {
struct Diagnostics {
    lv_timer_t* timer = nullptr;
    lv_obj_t* status = nullptr;
    lv_obj_t* touch = nullptr;
    lv_obj_t* meters[2] = {};
    lv_obj_t* buttons[2] = {};
    uint32_t frames = 0;
};

lv_obj_t* Label(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    nl_ov_set_font(label);
    lv_obj_set_style_text_color(label, UI_TEXT_SUB, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

void Record(lv_event_t* event) {
    const unsigned channel =
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    Application::GetInstance().Schedule([channel]() {
        auto& app = Application::GetInstance();
        if (!app.StartPanelAudioTest(channel)) {
            nodelio_panel_ui_toast_defer("请先结束对话、播放或激活，再开始测试");
        }
    });
}
}  // namespace

void nl_ov_build_diagnostics(lv_obj_t* body) {
    auto* state = new (std::nothrow) Diagnostics;
    if (!state) {
        Label(body, "内存不足，无法打开自检页");
        return;
    }
    lv_obj_add_event_cb(
        body,
        [](lv_event_t* event) {
            auto* state = static_cast<Diagnostics*>(lv_event_get_user_data(event));
            if (state->timer) {
                lv_timer_delete(state->timer);
            }
            Application::GetInstance().Schedule(
                []() { Application::GetInstance().CancelPanelAudioTest(); });
            delete state;
        },
        LV_EVENT_DELETE, state);

    Label(body, "点击并拖动触摸区，检查坐标是否随手指移动；息屏后首次触摸只唤醒屏幕。");
    auto* pad = lv_obj_create(body);
    lv_obj_set_width(pad, lv_pct(100));
    lv_obj_set_height(pad, nodelio_display_profile()->height / 3);
    lv_obj_set_style_bg_color(pad, UI_PANEL, 0);
    lv_obj_set_style_border_color(pad, UI_ACCENT, 0);
    lv_obj_set_style_radius(pad, UI_RADIUS, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
    state->touch = Label(pad, "触摸区：等待按下");
    lv_obj_center(state->touch);
    lv_obj_add_event_cb(
        pad,
        [](lv_event_t* event) {
            auto* state = static_cast<Diagnostics*>(lv_event_get_user_data(event));
            auto* indev = lv_event_get_indev(event);
            if (!indev) {
                return;
            }
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            lv_label_set_text_fmt(state->touch, "X: %d   Y: %d", (int)point.x, (int)point.y);
            Board::GetInstance().NoteUserActivity();
        },
        LV_EVENT_PRESSING, state);

    auto* codec = Board::GetInstance().GetAudioCodec();
    const int microphones =
        codec ? codec->input_channels() - (codec->input_reference() ? 1 : 0) : 0;
    Label(body, "选择麦克风，录音 5 秒后通过扬声器回放。只在本机处理，不上传。");
    if (codec && codec->input_reference()) {
        Label(body, "播放参考通道不作为麦克风测试；此页只显示实际配置的麦克风通道。");
    }
    state->status = Label(body, "等待测试（需处于待机或配网状态）");
    for (unsigned i = 0; i < 2; ++i) {
        if (static_cast<int>(i) >= microphones) {
            break;
        }
        state->meters[i] = Label(body, i ? "MIC 2：0" : "MIC 1：0");
        state->buttons[i] =
            ui_make_text_btn(body, i ? "录音 / 回放 MIC 2" : "录音 / 回放 MIC 1", 2, nullptr);
        lv_obj_add_event_cb(state->buttons[i], Record, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    }
    ui_make_text_btn(body, "停止测试", 2, [](lv_event_t*) {
        Application::GetInstance().Schedule(
            []() { Application::GetInstance().CancelPanelAudioTest(); });
    });
    state->timer = lv_timer_create(
        [](lv_timer_t* timer) {
            if (!nodelio_panel_ui_screen_on()) {
                lv_timer_set_period(timer, 1000);
                return;
            }
            lv_timer_set_period(timer, 250);
            auto* state = static_cast<Diagnostics*>(lv_timer_get_user_data(timer));
            auto& app = Application::GetInstance();
            auto& audio = app.GetAudioService();
            const uint32_t frames = audio.GetAudioTestFrames();
            const bool capturing = audio.IsAudioTesting();
            const bool testing = app.GetDeviceState() == kDeviceStateAudioTesting;
            const uint32_t levels =
                capturing && frames != state->frames ? audio.GetAudioTestLevels() : 0;
            state->frames = frames;
            for (unsigned i = 0; i < 2; ++i) {
                if (state->meters[i]) {
                    lv_label_set_text_fmt(state->meters[i], "MIC %u：峰值 %u / 32768", i + 1,
                                          (unsigned)((levels >> (i * 16)) & 0xffff));
                    ui_set_enabled(state->buttons[i], !testing);
                }
            }
            lv_label_set_text_fmt(
                state->status, "%s · 已采集 %lu 帧",
                capturing ? "正在录音" : (testing ? "正在回放" : "等待测试 / 测试已结束"),
                (unsigned long)frames);
        },
        250, state);
}
