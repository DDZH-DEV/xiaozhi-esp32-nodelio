#include "wake_dram.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "nl_wdrm"

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL

static constexpr size_t kWakeDramPocket = 12 * 1024;

static void *s_pocket = nullptr;
static size_t s_pocket_bytes = 0;

bool nodelio_wake_dram_reserve(void)
{
    if (s_pocket) {
        return true;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < kWakeDramPocket) {
        ESP_LOGW(TAG, "reserve skip largest=%u need=%u", (unsigned)largest,
                 (unsigned)kWakeDramPocket);
        return false;
    }
    s_pocket = heap_caps_malloc(kWakeDramPocket, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_pocket) {
        ESP_LOGW(TAG, "reserve alloc fail");
        return false;
    }
    s_pocket_bytes = kWakeDramPocket;
    ESP_LOGI(TAG, "reserved %u INTERNAL (largest was %u)", (unsigned)s_pocket_bytes,
             (unsigned)largest);
    return true;
}

size_t nodelio_wake_dram_release_for_arm(void)
{
    if (!s_pocket) {
        return 0;
    }
    const size_t n = s_pocket_bytes;
    heap_caps_free(s_pocket);
    s_pocket = nullptr;
    s_pocket_bytes = 0;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "released %u for wake arm → largest=%u", (unsigned)n, (unsigned)largest);
    return n;
}

bool nodelio_wake_dram_try_reclaim(void)
{
    if (s_pocket) {
        return true;
    }
    return nodelio_wake_dram_reserve();
}

bool nodelio_wake_dram_held(void)
{
    return s_pocket != nullptr;
}

#else

bool nodelio_wake_dram_reserve(void)
{
    return false;
}
size_t nodelio_wake_dram_release_for_arm(void)
{
    return 0;
}
bool nodelio_wake_dram_try_reclaim(void)
{
    return false;
}
bool nodelio_wake_dram_held(void)
{
    return false;
}

#endif
