#include "wake_dram.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "nl_wdrm"

#if CONFIG_BOARD_TYPE_SILA_86 && CONFIG_NODELIO_PANEL

static constexpr size_t kWakeDramPocket = 12 * 1024;
/* 一张 TCP MSS 量级；常驻不随唤醒释放，避免碎片后 12KB 再也占不回 */
static constexpr size_t kShotDmaPocket = 2 * 1024;

static void *s_wake_pocket = nullptr;
static size_t s_wake_bytes = 0;
static void *s_shot_pocket = nullptr;
static size_t s_shot_bytes = 0;

static bool reserve_wake(void)
{
    if (s_wake_pocket) {
        return true;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < kWakeDramPocket) {
        ESP_LOGW(TAG, "wake reserve skip largest=%u need=%u", (unsigned)largest,
                 (unsigned)kWakeDramPocket);
        return false;
    }
    s_wake_pocket = heap_caps_malloc(kWakeDramPocket, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_wake_pocket) {
        ESP_LOGW(TAG, "wake reserve alloc fail");
        return false;
    }
    s_wake_bytes = kWakeDramPocket;
    ESP_LOGI(TAG, "wake reserved %u INTERNAL (largest was %u)", (unsigned)s_wake_bytes,
             (unsigned)largest);
    return true;
}

static bool reserve_shot(void)
{
    if (s_shot_pocket) {
        return true;
    }
    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < kShotDmaPocket) {
        ESP_LOGW(TAG, "shot reserve skip largest=%u need=%u", (unsigned)largest,
                 (unsigned)kShotDmaPocket);
        return false;
    }
    s_shot_pocket =
        heap_caps_malloc(kShotDmaPocket, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_shot_pocket) {
        ESP_LOGW(TAG, "shot reserve alloc fail");
        return false;
    }
    s_shot_bytes = kShotDmaPocket;
    ESP_LOGI(TAG, "shot reserved %u DMA (largest was %u)", (unsigned)s_shot_bytes,
             (unsigned)largest);
    return true;
}

bool nodelio_wake_dram_reserve(void)
{
    const bool wake_ok = reserve_wake();
    const bool shot_ok = reserve_shot();
    return wake_ok || shot_ok;
}

size_t nodelio_wake_dram_release_for_arm(void)
{
    if (!s_wake_pocket) {
        return 0;
    }
    const size_t n = s_wake_bytes;
    heap_caps_free(s_wake_pocket);
    s_wake_pocket = nullptr;
    s_wake_bytes = 0;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "wake released %u → largest=%u", (unsigned)n, (unsigned)largest);
    return n;
}

bool nodelio_wake_dram_try_reclaim(void)
{
    if (s_wake_pocket) {
        return true;
    }
    return reserve_wake();
}

bool nodelio_wake_dram_held(void)
{
    return s_wake_pocket != nullptr;
}

size_t nodelio_shot_dma_release(void)
{
    if (!s_shot_pocket) {
        return 0;
    }
    const size_t n = s_shot_bytes;
    heap_caps_free(s_shot_pocket);
    s_shot_pocket = nullptr;
    s_shot_bytes = 0;
    const size_t dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "shot released %u → dma_largest=%u", (unsigned)n, (unsigned)dma);
    return n;
}

bool nodelio_shot_dma_try_reclaim(void)
{
    if (s_shot_pocket) {
        return true;
    }
    return reserve_shot();
}

bool nodelio_shot_dma_held(void)
{
    return s_shot_pocket != nullptr;
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
size_t nodelio_shot_dma_release(void)
{
    return 0;
}
bool nodelio_shot_dma_try_reclaim(void)
{
    return false;
}
bool nodelio_shot_dma_held(void)
{
    return false;
}

#endif
