#include "panel_jobs.h"
#include "panel_job_queue.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>

namespace {
std::atomic<uint32_t> s_view_epoch{1};
thread_local uint32_t s_result_epoch;
struct Lane {
    std::mutex mutex;
    nodelio::PanelJobQueue queue;
    bool running = false;
};
Lane s_lanes[3];
constexpr const char* kNames[] = {"nl_job_cmd", "nl_job_open", "nl_job_bg"};

void Drain(Lane& lane) {
    for (;;) {
        nodelio::PanelJob job;
        {
            std::lock_guard<std::mutex> lock(lane.mutex);
            if (!lane.queue.Pop(job)) {
                lane.running = false;
                return;
            }
        }
        if (!job.execute_when_stale && !nodelio_panel_skill_view_current(job.view_epoch)) {
            job.discard(job.argument);
            continue;
        }
        s_result_epoch = job.view_epoch;
        job.run(job.argument);
        s_result_epoch = 0;
    }
}
void Worker(void* argument) {
    Drain(*static_cast<Lane*>(argument));
    // All C++ locals have been destroyed before FreeRTOS deletes this stack.
    vTaskDeleteWithCaps(nullptr);
}
}  // namespace

uint32_t nodelio_panel_begin_skill_view(void) { return s_view_epoch.fetch_add(1) + 1; }
uint32_t nodelio_panel_skill_view_epoch(void) { return s_view_epoch.load(); }
uint32_t nodelio_panel_job_result_epoch(void) { return s_result_epoch; }
bool nodelio_panel_skill_view_current(uint32_t epoch) {
    return !epoch || epoch == s_view_epoch.load();
}

bool nodelio::SubmitPanelJob(PanelJobLane kind, void (*run)(void*), void* argument,
                             void (*discard)(void*), uint32_t epoch, const char* key,
                             bool execute_when_stale) {
    const unsigned index = static_cast<unsigned>(kind);
    if (index >= 3 || !run || !discard || (key && std::strlen(key) >= sizeof(PanelJob::key))) {
        return false;
    }
    PanelJob job{run, discard, argument, epoch, execute_when_stale, {}};
    if (key) {
        std::strcpy(job.key, key);
    }
    PanelJob replaced;
    auto& lane = s_lanes[index];
    {
        std::lock_guard<std::mutex> lock(lane.mutex);
        if (!lane.queue.Push(job, replaced)) {
            return false;
        }
        if (!lane.running) {
            lane.running = true;
            // ESP-IDF stack depth is BYTES (not upstream FreeRTOS words).
            if (xTaskCreateWithCaps(Worker, kNames[index], 16384, &lane, 3, nullptr,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
                lane.running = false;
                PanelJob failed;
                lane.queue.Pop(failed);  // Idle lanes were empty before this submission.
                return false;
            }
        }
    }
    if (replaced.argument) {
        replaced.discard(replaced.argument);
    }
    return true;
}
