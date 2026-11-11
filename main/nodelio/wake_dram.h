#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 开机预留两块片内连续内存（失败不阻塞启动）：
 * - wake：~12KB INTERNAL，供播中武装唤醒临时释放
 * - shot：~2KB DMA，专供截图上传；唤醒路径永不碰
 */
bool nodelio_wake_dram_reserve(void);

/** 武装唤醒前释放 wake 坑；返回释放字节（0=无坑）。不影响 shot 坑。 */
size_t nodelio_wake_dram_release_for_arm(void);

/** 播停 / 会话结束后尝试重新占 wake 坑。 */
bool nodelio_wake_dram_try_reclaim(void);

bool nodelio_wake_dram_held(void);

/** 截图上传前释放 shot DMA 坑；返回释放字节（0=无坑）。 */
size_t nodelio_shot_dma_release(void);

/** 截图结束后重新占 shot 坑。 */
bool nodelio_shot_dma_try_reclaim(void);

bool nodelio_shot_dma_held(void);

#ifdef __cplusplus
}
#endif
