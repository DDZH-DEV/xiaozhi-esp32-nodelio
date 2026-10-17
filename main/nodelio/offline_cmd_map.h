#pragma once

#include "nvs_config.h"
#include "panel_layout.h"

#include <esp_err.h>

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
};

/** 重启/退下等纯本机口令（不依赖 WiFi / layout）。 */
bool nodelio_offline_cmd_is_local(int command_id);

/** 命令 ID → 中文口令（供 MCP / 口令提示）. */
const char *nodelio_offline_cmd_zh(int command_id);

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

#ifdef __cplusplus
}
#endif
