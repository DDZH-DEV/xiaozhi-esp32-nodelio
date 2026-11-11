#pragma once

#include "nvs_config.h"
#include "panel_layout.h"

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** After Xiaozhi Display::SetupUI — creates FAB + hidden overlay. */
esp_err_t nodelio_overlay_init(void);

/** Show overlay (home/settings). If force_settings, open 连接页. */
void nodelio_overlay_show(bool force_settings);

void nodelio_overlay_hide(void);

bool nodelio_overlay_visible(void);

/** 展开对话抽屉；若当前 idle/播报中则 ToggleChat 建链开麦（点「进入对话」与唤醒同路径）. */
void nodelio_overlay_show_chat(void);

/** 唤醒建链：先弹出抽屉，正文显示连接进度（旧小智体验）. */
void nodelio_overlay_show_connecting(void);

/** 已持 LVGL/Display 锁时喂对话行（role: user/assistant/system）. */
void nodelio_overlay_on_chat_locked(const char *role, const char *text);

/** 已持锁时更新对话页状态（聆听/说话/待命）. */
void nodelio_overlay_on_status_locked(const char *status);

/**
 * 主题字库对象即将销毁前调用（已持显示锁）。
 * 把仍指向旧 lv_font_t 的控件改到新字库，避免下一帧字形回调用悬空指针。
 */
void nodelio_overlay_retarget_font(const void *from_font, const void *to_font);

/** 全局应用当前文字缩放（内部自带 DisplayLockGuard）。 */
void nodelio_overlay_apply_text_scale(void);

/**
 * 开机首帧刷完后再开背光（assets/字号落地后由 Application 调用）。
 * 壳建完不立刻亮：避免 assets 再刷一次首页时背光已开导致撕裂闪屏。
 */
void nodelio_overlay_reveal_boot_backlight(void);

/**
 * 首页已就绪且不在屏上配网/连接子页时返回 true：
 * 抑制后台 WiFi 扫描/连接/关联/已连等进度弹窗（避免双缓冲 DIRECT 连续全屏交换导致 RGB 闪屏）。
 */
bool nodelio_overlay_suppress_net_toast(void);

#ifdef __cplusplus
}
#endif
