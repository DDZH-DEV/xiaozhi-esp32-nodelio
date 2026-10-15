#pragma once

#include "nvs_config.h"
#include "panel_layout.h"

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Init LVGL on board panel+touch；建壳（状态栏/底栏/三页）. */
esp_err_t nodelio_panel_ui_start(const nodelio_nvs_config_t *cfg);

/** 用 layout 重建首页控件；须在 LVGL 锁内或由本函数加锁. */
esp_err_t nodelio_panel_ui_apply_layout(const nodelio_panel_layout_t *layout);

/** 更新状态栏：wifi / cloud / 文案. */
void nodelio_panel_ui_set_status(bool wifi_ok, bool cloud_ok, const char *msg);

/** 有触摸/操作时调用，重置息屏计时. */
void nodelio_panel_ui_note_activity(void);

/** 背光是否点亮（息屏时轮询可降频）. */
bool nodelio_panel_ui_screen_on(void);

/** 亮/息屏回调（息=断网省电；亮=先点背光再由产品壳拉网）. 勿在回调里长时间阻塞. */
typedef void (*nodelio_panel_ui_screen_cb_t)(bool screen_on);
void nodelio_panel_ui_set_screen_cb(nodelio_panel_ui_screen_cb_t cb);

/** 板级息屏/亮屏时调用：降频轮询并触发 screen_cb（勿在回调里阻塞）. */
void nodelio_panel_ui_notify_screen(bool on);

/** 顶层短提示（任意任务可调；内部切 LVGL 锁）. */
void nodelio_panel_ui_toast(const char *text);

/** 异步 toast：不等 LVGL 锁；供 AFE/MultiNet 任务，避免 FEED 环满. */
void nodelio_panel_ui_toast_defer(const char *text);

/**
 * 语音状态写入助手页预埋区（开机已建好的定宽标签，只改字不改布局）.
 * hint=NULL → 待命；含「听」→ 聆听中；其它 → 提示文案.
 */
void nodelio_panel_ui_set_voice_hint(const char *hint);

/**
 * 助手页对话行（预埋定宽标签，idle 刷；role 如「我」「助手」）.
 * 不切 Tab、不建新对象.
 */
void nodelio_panel_ui_append_chat(const char *role, const char *text);

/** 流式增量：刷新状态条文案（累积）；done=true 时写入对话区一行. */
void nodelio_panel_ui_stream_assistant(const char *delta_or_full, bool is_delta, bool done);

/** 当前已应用 layout（无则 NULL）；供离线命令词映射. */
const nodelio_panel_layout_t *nodelio_panel_ui_get_layout(void);

/** 与壳布局一致（APP 画布 content = display − STATUS − TAB）. */
#define NL_UI_STATUS_H 32
#define NL_UI_TAB_H 44
/** 首页网格：左右留白；顶距与设置页对齐（勿过大） */
#define NL_UI_HOME_PAD_H 14
#define NL_UI_HOME_PAD_V 2
/** 设置页卡片相对屏左右边距（比首页略宽，卡片不贴边） */
#define NL_UI_SETTINGS_PAD_H 20
/** 设置子页内容边距（顶略紧，避免导航栏下空一大块） */
#define NL_UI_SETTINGS_DETAIL_PAD 12
#define NL_UI_SETTINGS_DETAIL_PAD_TOP 8
/** 连接页左右再宽一档 */
#define NL_UI_CONNECT_PAD_H 36
/** 临时量顶栏间隙：1=顶栏红框/内容青描边；默认关 */
#ifndef NL_UI_CHROME_DEBUG
#define NL_UI_CHROME_DEBUG 0
#endif

/**
 * 按 snapshot 刷新控件可达性：offline 的 deviceId 灰显；
 * device_ids[i] 对应 online[i]；情景按钮不受影响.
 * @deprecated 优先用 snapshot_begin/add/end（含开/关态）
 */
void nodelio_panel_ui_apply_reachability(const char *const *device_ids, const bool *online, int n);

#define NL_DEV_CACHE_MAX 32
#define NL_CH_PER_DEV 16

typedef struct {
    char id[NL_LAYOUT_STR];
    char name[NL_LAYOUT_STR];
    bool online;
    char power[12];
    int16_t countdown; /* 设备级剩余秒；0=无 */
    uint8_t ch_n;
    int16_t ch_idx[NL_CH_PER_DEV];
    char ch_name[NL_CH_PER_DEV][NL_LAYOUT_STR];
    char ch_st[NL_CH_PER_DEV][12];
    int16_t ch_cd[NL_CH_PER_DEV]; /* 通道倒计时剩余秒 */
} nl_dev_cache_t;

/** snapshot 增量写入（解析完后 end 刷新磁贴开/关色）。 */
void nodelio_panel_ui_snapshot_begin(void);
void nodelio_panel_ui_snapshot_add(const char *device_id, const char *name, bool online, const char *power_state);
void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index, const char *ch_name,
                                           const char *state);
void nodelio_panel_ui_snapshot_end(void);

/** 仅写缓存（snapshot 批量用）；channel_index 为负表示设备级。 */
void nodelio_panel_ui_note_countdown(const char *device_id, int channel_index, int seconds);
/** 写缓存并立刻刷磁贴（MQTT / 乐观控制）。 */
void nodelio_panel_ui_set_countdown(const char *device_id, int channel_index, int seconds);

/**
 * panel/command 成功后立刻同步首页磁贴（写本地 cache + 刷色）。
 * 语音会话期 MQTT snapshot 会让路，靠本接口避免「已开关但页面不变」。
 * OFF_IN / CANCEL_COUNTDOWN 会写本地 countdown。
 */
void nodelio_panel_ui_apply_bind_state(const nl_bind_t *bind, const char *value);

/** 获取当前缓存的设备总数与单个设备快照（供 MCP 等模块枚举未排布设备） */
int nodelio_panel_ui_get_dev_count(void);
const nl_dev_cache_t *nodelio_panel_ui_get_dev(int index);

/** snapshot 缓存的在线态；未知设备视为在线（与灰显逻辑一致）。 */
bool nodelio_panel_ui_device_online(const char *device_id);

#ifdef __cplusplus
}
#endif
