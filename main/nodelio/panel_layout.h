#pragma once

#include "nvs_config.h"

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 结构体会进 PSRAM；上限按中控面板够用即可 */
#define NL_LAYOUT_MAX_PAGES 4
#define NL_LAYOUT_MAX_SECTIONS 6
#define NL_LAYOUT_MAX_WIDGETS 16
#define NL_LAYOUT_STR 32

typedef enum {
    NL_WIDGET_UNKNOWN = 0,
    NL_WIDGET_SCENE_CHIP,
    NL_WIDGET_SCENE_GRID,
    NL_WIDGET_SWITCH_TILE,
    NL_WIDGET_PIN_TOGGLE,
    NL_WIDGET_SLIDER_TILE,
    NL_WIDGET_STATUS_BADGE,
    NL_WIDGET_SECTION_HEADER,
    NL_WIDGET_SPACER,
} nl_widget_type_t;

typedef enum {
    NL_BIND_NONE = 0,
    NL_BIND_HOME_SCENARIO,
    NL_BIND_DEVICE_CHANNEL,
    NL_BIND_DEVICE_PIN,
} nl_bind_kind_t;

typedef struct {
    nl_bind_kind_t kind;
    char device_id[NL_LAYOUT_STR];
    char name[NL_LAYOUT_STR];
    char capability[16]; /* gpio_out / pwm / … */
    int channel;         /* -1 = unset */
    int pin;             /* -1 = unset */
    int min;
    int max;
} nl_bind_t;

typedef struct {
    char id[NL_LAYOUT_STR];
    char title[NL_LAYOUT_STR];
    nl_widget_type_t type;
    char skin[16];       /* dual_line / momentary / latching / dimmer / fan / heater / gauge / tank / generic */
    char labels_on[16];  /* DualLine 开态标签（如 NC） */
    char labels_off[16]; /* DualLine 关态标签（如 NO） */
    nl_bind_t bind;
    /* 网格占位：col/row 从 0；gw/gh 占格数。col/row=-1 表示由固件自动装箱 */
    int8_t col;
    int8_t row;
    int8_t gw;
    int8_t gh;
    char bg_color[16]; /* 可选 "#RRGGBB"；空则按 type 默认色 */
} nl_widget_t;

typedef struct {
    char id[NL_LAYOUT_STR];
    char title[NL_LAYOUT_STR];
    int widget_count;
    nl_widget_t widgets[NL_LAYOUT_MAX_WIDGETS];
} nl_section_t;

typedef struct {
    char id[NL_LAYOUT_STR];
    char title[NL_LAYOUT_STR];
    char system_page[24]; /* assistant / settings / empty */
    int section_count;
    nl_section_t sections[NL_LAYOUT_MAX_SECTIONS];
} nl_page_t;

typedef struct {
    int layout_version;
    char theme_primary[16];
    char theme_bg[16];
    uint16_t display_w;
    uint16_t display_h;
    uint8_t grid_cols; /* 默认 4 */
    uint8_t grid_rows; /* 默认 4 */
    int page_count;
    nl_page_t pages[NL_LAYOUT_MAX_PAGES];
} nodelio_panel_layout_t;

/** Parse API body `{ "layoutVersion", "layout": {…} }` or bare layout object. */
esp_err_t nodelio_layout_parse(const char *json, nodelio_panel_layout_t *out);

/** Log widget tree (A4b 前无屏；接 LVGL 后替换为真渲染). */
esp_err_t nodelio_layout_render(const nodelio_panel_layout_t *layout);

/** GET …/panel/layout；version 未变时返回 ESP_ERR_NOT_FINISHED（304/同版）. */
esp_err_t nodelio_layout_fetch(const nodelio_nvs_config_t *cfg, int known_version, nodelio_panel_layout_t *out);

/** GET …/panel/snapshot；日志 devices[].online（灰显等 LVGL）. */
esp_err_t nodelio_snapshot_fetch(const nodelio_nvs_config_t *cfg);

/**
 * POST …/panel/command。
 * value_str：开关用 "ON"/"OFF"/"TOGGLE"；PWM 用数字字符串；情景可空（用 bind.name）。
 * 返回 HTTP 状态码到 *http_status（可 NULL）；503→ESP_ERR_INVALID_STATE 供 UI 回滚。
 */
esp_err_t nodelio_panel_command(const nodelio_nvs_config_t *cfg, const char *widget_id, const char *action,
                                const nl_bind_t *bind, const char *value_str, int *http_status);

/**
 * 后台：开机 layout+snapshot 一次；之后 MQTT notify / 手动刷新 /
 * 无推送时半小时兜底 snapshot。禁止定时闪频轮询。
 */
esp_err_t nodelio_layout_start_poll(const nodelio_nvs_config_t *cfg);

/**
 * 立即拉一次 layout + snapshot（设置页「刷新布局」）。
 * 可从 LVGL 线程调用。
 */
void nodelio_layout_request_sync(void);

/** MQTT panel/notify(kind=snapshot) 触发：只拉 snapshot，不重建布局。 */
void nodelio_layout_request_snapshot(void);

#ifdef __cplusplus
}
#endif
