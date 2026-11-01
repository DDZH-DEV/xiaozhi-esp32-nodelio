#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Zero is an unscoped result (e.g. a voice command). UI navigation does not cancel
// playback/control; opening another skill or explicitly closing it cancels presentation.
uint32_t nodelio_panel_begin_skill_view(void);
uint32_t nodelio_panel_skill_view_epoch(void);
uint32_t nodelio_panel_job_result_epoch(void);
bool nodelio_panel_skill_view_current(uint32_t epoch);
#ifdef __cplusplus
}

namespace nodelio {
enum class PanelJobLane { Control, Interactive, Background };

// The callback owns argument on execution and must return (never delete its task).
// discard owns argument only if accepted work is replaced/canceled before execution.
// On false, the caller retains argument. No queue waits or internal-stack fallback.
bool SubmitPanelJob(PanelJobLane lane, void (*run)(void*), void* argument, void (*discard)(void*),
                    uint32_t view_epoch = 0, const char* key = nullptr,
                    bool execute_when_stale = false);
}  // namespace nodelio
#endif
