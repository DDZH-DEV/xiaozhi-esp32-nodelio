#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 开机预留片内连续块，供播中武装唤醒时释放（防碎片）。失败不阻塞启动。 */
bool nodelio_wake_dram_reserve(void);

/** 武装唤醒前释放坑；返回释放的字节数（0=当时无坑）。 */
size_t nodelio_wake_dram_release_for_arm(void);

/** 播停后尝试重新占坑；失败仅打日志。 */
bool nodelio_wake_dram_try_reclaim(void);

bool nodelio_wake_dram_held(void);

#ifdef __cplusplus
}
#endif
