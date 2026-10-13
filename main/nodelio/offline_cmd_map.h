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
};

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
