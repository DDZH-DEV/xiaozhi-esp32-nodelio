#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 墙板 DeviceCapabilities（模板名 esp32-panel-480，仅本地；线上不报 profile）。
 *
 * 诚实申报 / 线上形状（Protocol 00 §4.1、PLAN K4）：
 * - 只写展开后的 capabilities（无 profile、无 disabled 凑数）
 * - NL_CAPS_WITH_MEDIA=0：content-only，无 media.*、无 limits.media
 * - NL_CAPS_WITH_MEDIA=1：含 media.play_local + limits.media（fetch=http_via_server）
 * - surfaces 含 home_tile；不报 action.open_url
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
