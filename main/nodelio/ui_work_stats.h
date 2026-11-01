#pragma once

#include <esp_log.h>
#include <esp_timer.h>

// Event-driven diagnostics: no polling, allocations or forced redraws. These measure
// UI mutation time, not panel scanout or touch-to-photon latency.
class NlUiWorkScope {
public:
    explicit NlUiWorkScope(const char* operation)
        : operation_(operation), started_(esp_timer_get_time()) {}
    ~NlUiWorkScope() {
        const int64_t elapsed = esp_timer_get_time() - started_;
        if (elapsed >= 20000) {
            ESP_LOGW("nl_ui_perf", "%s mutation_us=%lld", operation_,
                     static_cast<long long>(elapsed));
        } else {
            ESP_LOGD("nl_ui_perf", "%s mutation_us=%lld", operation_,
                     static_cast<long long>(elapsed));
        }
    }

private:
    const char* operation_;
    int64_t started_;
};
