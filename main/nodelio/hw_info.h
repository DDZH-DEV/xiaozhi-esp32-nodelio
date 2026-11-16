#pragma once

#include <cJSON.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build config.hw object for device registration (APP「硬件信息」).
 * Fields match Arduino nodelio_hw_info.h; failed optional fields are omitted.
 * Caller owns the returned object (or NULL on OOM).
 */
cJSON *nodelio_hw_info_build(const char *module, const char *fw);

#ifdef __cplusplus
}
#endif
