#include "overlay_priv.h"

#include "application.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "board_info.h"
#include "display/display.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "panel_jobs.h"
#include "ui_widgets.h"
#include "wifi_provision.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

static const char *TAG = "nodelio_ov";

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
    if (codec && codec->output_channels() > 1) {
        Label(body, "分别试听左右扬声器。确认两只有声后，再到「显示与声音」选择输出。");
        ui_make_text_btn(body, "试听左扬声器", 2, [](lv_event_t*) {
            Application::GetInstance().Schedule([]() {
                auto* c = Board::GetInstance().GetAudioCodec();
                if (!c) {
                    return;
                }
                c->SetOutputRoute(1);
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                nodelio_panel_ui_toast_defer("仅左声道 · 可在显示与声音改回");
            });
        });
        ui_make_text_btn(body, "试听右扬声器", 2, [](lv_event_t*) {
            Application::GetInstance().Schedule([]() {
                auto* c = Board::GetInstance().GetAudioCodec();
                if (!c) {
                    return;
                }
                c->SetOutputRoute(2);
                Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
                nodelio_panel_ui_toast_defer("仅右声道 · 可在显示与声音改回");
            });
        });
    }
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

struct shot_job {
    std::string *jpeg;
    nodelio_nvs_config_t cfg;
};

static void capture_and_upload_screenshot(void)
{
    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
        nl_ov_toast("网络未就绪");
        return;
    }
    Display *disp = Board::GetInstance().GetDisplay();
    if (!disp) {
        nl_ov_toast("未检测到显示屏");
        return;
    }
    nodelio_nvs_config_t shot_cfg{};
    if (!nl_ov_cfg_http_ready(&shot_cfg, true)) {
        return;
    }
    const size_t dram_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t dma_before = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    /* WiFi 發送要一塊能放下 MSS 的 DMA。內部堆還有 7KB 也不代表 DMA 夠。 */
    if (dma_before < 1536) {
        ESP_LOGW(TAG, "screenshot skip: dma_largest=%u dram_largest=%u", (unsigned)dma_before,
                 (unsigned)dram_before);
        nl_ov_toast("内存过碎，稍后再截");
        return;
    }
    /* JPEG 在 LVGL 线程生成；HTTP 必须扔后台，否则堵 UI / 撑爆 LVGL 栈 */
    auto *jpeg = new (std::nothrow) std::string();
    if (!jpeg) {
        nl_ov_toast("内存不足");
        return;
    }
    /* 碎片严重时压质量，减小编码峰值与上传体积 */
    const int quality = (dram_before < 20 * 1024) ? 40 : ((dram_before < 40 * 1024) ? 55 : 70);
    lv_obj_t *snap = lv_layer_top();
    if (!disp->SnapshotToJpeg(*jpeg, quality, snap) || jpeg->empty()) {
        ESP_LOGW(TAG, "SnapshotToJpeg failed q=%d dram=%u", quality, (unsigned)dram_before);
        delete jpeg;
        nl_ov_toast("截屏生成失败");
        return;
    }
    ESP_LOGI(TAG, "screenshot jpeg=%u q=%d dram_free_largest=%u", (unsigned)jpeg->size(), quality,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    auto *job = static_cast<shot_job *>(heap_caps_malloc(sizeof(shot_job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job) {
        delete jpeg;
        nl_ov_toast("无法上传");
        return;
    }
    job->jpeg = jpeg;
    job->cfg = shot_cfg;
    nl_ov_toast("正在上传截图…");
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto *job = static_cast<shot_job *>(p);
                /* 把 JPEG 拷到 PSRAM，释放 string 可能占的内部堆，给 lwIP TCP 缓冲腾地方 */
                const size_t n = job->jpeg->size();
                uint8_t *ps =
                    static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (ps) {
                    memcpy(ps, job->jpeg->data(), n);
                    delete job->jpeg;
                    job->jpeg = nullptr;
                }
                const void *body = ps ? static_cast<const void *>(ps) : job->jpeg->data();
                const size_t body_n = ps ? n : job->jpeg->size();

                char url[256];
                const char *custom = job->cfg.screenshot_url;
                while (*custom == ' ' || *custom == '\t') custom++;
                if (*custom != '\0') {
                    if (strstr(custom, "://")) {
                        if (strchr(strstr(custom, "://") + 3, '/')) {
                            std::snprintf(url, sizeof(url), "%s", custom);
                        } else {
                            std::snprintf(url, sizeof(url), "%s/api/devices/%s/panel/screenshot", custom, job->cfg.device_id);
                        }
                    } else {
                        char host_port[sizeof(job->cfg.screenshot_url) + 8];
                        if (strchr(custom, ':')) {
                            std::snprintf(host_port, sizeof(host_port), "%s", custom);
                        } else {
                            std::snprintf(host_port, sizeof(host_port), "%s:8899", custom);
                        }
                        std::snprintf(url, sizeof(url), "http://%s/api/devices/%s/panel/screenshot", host_port, job->cfg.device_id);
                    }
                } else {
                    char base[160];
                    nl_url_strip_base(job->cfg.server_url, base, sizeof(base));
                    std::snprintf(url, sizeof(url), "%s/api/devices/%s/panel/screenshot", base, job->cfg.device_id);
                }
                int http_status = 0;
                esp_err_t err = nodelio_http_post_image(url, job->cfg.device_key, body, body_n, &http_status);
                if (err == ESP_OK && http_status >= 200 && http_status < 300) {
                    ESP_LOGI(TAG, "Screenshot uploaded successfully: %u bytes", (unsigned)body_n);
                    /* P3c：多屏时提示当前屏序号，便于各拍一帧 */
                    const int pi = nl_ov_home_user_page_index();
                    const int pn = nl_ov_home_user_page_count();
                    if (pi >= 0 && pn > 1) {
                        char msg[40];
                        std::snprintf(msg, sizeof(msg), "已上传 · 屏%d/%d", pi + 1, pn);
                        nodelio_panel_ui_toast_defer(msg);
                    } else {
                        nodelio_panel_ui_toast_defer("已上传实机截图");
                    }
                } else {
                    ESP_LOGW(TAG, "Screenshot upload failed err=%s http=%d jpeg=%u dram=%u",
                             esp_err_to_name(err), http_status, (unsigned)body_n,
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                    if (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE) {
                        nodelio_panel_ui_toast_defer("内存/网络忙，稍后再截");
                    } else {
                        nodelio_panel_ui_toast_defer("截图上传失败");
                    }
                }
                if (ps) {
                    heap_caps_free(ps);
                }
                if (job->jpeg) {
                    delete job->jpeg;
                }
                heap_caps_free(job);
            },
            job,
            [](void* p) {
                auto* a = static_cast<shot_job*>(p);
                delete a->jpeg;
                free(a);
            },
            0, "screenshot")) {
        if (job->jpeg) {
            delete job->jpeg;
        }
        heap_caps_free(job);
        nl_ov_toast("无法上传");
    }
}

void nl_ov_trigger_screenshot(void)
{
    static uint32_t last_click_ms = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_click_ms < 1200) {
        return;
    }
    last_click_ms = now;

    bool shot_fab_hidden_temp = false;
    if (s_shot_fab && !lv_obj_has_flag(s_shot_fab, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_shot_fab, LV_OBJ_FLAG_HIDDEN);
        shot_fab_hidden_temp = true;
        lv_refr_now(nullptr);
    }

    capture_and_upload_screenshot();

    if (shot_fab_hidden_temp && s_shot_fab && s_cfg.screenshot_fab_en) {
        lv_obj_clear_flag(s_shot_fab, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_shot_fab);
    }
}

void nl_ov_on_brand_click(lv_event_t *e)
{
    (void)e;
    nl_ov_trigger_screenshot();
}
