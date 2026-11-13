#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 段1渲染实测页：经典实心 / 运行时半透明 / 预烘焙毛玻璃 三模式自动轮换并打日志。
 *  须持显示锁调用；仅 CONFIG_NODELIO_UI_PROBE=y 时有实现。点统计标签退出。 */
void nl_ui_probe_open(void);
void nl_ui_probe_close(void);

#ifdef __cplusplus
}
#endif

