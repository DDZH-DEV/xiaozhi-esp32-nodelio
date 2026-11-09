#pragma once

/** support_thread：左右气泡 + 底部操作条。须在 LVGL 锁内调用。 */
void fill_support_thread(void);
/** 收起标题行上的「返回列表 / 取消托管」。其它卡片填充前调用。 */
void hide_support_title_nav(void);
