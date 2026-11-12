/*
 * IDF 6.1 的 DPI 面板没有 disp_on_off。这里按 esp_lcd_panel_dpi.c 的
 * dpi_panel_init / dpi_panel_del 逆序停扫描：换掉会自行重启的 DMA 回调，
 * 关掉 video / DPI 输出 / underrun，再关 DPI 时钟并放开 CPU_FREQ_MAX。
 * 结构体布局必须与 IDF 6.1 components/esp_lcd/dsi/esp_lcd_panel_dpi.c 一致。
 */
#include "p4_dpi_gate.h"

#include "esp_lcd_panel_interface.h"
#include "esp_lcd_mipi_dsi.h"
#include "mipi_dsi_priv.h"
#include "esp_private/dw_gdma.h"
#include "hal/color_hal.h"

#undef TAG
static const char *TAG = "p4.dpi";

struct esp_lcd_dpi_panel_t {
    esp_lcd_panel_t base;
    esp_lcd_dsi_bus_handle_t bus;
    uint8_t virtual_channel;
    uint8_t cur_fb_index;
    uint8_t num_fbs;
    uint8_t *fbs[DPI_PANEL_MAX_FB_NUM];
    uint32_t h_pixels;
    uint32_t v_pixels;
    size_t fb_size;
    size_t bits_per_pixel;
    soc_module_clk_t clk_src;
    lcd_color_format_t in_color_format;
    lcd_color_format_t out_color_format;
    dw_gdma_channel_handle_t dma_chan;
    intr_handle_t brg_intr;
    dw_gdma_link_list_handle_t link_lists[DPI_PANEL_MAX_FB_NUM];
    esp_lcd_panel_draw_bitmap_hook_t draw_bitmap_hook;
    void *hook_ctx;
    bool (*on_hook_end)(esp_lcd_panel_handle_t panel);
    void *fbcpy_handle;
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t pm_lock;
#endif
    esp_lcd_dpi_panel_color_trans_done_cb_t on_color_trans_done;
    esp_lcd_dpi_panel_frame_buf_complete_cb_t on_frame_buf_complete;
    esp_lcd_dpi_panel_vsync_cb_t on_vsync;
    void *user_ctx;
};

extern bool mipi_dsi_dma_trans_done_cb(dw_gdma_channel_handle_t chan,
                                       const dw_gdma_trans_done_event_data_t *event_data,
                                       void *user_data);

static bool s_scan_on = true;
static volatile bool s_dma_idle = false;

static bool IRAM_ATTR p4_dpi_dma_hold(dw_gdma_channel_handle_t chan,
                                      const dw_gdma_trans_done_event_data_t *event_data,
                                      void *user_data) {
    (void)chan;
    (void)event_data;
    (void)user_data;
    /* 这一帧已经完整送完，不要再启动下一帧。链表长度保持整帧。 */
    s_dma_idle = true;
    return false;
}

static bool p4_dpi_layout_ok(const struct esp_lcd_dpi_panel_t *dpi) {
    if (dpi->bus == NULL || dpi->dma_chan == NULL) {
        return false;
    }
    if (dpi->num_fbs == 0 || dpi->num_fbs > DPI_PANEL_MAX_FB_NUM) {
        return false;
    }
    if (dpi->bus->bus_id < 0 || dpi->bus->bus_id > 1) {
        return false;
    }
    if (dpi->cur_fb_index >= dpi->num_fbs || dpi->link_lists[dpi->cur_fb_index] == NULL) {
        return false;
    }
    return true;
}

static void p4_dpi_restart_dma(struct esp_lcd_dpi_panel_t *dpi) {
    /* 停扫描时已等整帧结束，这里只恢复驱动自己的完成回调并重新打开通道。
     * 不要重写链表：运行中改描述符会让唤醒卡死。 */
    dw_gdma_event_callbacks_t cbs = {
        .on_full_trans_done = mipi_dsi_dma_trans_done_cb,
    };
    dw_gdma_channel_register_event_callbacks(dpi->dma_chan, &cbs, dpi);

    dw_gdma_link_list_handle_t link_list = dpi->link_lists[dpi->cur_fb_index];
    dw_gdma_lli_handle_t lli = dw_gdma_link_list_get_item(link_list, 0);
    dw_gdma_block_markers_t markers = {
        .is_valid = true,
        .is_last = true,
    };
    dw_gdma_lli_set_block_markers(lli, markers);
    dw_gdma_channel_use_link_list(dpi->dma_chan, link_list);
    dw_gdma_channel_suspend_ctrl(dpi->dma_chan, false);
    dw_gdma_channel_enable_ctrl(dpi->dma_chan, true);
}

void p4_dpi_set_scan(esp_lcd_panel_handle_t panel, bool on) {
    if (panel == NULL || on == s_scan_on) {
        return;
    }
    struct esp_lcd_dpi_panel_t *dpi = __containerof(panel, struct esp_lcd_dpi_panel_t, base);
    if (!p4_dpi_layout_ok(dpi)) {
        ESP_LOGE(TAG, "dpi gate skipped: panel layout mismatch");
        return;
    }

    mipi_dsi_hal_context_t *hal = &dpi->bus->hal;
    const int bus_id = dpi->bus->bus_id;

    if (!on) {
        s_dma_idle = false;
        dw_gdma_event_callbacks_t cbs = {
            .on_full_trans_done = p4_dpi_dma_hold,
        };
        dw_gdma_channel_register_event_callbacks(dpi->dma_chan, &cbs, dpi);
        /* 等当前整帧送完再关。半帧掐断会把 RGB888 错位，重写链表则会让唤醒起不来。 */
        for (int i = 0; i < 40 && !s_dma_idle; i++) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        dw_gdma_channel_enable_ctrl(dpi->dma_chan, false);
        mipi_dsi_host_ll_enable_video_mode(hal->host, false);
        mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, false);
        mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
        mipi_dsi_brg_ll_enable_interrupt(hal->bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
        PERIPH_RCC_ATOMIC() {
            mipi_dsi_ll_enable_dpi_clock(bus_id, false);
        }
#if CONFIG_PM_ENABLE
        if (dpi->pm_lock) {
            esp_pm_lock_release(dpi->pm_lock);
        }
#endif
        s_scan_on = false;
        ESP_LOGI(TAG, "dpi scan off");
        return;
    }

#if CONFIG_PM_ENABLE
    if (dpi->pm_lock) {
        esp_pm_lock_acquire(dpi->pm_lock);
    }
#endif
    PERIPH_RCC_ATOMIC() {
        mipi_dsi_ll_enable_dpi_clock(bus_id, true);
    }
    /* 不复位 DSI 桥：复位会把时序清掉，触摸唤醒后屏幕起不来。整帧 DMA 已从缓冲头重写。 */
    p4_dpi_restart_dma(dpi);
    mipi_dsi_host_ll_enable_video_mode(hal->host, true);
    mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, true);
    mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
    mipi_dsi_brg_ll_enable_interrupt(hal->bridge, MIPI_DSI_BRG_LL_EVENT_UNDERRUN, true);
    s_scan_on = true;
    ESP_LOGI(TAG, "dpi scan on");
}
