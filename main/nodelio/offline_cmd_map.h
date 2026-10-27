#pragma once

#include "nvs_config.h"
#include "panel_layout.h"

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 出厂通用命令 ID（拼音词表）；映射跟当前 Layout，不写死某户设备 ID */
enum {
    NL_OC_LIGHT_ON = 1,
    NL_OC_LIGHT_OFF = 2,
    NL_OC_SCENE_HOME = 3,
    NL_OC_SCENE_AWAY = 4,
    NL_OC_CURTAIN_OPEN = 5,
    NL_OC_CURTAIN_CLOSE = 6,
    /* 本机动作：断网/无云也可执行，不走 panel/command */
    NL_OC_REBOOT = 7,
    NL_OC_GOODBYE = 8,
    /* 面板曲库切歌/暂停（依赖内存中的当前列表，不走 HTTP） */
    NL_OC_MEDIA_PREV = 9,
    NL_OC_MEDIA_NEXT = 10,
    NL_OC_MEDIA_PAUSE = 11,
    NL_OC_MEDIA_RESUME = 12,
    /* 控家模块关闭时的降级口令：只提示，不发 panel/command */
    NL_OC_CTRL_OFF = 13,
    /* 本机：进 SoftAP 配网（屏坏可说「进入配网」） */
    NL_OC_WIFI_CONFIG = 14,
    /* 本机 UI 导航（屏坏可语音切页） */
    NL_OC_GOTO_HOME = 15,
    NL_OC_GOTO_APPS = 16,
    NL_OC_GOTO_SETTINGS = 17,
    NL_OC_GOTO_CONNECT = 18,
    NL_OC_GOTO_DISP = 19,
    NL_OC_GOTO_POWER = 20,
    NL_OC_GOTO_VOICE = 21,
    NL_OC_GOTO_MEDIA = 22,
    NL_OC_GOTO_CTRL = 23,
    NL_OC_GOTO_SYS = 24,
    /* 本机设置开关（≠ 打开省电/控家设置页） */
    NL_OC_POWER_SAVE_ON = 25,
    NL_OC_POWER_SAVE_OFF = 26,
    NL_OC_CONTROLS_ON = 27,
    NL_OC_CONTROLS_OFF = 28,
    NL_OC_XIAOZHI_ON = 29,
    NL_OC_XIAOZHI_OFF = 30,
    NL_OC_LAYOUT_SYNC = 31,
    NL_OC_FW_CHECK = 32,
    NL_OC_GOTO_RESET = 33, /* 仅打开确认页，不执行重置 */
    /* 首页多屏：与左滑/右滑同一路径 */
    NL_OC_PAGE_NEXT = 34,
    NL_OC_PAGE_PREV = 35,
    /* 当前可见页竖向滚（设置/内页/连接/应用/启动台） */
    NL_OC_SCROLL_DOWN = 36,
    NL_OC_SCROLL_UP = 37,
    /* 已装样例应用：本地 MultiNet 固定拼音，打开走 open_app 别名 */
    NL_OC_APP_MUSIC = 38,
    NL_OC_APP_NEWS = 39,
    NL_OC_APP_WEATHER = 40,
    NL_OC_APP_RADIO = 41,
    NL_OC_APP_PODCAST = 42,
    NL_OC_APP_AIR = 43,
    NL_OC_APP_QUOTE = 44,
    /* 停播并收起曲库/电台等技能弹窗（≠ 退下息屏） */
    NL_OC_SKILL_CLOSE = 45,
};

/** 重启/退下等纯本机口令（不依赖 WiFi / layout）。 */
bool nodelio_offline_cmd_is_local(int command_id);

/** 命令 ID → 中文口令（供 MCP / 口令提示）. */
const char *nodelio_offline_cmd_zh(int command_id);

/** 设置页「可说口令」：分组 + 用户可说的中文（多别名用「 / 」）。 */
typedef struct {
    const char *group;
    const char *say;
} nodelio_oc_catalog_entry_t;

const nodelio_oc_catalog_entry_t *nodelio_offline_cmd_catalog(size_t *count);

/** 按 command_id 在 layout 中找 bind 并 POST panel/command。 */
esp_err_t nodelio_offline_cmd_dispatch(const nodelio_nvs_config_t *cfg, int command_id,
                                       const nodelio_panel_layout_t *layout);

/**
 * 从自然语言短句映射并执行（开灯/关灯/打开继电器/回家…）。
 * 供 MCP 与 K4 本地口令共用。
 * 无法识别口令 → ESP_ERR_INVALID_ARG；布局无控件 → ESP_ERR_NOT_FOUND；离线 → ESP_ERR_INVALID_STATE。
 */
esp_err_t nodelio_offline_try_phrase(const nodelio_nvs_config_t *cfg, const char *phrase,
                                     const nodelio_panel_layout_t *layout);

/**
 * 仅匹配本机系统口令并立即执行（不依赖 cfg / layout）：
 * 重启 / 进入配网 / UI 导航 / 打开某应用。
 * 供小智 STT 旁路与 MCP 使用；未命中返回 false。
 * end_session：非空时写出是否应结束语音会话。
 * true：重启/配网/关播放器/打开应用（须 Idle 才能播）。
 * false：设置导航/开关/翻页/切歌（保持 Listening，对话框不落，可连说）。
 */
bool nodelio_offline_try_system_phrase(const char *phrase, bool *end_session);

#ifdef __cplusplus
}
#endif
