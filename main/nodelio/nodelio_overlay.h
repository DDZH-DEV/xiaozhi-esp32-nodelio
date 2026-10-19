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

#ifdef __cplusplus
}
#endif
