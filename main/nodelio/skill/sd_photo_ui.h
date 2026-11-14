#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载并扫描 jpg，打开曲库卡「内存卡相册」列表（异步 Job）。 */
void nodelio_skill_request_sd_photos(void);

/** 打开全屏查看器并解码第 index 张（列表点选 / 左右滑）。 */
void nodelio_skill_sd_photo_show(int index);

/** 按 VFS 绝对路径打开；必要时先扫照片表。 */
void nodelio_skill_sd_photo_show_path(const char *abs_path);

/** 是否正在显示内存卡相册查看器。 */
bool nodelio_skill_sd_photo_visible(void);

/** 关闭相册查看器。 */
void nodelio_skill_sd_photo_hide(void);

#ifdef __cplusplus
}
#endif
