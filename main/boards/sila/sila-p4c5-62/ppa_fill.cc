#include "ppa_fill.h"
#include <sdkconfig.h>

#if CONFIG_NODELIO_P4_PPA_FILL
#include <driver/ppa.h>
#include <esp_cache.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <lvgl.h>
#include <lvgl_private.h>
#include <src/draw/sw/lv_draw_sw.h>

// This draw-unit adapter uses the pinned 9.5 ABI. Re-audit when migrating fonts
// and LVGL together; do not silently enable the vendor 9.5 PPA cache handlers.
static_assert(LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5);
static_assert(LV_USE_PPA == 0 && LV_USE_DRAW_SW && LV_DRAW_SW_DRAW_UNIT_CNT == 1);
static_assert(LV_USE_OS == LV_OS_NONE && !LV_DRAW_TRANSFORM_USE_MATRIX);
static_assert(LV_DRAW_BUF_ALIGN == CONFIG_CACHE_L2_CACHE_LINE_SIZE);

namespace {
constexpr uint8_t kUnitId = 81;
constexpr const char* kTag = "SilaPpaFill";
struct FillUnit {
    lv_draw_unit_t base;
    ppa_client_handle_t client;
    SilaPpaFillStats stats;
    bool disabled;
};
FillUnit* s_unit = nullptr;

bool SuitableBuffer(const lv_draw_buf_t* buffer) {
    // Only borrow whole cache lines owned by this buffer. Other layers (including
    // unaligned/small/translucent layers) remain entirely on the software unit.
    constexpr size_t line = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    return buffer && buffer->data && buffer->data_size && buffer->header.w && buffer->header.h &&
           esp_ptr_external_ram(buffer->data) &&
           reinterpret_cast<uintptr_t>(buffer->data) % line == 0 && buffer->data_size % line == 0 &&
           buffer->header.cf == LV_COLOR_FORMAT_RGB888 &&
           buffer->header.stride == buffer->header.w * 3 &&
           buffer->data_size >= static_cast<uint64_t>(buffer->header.stride) * buffer->header.h;
}

int32_t Evaluate(lv_draw_unit_t* base, lv_draw_task_t* task) {
    auto* unit = reinterpret_cast<FillUnit*>(base);
    if (unit->disabled || task->type != LV_DRAW_TASK_TYPE_FILL || task->opa != LV_OPA_COVER ||
        !SuitableBuffer(task->target_layer->draw_buf)) {
        return 0;
    }
    auto* fill = static_cast<lv_draw_fill_dsc_t*>(task->draw_dsc);
    if (fill->radius || fill->grad.dir != LV_GRAD_DIR_NONE || fill->opa != LV_OPA_COVER ||
        lv_area_get_size(&task->area) < 4096) {
        return 0;
    }
    if (task->preference_score > 70) {
        task->preference_score = 70;
        task->preferred_draw_unit_id = kUnitId;
    }
    return 1;
}

int32_t Dispatch(lv_draw_unit_t* base, lv_layer_t* layer) {
    auto* unit = reinterpret_cast<FillUnit*>(base);
    lv_draw_task_t* task = lv_draw_get_available_task(layer, nullptr, kUnitId);
    if (!task || task->preferred_draw_unit_id != kUnitId) {
        return LV_DRAW_UNIT_IDLE;
    }
    auto* fill = static_cast<lv_draw_fill_dsc_t*>(task->draw_dsc);
    lv_draw_buf_t* buffer = layer->draw_buf;
    lv_area_t area;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    task->draw_unit = base;
    if (lv_area_intersect(&area, &task->area, &task->clip_area) &&
        lv_area_intersect(&area, &area, &layer->buf_area)) {
        bool done = false;
        if (!unit->disabled && SuitableBuffer(buffer)) {
            // One synchronous LVGL draw context: clean CPU writes before DMA;
            // discard CPU cache after DMA. Never invalidate a neighbour's bytes.
            esp_err_t err =
                esp_cache_msync(buffer->data, buffer->data_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
            if (err == ESP_OK) {
                ppa_fill_oper_config_t op = {};
                op.out.buffer = buffer->data;
                op.out.buffer_size = buffer->data_size;
                op.out.pic_w = buffer->header.w;
                op.out.pic_h = buffer->header.h;
                op.out.block_offset_x = area.x1 - layer->buf_area.x1;
                op.out.block_offset_y = area.y1 - layer->buf_area.y1;
                op.out.fill_cm = PPA_FILL_COLOR_MODE_RGB888;
                op.fill_block_w = lv_area_get_width(&area);
                op.fill_block_h = lv_area_get_height(&area);
                op.fill_argb_color.val = lv_color_to_u32(fill->color);
                op.mode = PPA_TRANS_MODE_BLOCKING;
                err = ppa_do_fill(unit->client, &op);
                // Also invalidate after a failed transaction before CPU fallback.
                const esp_err_t sync =
                    esp_cache_msync(buffer->data, buffer->data_size,
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
                if (err == ESP_OK)
                    err = sync;
            }
            done = err == ESP_OK;
            if (!done) {
                unit->disabled = true;
                ESP_LOGW(kTag, "Fill disabled after error %s; using software",
                         esp_err_to_name(err));
            }
        }
        if (done) {
            ++unit->stats.completed;
        } else {
            lv_draw_sw_fill(task, fill, &area);
            ++unit->stats.fallback;
        }
    }
    task->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

int32_t Delete(lv_draw_unit_t* base) {
    auto* unit = reinterpret_cast<FillUnit*>(base);
    ppa_unregister_client(unit->client);
    s_unit = nullptr;
    return 0;  // LVGL frees the draw unit.
}
}  // namespace

bool SilaPpaFillInit() {
    if (s_unit)
        return !s_unit->disabled;
    ppa_client_config_t config = {};
    config.oper_type = PPA_OPERATION_FILL;
    config.max_pending_trans_num = 1;
    ppa_client_handle_t client = nullptr;
    const esp_err_t err = ppa_register_client(&config, &client);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "PPA unavailable (%s); using software", esp_err_to_name(err));
        return false;
    }
    s_unit = static_cast<FillUnit*>(lv_draw_create_unit(sizeof(FillUnit)));
    if (!s_unit) {
        ppa_unregister_client(client);
        return false;
    }
    s_unit->client = client;
    s_unit->base.name = "SILA_PPA_FILL";
    s_unit->base.evaluate_cb = Evaluate;
    s_unit->base.dispatch_cb = Dispatch;
    s_unit->base.delete_cb = Delete;
    return true;
}

SilaPpaFillStats SilaPpaFillGetStats() { return s_unit ? s_unit->stats : SilaPpaFillStats{}; }
#else
bool SilaPpaFillInit() { return false; }
SilaPpaFillStats SilaPpaFillGetStats() { return {}; }
#endif
