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
    uint8_t brightness; /* 1–100；0=未设置用板级默认 */
    uint8_t volume;     /* 0–100；助手音频启用后生效 */
    uint8_t xiaozhi_en; /* 1=启用小智云对话（默认）；0=关闭 */
    uint8_t local_cmd_en; /* 1=启用本地 MultiNet 口令；0=默认不加载（省片内 DRAM） */
    uint8_t power_save_en; /* 1=允许息屏/断网省电（默认）；0=永不息屏/不断 WiFi */
    uint8_t controls_en;   /* 1=允许控家模块（默认）；0=首页不渲染控家，语音/MCP 降级提示 */
    uint8_t media_sleep_en; /* 1=播放中可息屏不停播（默认）；0=播放中常亮 */
    uint16_t idle_sleep_s;    /* 空闲息屏秒数，默认 60，范围 15–180 */
    uint16_t idle_wifi_s;     /* 空闲断网秒数，默认同息屏，范围 15–300；与息屏独立 */
    uint16_t goodbye_sleep_s; /* 退下后延迟息屏秒数，默认 15，范围 5–60 */
    uint16_t listen_idle_s; /* 唤醒后无对话自动退下秒数，默认 15，范围 10–30 */
    uint16_t media_pause_s; /* 暂停后关闭播放器秒数，默认 20，范围 10–120 */
} nodelio_nvs_config_t;

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

esp_err_t nodelio_nvs_init(void);
/** Flash 读写：禁止在 PSRAM 栈任务调用（LVGL / mcp_http / nl_* invoke）。用 cache_get。 */
esp_err_t nodelio_nvs_load(nodelio_nvs_config_t *out);
esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t *in);
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
/** 本地 MultiNet 口令；读 RAM 镜像，缺省关闭（不占片内 DRAM）。 */
bool nodelio_local_cmd_enabled(void);
esp_err_t nodelio_local_cmd_set_enabled(bool on);
/** 省电息屏断网开关；读 RAM 镜像，缺省开启。 */
bool nodelio_power_save_enabled(void);
esp_err_t nodelio_power_save_set_enabled(bool on);
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
/** 写入并钳位；某项为 0 则保持原值。含 flash，勿在 LVGL 任务调用。 */
esp_err_t nodelio_set_sleep_seconds(uint16_t idle_s, uint16_t goodbye_s);
esp_err_t nodelio_set_idle_wifi_seconds(uint16_t wifi_s);
esp_err_t nodelio_set_listen_idle_seconds(uint16_t listen_s);

#ifdef __cplusplus
}
#endif
