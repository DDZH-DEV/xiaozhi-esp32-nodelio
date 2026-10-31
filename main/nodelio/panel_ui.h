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
/** 注册完成后同步 overlay / NVS 缓存（勿只更新 bridge 私有 cfg）. */
void nodelio_panel_ui_apply_cfg(const nodelio_nvs_config_t *cfg);

/** 用 layout 重建首页控件；须在 LVGL 锁内或由本函数加锁. */
esp_err_t nodelio_panel_ui_apply_layout(const nodelio_panel_layout_t *layout);

/** overlay 开机已 fill_home 后标记，避免 start_poll/同版本 apply 再 lv_obj_clean 闪屏. */
void nodelio_panel_ui_mark_home_filled(int layout_version);

/** 当前首页已渲染的 layout 版本；未渲染过为 -1. */
int nodelio_panel_ui_home_filled_version(void);

/** 在 HTTP/cJSON 之前占住界面布局缓冲（约 660KB）。已分配则直接成功。 */
esp_err_t nodelio_panel_ui_reserve_layout(void);

/** 更新状态栏：wifi / cloud / 文案. */
void nodelio_panel_ui_set_status(bool wifi_ok, bool cloud_ok, const char *msg);

/** 有触摸/操作时调用，重置息屏计时. */
void nodelio_panel_ui_note_activity(void);

/** 背光是否点亮（息屏时轮询可降频）. */
bool nodelio_panel_ui_screen_on(void);

/** 亮/息屏回调（仅显示态；断网见 net_cb）. 勿在回调里长时间阻塞. */
typedef void (*nodelio_panel_ui_screen_cb_t)(bool screen_on);
void nodelio_panel_ui_set_screen_cb(nodelio_panel_ui_screen_cb_t cb);

/** 联网/断网省电回调（可与息屏时刻不同）. 勿在回调里长时间阻塞. */
typedef void (*nodelio_panel_ui_net_cb_t)(bool net_on);
void nodelio_panel_ui_set_net_cb(nodelio_panel_ui_net_cb_t cb);

/** 板级息屏/亮屏时调用：降频轮询；不再隐含断网. */
void nodelio_panel_ui_notify_screen(bool on);

/** 板级断网/拉网时调用：触发 net_cb（勿在回调里阻塞）. */
void nodelio_panel_ui_notify_net(bool on);

/** 顶层短提示（任意任务可调；内部切 LVGL 锁）. */
void nodelio_panel_ui_toast(const char *text);

/** MQTT kind=skills：invalidate 已装清单后再拉一次（无市场轮询）. */
void nodelio_panel_ui_skills_changed(void);

/** bootstrap /skills 响应：同步写入已装列表与钉选（可无 UI 锁）；随后 Schedule 刷 UI. */
void nodelio_panel_ui_apply_skills_http_body(const char *json);

/**
 * MQTT kind=notice（及过渡 skill_hint）：事件驱动 Toast / 单次 invoke。
 * 禁止固件轮询。mode=info|action|alert；severity=normal|warning|critical。
 */
typedef struct nodelio_panel_notice {
    char notice_id[40];
    char mode[12];
    char severity[12];
    char title[96];
    char app_id[72];
    char invoke_app[72];
    char invoke_params[256];
    char session[80];
    char reason[24];
    int wake; /* 1=允许点亮背光 */
} nodelio_panel_notice_t;

void nodelio_panel_ui_notice(const nodelio_panel_notice_t *notice);
void nodelio_panel_ui_notice_clear(const char *notice_id);
/** 息屏期间积压的 info：亮屏后 Flush（由板级 OnExitSleepMode 调用）。 */
void nodelio_panel_ui_notice_flush_deferred(void);

/**
 * 旧 MQTT kind=skill_hint 入口：映射为 notice 后处理。
 */
void nodelio_panel_ui_skill_hint(const char *app_or_instance_id, const char *reason,
                                 const char *session);

/** 本地口令提示音（主任务播 OGG）；0=听令 1=成功 2=失败 */
void nodelio_panel_ui_play_cue(int cue);

/** 异步 toast：不等 LVGL 锁；供 AFE/MultiNet 任务，避免 FEED 环满. */
void nodelio_panel_ui_toast_defer(const char *text);

/**
 * 技能摘要上屏 + toast；有 audio_url（桥 SAPI WAV）则 Idle 后 HTTP 播报。
 * 无 URL 时仅 toast/上屏（勿当正文朗读）。
 */
void nodelio_panel_ui_announce_spoken(const char *text);

/**
 * 会话中短播报：listen.detect 注入「请向用户简短播报：…」（云端约 ≤16 字）。
 */
void nodelio_panel_ui_request_cloud_speak(const char *text);
/** 新闻朗读：op=read|continue → 短 detect 促云端 self.skill.news。 */
void nodelio_panel_ui_news_read_local(const char *op);

/** 语音「打开新闻」后武装：列表上屏时追问要不要读第一条（触控打开勿调）。 */
void nodelio_panel_ui_arm_news_read_offer(void);
/** 消费一次武装；未武装返回 false。 */
bool nodelio_panel_ui_take_news_read_offer(void);
/** audio_url 非空 → StartNotification；否则仅 toast/上屏。 */
void nodelio_panel_ui_announce_spoken_ex(const char *text, const char *audio_url);
/** 多段 WAV 连续播（urls[0..n)）；n≤0 则退回 toast。 */
void nodelio_panel_ui_announce_spoken_playlist(const char *text, const char (*urls)[320], int n);
/* urls 行宽与 NL_ENV_URL(320) 一致 */

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
    /* DUAL_IO_LOCAL_WEB：同网 HTTP 直控 */
    bool lan_control;
    bool pass_required;
    char ip[48];
} nl_dev_cache_t;

/** snapshot 增量写入（解析完后 end 刷新磁贴开/关色）。 */
void nodelio_panel_ui_snapshot_begin(void);
void nodelio_panel_ui_snapshot_add(const char *device_id, const char *name, bool online, const char *power_state);
/** 标记局域网直控设备（须在 snapshot_add 之后）。 */
void nodelio_panel_ui_snapshot_set_lan(const char *device_id, const char *ip, bool lan_control, bool has_pass);
void nodelio_panel_ui_snapshot_add_channel(const char *device_id, int channel_index, const char *ch_name,
                                           const char *state);
void nodelio_panel_ui_snapshot_end(void);

/**
 * 设备缓存视觉签名（online/power/通道/倒计时/灰显相关）。
 * snapshot / DIOL 前后比较：相同则跳过 refresh_home_tiles，避免无变化整页闪。
 */
uint32_t nodelio_panel_ui_dev_visual_sig(void);

/** 开机无云 snapshot 时恢复上次局域网 IP（DIOL）；overlay 启动时调一次。 */
void nodelio_panel_ui_lan_cache_load(void);

/**
 * 丢到 Application 主任务跑（片内栈）。
 * PSRAM 栈任务禁止直接 nvs/flash；layout_sync 等须经此再落盘。
 */
void nodelio_run_on_app_task(void (*fn)(void *), void *arg);

void nodelio_panel_ui_set_lan_online(const char *device_id, bool online, const char *power_state);
void nodelio_panel_ui_set_lan_pass_required(const char *device_id, bool need);
void nodelio_panel_ui_clear_lan_channels(const char *device_id);
void nodelio_panel_ui_refresh_tiles(void);

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

/** 离线口令「重启」：主任务延时后 Application::Reboot。 */
void nodelio_panel_ui_local_reboot(void);

/** 离线口令「进入配网」：进 SoftAP 门户（WiFi + 服务器）。 */
void nodelio_panel_ui_local_wifi_config(void);

/** 离线口令「退下」：收束会话并 RequestGoodbyeSleep（不依赖云）。 */
void nodelio_panel_ui_local_goodbye(void);

/** 语音/MCP 导航目标（屏坏可语音切页） */
enum {
    NL_UI_HOME = 0,
    NL_UI_APPS = 1,
    NL_UI_SETTINGS = 2,
    NL_UI_CONNECT = 3,
    NL_UI_DISP = 4,
    NL_UI_POWER = 5,
    NL_UI_VOICE = 6,
    NL_UI_MEDIA = 7,
    NL_UI_CTRL = 8,
    NL_UI_SYS = 9,
    NL_UI_RESET = 10, /* 恢复出厂确认页（须触确认，语音只打开不执行） */
};

/** 打开 overlay 并切到指定页（设置子页/连接页含在内）。 */
void nodelio_panel_ui_goto(int page);

/**
 * 首页用户屏翻页（与左/右滑同一路径）。delta=+1 下一屏，-1 上一屏。
 * 不在首页时先回到首页再翻。只有一屏 → ESP_ERR_NOT_FOUND；到头 → ESP_ERR_INVALID_STATE。
 */
esp_err_t nodelio_panel_ui_home_page(int delta);

/**
 * 竖向滚当前可见页（设置/内页/连接/应用/启动台）。dir=+1 往下，-1 往上。
 * 无可滚区 → ESP_ERR_NOT_FOUND；到顶/底 → ESP_ERR_INVALID_STATE。
 */
esp_err_t nodelio_panel_ui_scroll(int dir);

/**
 * 按应用名或 app_id 打开已装应用（invoke）。
 * 命中返回 true；未装/未知返回 false。
 */
bool nodelio_panel_ui_open_app(const char *name_or_id);

/**
 * 语音派活：POST dsh.task，body 含 instruction。
 * instruction 空则只 toast 提示，不 invoke。
 * 若已 arm 工作区/会话，body 会带上 workspace/session。
 */
bool nodelio_panel_ui_dsh_task(const char *instruction);

/**
 * 续聊武装后：先缓存 instruction，屏上确认后再 dsh.task。
 * 说「是/好」或点「发送」→ 投递；「不用/取消」或点「取消」→ 丢弃并继续开麦。
 */
bool nodelio_panel_ui_dsh_confirm_send(const char *instruction);
/**
 * STT 处理待确认句。
 * @return 0 未处理；1 已确认发送；2 取消/改口。处理后保持开麦。
 */
int nodelio_panel_ui_dsh_try_confirm_send(const char *stt);
void nodelio_panel_ui_dsh_confirm_accept(void);
void nodelio_panel_ui_dsh_confirm_reject(void);
bool nodelio_panel_ui_dsh_has_pending_send(void);
/** 读待确认指令（可空）；无则 dst 置空串。 */
void nodelio_panel_ui_dsh_get_pending_send(char *dst, size_t dst_len);

/** 列表「派活/续聊」：武装下次「让它…」或武装直说（可空）。 */
void nodelio_panel_ui_arm_dsh(const char *workspace, const char *session);
void nodelio_panel_ui_clear_dsh_arm(void);
/** 是否已武装工作区或会话（续聊后下一句可直接派活）。 */
bool nodelio_panel_ui_dsh_is_armed(void);
/** 读当前武装；无则对应缓冲为空串。 */
void nodelio_panel_ui_get_dsh_arm(char *workspace, size_t ws_len, char *session, size_t sess_len);

/**
 * 统一改设置（语音/MCP/UI 同路径）。key/value 见 PLAN-VOICE-SETTINGS。
 * 校验失败写中文到 err（可空），不静默夹到非法值；brightness 仅 0–4→5。
 * 落盘：nvs_cache_put + Schedule(nvs_save)；勿在 PSRAM 任务直接写 flash。
 */
esp_err_t nodelio_panel_ui_set_pref(const char *key, const char *value, char *err, size_t err_len);

/** 读当前偏好为短字符串；未知 key → ESP_ERR_NOT_FOUND。 */
esp_err_t nodelio_panel_ui_get_pref(const char *key, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
