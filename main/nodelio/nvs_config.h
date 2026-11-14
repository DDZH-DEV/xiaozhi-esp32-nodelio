#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODELIO_NVS_NS "nodelio"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server_url[128];
    char device_key[65];
    char assistant_ws_url[128];
    char device_id[40];
    char owner[64];
    char mqtt_server[40];
    char mqtt_port[8];
    char mqtt_user[64];
    char mqtt_pass[40];
    char topic_prefix[40];
    char screenshot_url[128];
    uint8_t brightness; /* 1–100；0=未设置用板级默认 */
    uint8_t volume;     /* 0–100；助手音频启用后生效 */
    uint8_t xiaozhi_en; /* 1=启用小智云对话（默认）；0=关闭 */
    uint8_t voice_barge_in; /* 1=说话中免唤醒词打断（服务端 AEC+realtime）；默认 0 */
    uint8_t local_cmd_en; /* 已废弃：固件不再编 MultiNet，读写恒为 0 */
    uint8_t power_save_en; /* 1=允许空闲断网（默认）；0=不断 WiFi；息屏仍走 idle_sleep */
    uint8_t wake_on_sleep; /* 1=息屏保留语音唤醒；0=息屏停麦克风与唤醒词（省电默认） */
    uint8_t controls_en;   /* 1=允许控家模块（默认）；0=首页不渲染控家，语音/MCP 降级提示 */
    uint8_t media_sleep_en; /* 1=播放中可息屏不停播（默认）；0=播放中常亮 */
    uint16_t idle_sleep_s;    /* 空闲息屏秒数，默认 60，范围 15–180 */
    uint16_t idle_wifi_s;     /* 空闲断网秒数，默认同息屏，范围 15–300；与息屏独立 */
    uint16_t goodbye_sleep_s; /* 退下后延迟息屏秒数，默认 15，范围 5–60 */
    uint16_t listen_idle_s; /* 唤醒后无对话自动退下秒数，默认 15，范围 10–30 */
    uint16_t media_pause_s; /* 暂停后关闭播放器秒数，默认 20，范围 10–120 */
    uint8_t media_wake_duck_pct; /* 播中武装唤醒时保留音量%，默认 70，范围 40–100 */
    uint8_t vinyl_spin_en; /* 沉浸页黑胶旋转：1=开；0=关（默认，避 LVGL 占满 core1） */
    uint8_t text_scale; /* 显示文字：0=标准 1=较大 2=更大；默认 1（墙板远读） */
    uint8_t ui_mode;    /* 0=浅色（缺键默认） 1=曜夜深色 */
    uint8_t ai_pos_set; /* 1=小智弹窗用 ai_x/ai_y；0=默认贴底 */
    int16_t ai_x;       /* 弹窗左上角，相对屏幕 */
    int16_t ai_y;
    uint16_t ai_w;      /* 对话框宽，0=默认 348 */
    uint16_t ai_h;      /* 对话框高，0=默认 UI_AI_DRAWER_H */
    uint8_t ai_lines;   /* 展示句数，0=默认 5；范围 2–8 */
    uint8_t ai_bd_w;    /* 边框粗细 0–4；255=默认 1 */
    uint8_t ai_sh_w;    /* 阴影宽度 0–24；255=浅色 12 / 深色 0 */
    uint8_t ai_bd_i;    /* 边框色 0=默认 1=暖灰 2=绿 */
    uint8_t status_bar_mode;   /* 顶栏状态栏：0=自动隐藏(默认) 1=常显 2=常隐 */
    uint8_t screenshot_fab_en; /* 截图悬浮图标：0=隐藏 1=显示(默认) */
    uint8_t card_radius;       /* 卡片圆角像素半径：0–24px (默认 12px) */
    uint8_t notice_sound;      /* 推送提示音：0=关 1=轻响 2=成功 3=警示；缺省 1 */
    uint8_t notice_repeat;     /* 1=未处理的 action/alert 每隔十余秒再响；0=只响一次 */
} nodelio_nvs_config_t;

#define NODELIO_TEXT_SCALE_DEFAULT 1
#define NODELIO_TEXT_SCALE_MAX 1
#define NODELIO_UI_MODE_MAX 2

#define NODELIO_IDLE_SLEEP_DEFAULT 60
#define NODELIO_IDLE_WIFI_DEFAULT 60
#define NODELIO_BYE_SLEEP_DEFAULT 15
#define NODELIO_LISTEN_IDLE_DEFAULT 15
#define NODELIO_MEDIA_PAUSE_DEFAULT 20
#define NODELIO_IDLE_SLEEP_MIN 15
#define NODELIO_IDLE_SLEEP_MAX 180
#define NODELIO_IDLE_WIFI_MIN 15
#define NODELIO_IDLE_WIFI_MAX 300
#define NODELIO_BYE_SLEEP_MIN 5
#define NODELIO_BYE_SLEEP_MAX 60
#define NODELIO_LISTEN_IDLE_MIN 10
#define NODELIO_LISTEN_IDLE_MAX 30
#define NODELIO_MEDIA_PAUSE_MIN 10
#define NODELIO_MEDIA_PAUSE_MAX 120
#define NODELIO_MEDIA_WAKE_DUCK_DEFAULT 70
#define NODELIO_MEDIA_WAKE_DUCK_MIN 40
#define NODELIO_MEDIA_WAKE_DUCK_MAX 100
#define NODELIO_AI_W_DEFAULT 348
#define NODELIO_AI_W_MIN 280
#define NODELIO_AI_H_DEFAULT 174
#define NODELIO_AI_H_MIN 140
#define NODELIO_AI_LINES_DEFAULT 5
#define NODELIO_AI_LINES_MIN 2
#define NODELIO_AI_LINES_MAX 8
#define NODELIO_AI_BD_DEFAULT 1
#define NODELIO_AI_BD_MAX 4
#define NODELIO_AI_SH_DEFAULT 12
#define NODELIO_AI_SH_MAX 24
#define NODELIO_AI_UNSET 255

esp_err_t nodelio_nvs_init(void);
/** Flash 读写：禁止在 PSRAM 栈任务调用（LVGL / mcp_http / nl_* invoke）。用 cache_get。 */
esp_err_t nodelio_nvs_load(nodelio_nvs_config_t *out);
esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t *in);
/** Save preferences without writing connection/registration fields from a stale UI snapshot. */
esp_err_t nodelio_nvs_save_preferences(const nodelio_nvs_config_t* in);
void nodelio_nvs_cache_put_preferences(const nodelio_nvs_config_t* in);
/** Merge registration credentials only if the server and account are still current. */
esp_err_t nodelio_nvs_save_registration(const nodelio_nvs_config_t* in);
/** Save connection fields, preserve current preferences, then verify persisted URL/account. */
esp_err_t nodelio_nvs_save_connection(const nodelio_nvs_config_t* in);
/** NVS 剩余条目是否够写历史等非关键 blob；不够则跳过以免占满后开机整区擦除。 */
bool nodelio_nvs_has_headroom(void);
esp_err_t nodelio_nvs_clear(void);
/** 仅更新 RAM 镜像（LVGL 安全）；随后应由主任务 nodelio_nvs_save。 */
void nodelio_nvs_cache_put(const nodelio_nvs_config_t *in);
void nodelio_nvs_cache_get(nodelio_nvs_config_t *out);

/** 连接页「服务器」历史（主机/端口/协议/绑定码），最多 6 条，MRU。 */
#define NODELIO_CONN_HIST_MAX 6
typedef struct {
    char host[96];
    char port[8];
    char owner[64];
    uint8_t https; /* 1=https */
} nodelio_conn_hist_entry_t;
typedef struct {
    nodelio_conn_hist_entry_t e[NODELIO_CONN_HIST_MAX];
    uint8_t n;
} nodelio_conn_hist_t;
void nodelio_conn_hist_cache_get(nodelio_conn_hist_t *out);
void nodelio_conn_hist_cache_put(const nodelio_conn_hist_t *in);
esp_err_t nodelio_conn_hist_load(nodelio_conn_hist_t *out);
esp_err_t nodelio_conn_hist_save(const nodelio_conn_hist_t *in);
/** 去重后插入队首；host 空则忽略。 */
void nodelio_conn_hist_push(nodelio_conn_hist_t *h, bool https, const char *host, const char *port,
                            const char *owner);
/** Drop broker credentials (keep device_id/key); call when server_url changes. */
void nodelio_nvs_clear_mqtt(nodelio_nvs_config_t *cfg);
bool nodelio_nvs_has_wifi(const nodelio_nvs_config_t *cfg);
/** 小智云对话开关；读 RAM 镜像，缺省开启。 */
bool nodelio_xiaozhi_enabled(void);
esp_err_t nodelio_xiaozhi_set_enabled(bool on);
/** 免唤醒词语音打断；读 RAM 镜像，缺省关闭。 */
bool nodelio_voice_barge_in_enabled(void);
esp_err_t nodelio_voice_barge_in_set_enabled(bool on);
/** 按 NVS 把 Application AEC 模式同步为 ServerSide/Off（可静默）。 */
void nodelio_voice_barge_in_apply(bool notify);
/** 本地 MultiNet 口令；读 RAM 镜像，缺省关闭（不占片内 DRAM）。 */
bool nodelio_local_cmd_enabled(void);
esp_err_t nodelio_local_cmd_set_enabled(bool on);
/** 空闲断网开关（不影响息屏）；读 RAM 镜像，缺省开启。 */
bool nodelio_power_save_enabled(void);
esp_err_t nodelio_power_save_set_enabled(bool on);
bool nodelio_wake_on_sleep_enabled(void);
esp_err_t nodelio_wake_on_sleep_set_enabled(bool on);
/** 控家模块；读 RAM 镜像，缺省开启。 */
bool nodelio_controls_enabled(void);
esp_err_t nodelio_controls_set_enabled(bool on);
uint16_t nodelio_idle_sleep_seconds(void);
uint16_t nodelio_idle_wifi_seconds(void);
uint16_t nodelio_goodbye_sleep_seconds(void);
uint16_t nodelio_listen_idle_seconds(void);
/** 播放中是否允许息屏（不停播）；默认开。 */
bool nodelio_media_sleep_enabled(void);
/** 暂停后关闭播放器秒数。 */
uint16_t nodelio_media_pause_seconds(void);
/** 播中武装唤醒时保留的音量百分比（相对用户音量）。 */
uint8_t nodelio_media_wake_duck_pct(void);
/** 沉浸页黑胶是否旋转；默认关。 */
bool nodelio_vinyl_spin_enabled(void);
/** 显示文字档 0–2；读 RAM 镜像。 */
uint8_t nodelio_text_scale(void);
esp_err_t nodelio_text_scale_set(uint8_t scale);
/** 0=浅色 1=深色；读 RAM 镜像，缺省浅色。 */
uint8_t nodelio_ui_mode(void);
/** 写入并钳位；某项为 0 则保持原值。含 flash，勿在 LVGL 任务调用。 */
esp_err_t nodelio_set_sleep_seconds(uint16_t idle_s, uint16_t goodbye_s);
esp_err_t nodelio_set_idle_wifi_seconds(uint16_t wifi_s);
esp_err_t nodelio_set_listen_idle_seconds(uint16_t listen_s);
/** 小智对话框宽（已按本屏夹取）。 */
uint16_t nodelio_ai_drawer_width(void);
uint16_t nodelio_ai_drawer_width_max(void);
/** 小智对话框总高 / 展示句数。 */
uint16_t nodelio_ai_drawer_height(void);
uint16_t nodelio_ai_drawer_height_max(void);
uint8_t nodelio_ai_drawer_lines(void);
/** 边框粗细 0–4。 */
uint8_t nodelio_ai_border_width(void);
/** 阴影宽度；未设置时浅色 12、深色 0。 */
uint8_t nodelio_ai_shadow_width(void);
/** 边框色档 0=默认 1=暖灰 2=绿。 */
uint8_t nodelio_ai_border_preset(void);
/** 获取当前卡片圆角像素值 (4, 16, 22)。 */
int nodelio_card_radius_px(void);

#ifdef __cplusplus
}
#endif
