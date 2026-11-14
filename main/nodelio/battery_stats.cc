#include "battery_stats.h"

#include "board.h"
#include "nvs_config.h"
#include "panel_ui.h"
#include "wifi_provision.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

#define TAG "nl_batt"

namespace {

constexpr int kNomMah = 2000;
constexpr float kEff = 0.85f;
constexpr float kEwmaAlpha = 0.3f;
constexpr int64_t kMinWinUs = 20LL * 60 * 1000 * 1000; /* 20 min */
constexpr int kMinDeltaPct = 2;
constexpr int64_t kSampleOnUs = 60LL * 1000 * 1000;
constexpr int64_t kSampleOffUs = 10LL * 60 * 1000 * 1000; /* 10 min asleep */

SemaphoreHandle_t s_mu;
bool s_inited;

int s_pct = -1;
bool s_charging = false;
bool s_discharging = false;
bool s_present = false;

bool s_win_open = false;
int s_win_pct = 0;
int64_t s_win_us = 0;

bool s_ready = false;
float s_i_ewma = 0.f;
int64_t s_last_sample_us = 0;

void lock()
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

void unlock()
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

void close_window()
{
    s_win_open = false;
}

void on_discharge_sample(int pct, int64_t now)
{
    if (!s_win_open) {
        s_win_open = true;
        s_win_pct = pct;
        s_win_us = now;
        return;
    }
    const int d_pct = s_win_pct - pct;
    const int64_t d_us = now - s_win_us;
    if (d_pct < kMinDeltaPct || d_us < kMinWinUs) {
        return;
    }
    const float hours = static_cast<float>(d_us) / (3600.f * 1e6f);
    if (hours < 0.01f) {
        return;
    }
    const float i = static_cast<float>(kNomMah) * (static_cast<float>(d_pct) / 100.f) / hours;
    if (i < 1.f || i > 2000.f) {
        /* 异常跳变：重置窗口 */
        s_win_pct = pct;
        s_win_us = now;
        return;
    }
    if (!s_ready) {
        s_i_ewma = i;
        s_ready = true;
    } else {
        s_i_ewma = kEwmaAlpha * i + (1.f - kEwmaAlpha) * s_i_ewma;
    }
    ESP_LOGI(TAG, "I_sample=%.0fmA I_ewma=%.0fmA dSoC=%d dt=%.2fh", (double)i, (double)s_i_ewma,
             d_pct, (double)hours);
    s_win_pct = pct;
    s_win_us = now;
}

}  // namespace

extern "C" void nodelio_battery_stats_init(void)
{
    if (s_inited) {
        return;
    }
    s_mu = xSemaphoreCreateMutex();
    s_inited = true;
}

extern "C" void nodelio_battery_stats_tick(void)
{
    if (!s_inited) {
        nodelio_battery_stats_init();
    }
    const int64_t now = esp_timer_get_time();
    const bool screen_on = nodelio_panel_ui_screen_on();
    const int64_t period = screen_on ? kSampleOnUs : kSampleOffUs;
    lock();
    const int64_t last = s_last_sample_us;
    unlock();
    if (last != 0 && (now - last) < period) {
        return;
    }

    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        lock();
        s_present = false;
        s_pct = -1;
        s_charging = false;
        s_discharging = false;
        close_window();
        s_last_sample_us = now;
        unlock();
        return;
    }

    lock();
    s_present = true;
    s_pct = level;
    s_charging = charging;
    s_discharging = discharging;
    s_last_sample_us = now;
    if (charging) {
        close_window();
    } else if (discharging) {
        on_discharge_sample(level, now);
    } else {
        /* idle（既不充也不放）：保持窗口但不推进估流 */
    }
    unlock();
}

extern "C" void nodelio_battery_stats_get_summary(nodelio_battery_summary_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->pct = -1;
    out->remain_min = -1;
    if (!s_inited) {
        nodelio_battery_stats_init();
    }
    lock();
    out->pct = s_present ? s_pct : -1;
    out->charging = s_charging;
    out->ready = s_ready && s_i_ewma > 1.f;
    out->i_avg_ma = out->ready ? static_cast<int>(s_i_ewma + 0.5f) : 0;
    if (out->ready && s_present && !s_charging && s_pct >= 0) {
        const float rem_mah = (static_cast<float>(s_pct) / 100.f) * (static_cast<float>(kNomMah) * kEff);
        out->remain_min = static_cast<int>((rem_mah / s_i_ewma) * 60.f + 0.5f);
        if (out->remain_min < 0) {
            out->remain_min = 0;
        }
    } else {
        out->remain_min = -1;
    }
    unlock();
    out->screen_on = nodelio_panel_ui_screen_on();
    out->wake_on_sleep = nodelio_wake_on_sleep_enabled();
    out->net_asleep = nodelio_wifi_is_asleep();
}

extern "C" int nodelio_battery_stats_format_json(char *buf, size_t len)
{
    if (!buf || len < 32) {
        return -1;
    }
    nodelio_battery_summary_t s;
    nodelio_battery_stats_get_summary(&s);
    if (s.pct < 0) {
        return snprintf(buf, len, "{\"pct\":-1,\"charging\":false,\"ready\":false}");
    }
    return snprintf(buf, len,
                    "{\"pct\":%d,\"charging\":%s,\"i_avg_ma\":%d,\"remain_min\":%d,"
                    "\"wake_on_sleep\":%s,\"screen_on\":%s,\"net_asleep\":%s,\"ready\":%s}",
                    s.pct, s.charging ? "true" : "false", s.i_avg_ma, s.remain_min,
                    s.wake_on_sleep ? "true" : "false", s.screen_on ? "true" : "false",
                    s.net_asleep ? "true" : "false", s.ready ? "true" : "false");
}
