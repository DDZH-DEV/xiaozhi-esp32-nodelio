#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 打开内存卡目录浏览器（异步挂卡 + 列举根目录）。 */
void nodelio_skill_request_sd_files(void);

bool nodelio_skill_sd_files_visible(void);

void nodelio_skill_sd_files_hide(void);

#ifdef __cplusplus
}
#endif
