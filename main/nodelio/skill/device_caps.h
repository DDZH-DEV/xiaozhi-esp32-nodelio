#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 墙板 DeviceCapabilities（Protocol esp32-panel-480）。
 *
 * 诚实申报：
 * - N0～N2：NL_CAPS_WITH_MEDIA=0（content-only，disabled media）
 * - N0b 后：NL_CAPS_WITH_MEDIA=1（含 media.play_local / now_playing）
 * - surfaces 含 home_tile（PLAN C）：钉选槽位 + 首页磁贴渲染（overlay_home_skills）
 * 编译覆盖：-DNL_CAPS_WITH_MEDIA=0|1
 */
bool nodelio_caps_has(const char *capability);
bool nodelio_caps_has_media(void);
bool nodelio_caps_has_surface(const char *surface);

int nodelio_caps_max_content_bytes(void);
int nodelio_caps_max_list_items(void);

/** 写出完整 capabilities JSON；截断返回负值。 */
int nodelio_caps_export_json(char *buf, size_t buf_len);

const char *nodelio_caps_profile(void);

#ifdef __cplusplus
}
#endif
