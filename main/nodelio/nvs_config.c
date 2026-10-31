#include "nvs_config.h"
#include "board_info.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>

static const char *TAG = "nl_nvs";

/* LVGL 任务栈在 PSRAM：禁 cache 读 flash 时不可用 → 回调内禁止 nvs_load/save */
static nodelio_nvs_config_t s_ram;
static bool s_ram_ok;
static _lock_t s_config_lock;

/** 可重建缓存：腾空间保住 server_url / 凭证。NVS 仅 16KB，易被 lan/apps/hist 占满。 */
static void reclaim_ephemeral(nvs_handle_t h)
{
    (void)nvs_erase_key(h, "lan_devs");
    (void)nvs_erase_key(h, "apps_list");
    (void)nvs_erase_key(h, "oc_dyn");
    (void)nvs_erase_key(h, "conn_hist");
}

#define NL_NVS_HEADROOM_MIN 64

static void log_nvs_stats(const char *where)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "nvs %s used=%u free=%u ns=%u", where ? where : "", (unsigned)st.used_entries,
             (unsigned)st.free_entries, (unsigned)st.namespace_count);
}

bool nodelio_nvs_has_headroom(void)
{
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) != ESP_OK) {
        return false;
    }
    return st.free_entries >= NL_NVS_HEADROOM_MIN;
}

static esp_err_t set_str_checked(nvs_handle_t h, const char *key, const char *val)
{
    esp_err_t err = nvs_set_str(h, key, val ? val : "");
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        reclaim_ephemeral(h);
        err = nvs_set_str(h, key, val ? val : "");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_str %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

static void ram_put(const nodelio_nvs_config_t *in)
{
    if (!in) {
        return;
    }
    s_ram = *in;
    s_ram_ok = true;
}

void nodelio_nvs_cache_put(const nodelio_nvs_config_t *in)
{
    _lock_acquire(&s_config_lock);
    ram_put(in);
    _lock_release(&s_config_lock);
}

static void copy_preferences(nodelio_nvs_config_t* out, const nodelio_nvs_config_t* in) {
    // Preferences are the tail of this RAM structure; connection fields precede it.
    const size_t offset = offsetof(nodelio_nvs_config_t, brightness);
    memcpy((uint8_t*)out + offset, (const uint8_t*)in + offset, sizeof(*out) - offset);
}

void nodelio_nvs_cache_put_preferences(const nodelio_nvs_config_t* in) {
    if (!in)
        return;
    _lock_acquire(&s_config_lock);
    copy_preferences(&s_ram, in);
    _lock_release(&s_config_lock);
}

void nodelio_nvs_cache_get(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    _lock_acquire(&s_config_lock);
    if (s_ram_ok) {
        *out = s_ram;
        _lock_release(&s_config_lock);
        return;
    }
    _lock_release(&s_config_lock);
    memset(out, 0, sizeof(*out));
    out->xiaozhi_en = 1;
    out->local_cmd_en = 0;
    out->power_save_en = 1;
    out->controls_en = 1;
    out->media_sleep_en = 1;
    out->idle_sleep_s = NODELIO_IDLE_SLEEP_DEFAULT;
    out->idle_wifi_s = NODELIO_IDLE_WIFI_DEFAULT;
    out->goodbye_sleep_s = NODELIO_BYE_SLEEP_DEFAULT;
    out->listen_idle_s = NODELIO_LISTEN_IDLE_DEFAULT;
    out->media_pause_s = NODELIO_MEDIA_PAUSE_DEFAULT;
    out->media_wake_duck_pct = NODELIO_MEDIA_WAKE_DUCK_DEFAULT;
    out->text_scale = NODELIO_TEXT_SCALE_DEFAULT;
}

static esp_err_t get_str(nvs_handle_t h, const char *key, char *buf, size_t len)
{
    size_t required = len;
    esp_err_t err = nvs_get_str(h, key, buf, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (len) {
            buf[0] = '\0';
        }
        return ESP_OK;
    }
    return err;
}

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi, uint16_t def)
{
    if (v == 0) {
        return def;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi, uint8_t def)
{
    if (v == 0) {
        return def;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static void apply_defaults(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    if (out->xiaozhi_en > 1) {
        out->xiaozhi_en = 1;
    }
    if (out->local_cmd_en > 1) {
        out->local_cmd_en = 0;
    }
    if (out->power_save_en > 1) {
        out->power_save_en = 1;
    }
    if (out->controls_en > 1) {
        out->controls_en = 1;
    }
    if (out->media_sleep_en > 1) {
        out->media_sleep_en = 1;
    }
    out->idle_sleep_s =
        clamp_u16(out->idle_sleep_s, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX,
                  NODELIO_IDLE_SLEEP_DEFAULT);
    out->idle_wifi_s =
        clamp_u16(out->idle_wifi_s, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX,
                  out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT);
    out->goodbye_sleep_s =
        clamp_u16(out->goodbye_sleep_s, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX,
                  NODELIO_BYE_SLEEP_DEFAULT);
    out->listen_idle_s =
        clamp_u16(out->listen_idle_s, NODELIO_LISTEN_IDLE_MIN, NODELIO_LISTEN_IDLE_MAX,
                  NODELIO_LISTEN_IDLE_DEFAULT);
    out->media_pause_s =
        clamp_u16(out->media_pause_s, NODELIO_MEDIA_PAUSE_MIN, NODELIO_MEDIA_PAUSE_MAX,
                  NODELIO_MEDIA_PAUSE_DEFAULT);
    out->media_wake_duck_pct =
        clamp_u8(out->media_wake_duck_pct, NODELIO_MEDIA_WAKE_DUCK_MIN, NODELIO_MEDIA_WAKE_DUCK_MAX,
                 NODELIO_MEDIA_WAKE_DUCK_DEFAULT);
    if (out->text_scale > NODELIO_TEXT_SCALE_MAX) {
        out->text_scale = NODELIO_TEXT_SCALE_DEFAULT;
    }
    if (out->ai_pos_set > 1) {
        out->ai_pos_set = 0;
    }
}

/* 避免 save 路径把「关闭小智/省电」(0) 误当成未初始化 */
static void apply_defaults_for_save(nodelio_nvs_config_t *out)
{
    if (!out) {
        return;
    }
    out->idle_sleep_s =
        clamp_u16(out->idle_sleep_s, NODELIO_IDLE_SLEEP_MIN, NODELIO_IDLE_SLEEP_MAX,
                  NODELIO_IDLE_SLEEP_DEFAULT);
    out->idle_wifi_s =
        clamp_u16(out->idle_wifi_s, NODELIO_IDLE_WIFI_MIN, NODELIO_IDLE_WIFI_MAX,
                  out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT);
    out->goodbye_sleep_s =
        clamp_u16(out->goodbye_sleep_s, NODELIO_BYE_SLEEP_MIN, NODELIO_BYE_SLEEP_MAX,
                  NODELIO_BYE_SLEEP_DEFAULT);
    out->listen_idle_s =
        clamp_u16(out->listen_idle_s, NODELIO_LISTEN_IDLE_MIN, NODELIO_LISTEN_IDLE_MAX,
                  NODELIO_LISTEN_IDLE_DEFAULT);
    out->media_pause_s =
        clamp_u16(out->media_pause_s, NODELIO_MEDIA_PAUSE_MIN, NODELIO_MEDIA_PAUSE_MAX,
                  NODELIO_MEDIA_PAUSE_DEFAULT);
    out->media_wake_duck_pct =
        clamp_u8(out->media_wake_duck_pct, NODELIO_MEDIA_WAKE_DUCK_MIN, NODELIO_MEDIA_WAKE_DUCK_MAX,
                 NODELIO_MEDIA_WAKE_DUCK_DEFAULT);
    if (out->xiaozhi_en > 1) {
        out->xiaozhi_en = 1;
    }
    if (out->local_cmd_en > 1) {
        out->local_cmd_en = 0;
    }
    if (out->power_save_en > 1) {
        out->power_save_en = 1;
    }
    if (out->controls_en > 1) {
        out->controls_en = 1;
    }
    if (out->media_sleep_en > 1) {
        out->media_sleep_en = 1;
    }
    if (out->text_scale > NODELIO_TEXT_SCALE_MAX) {
        out->text_scale = NODELIO_TEXT_SCALE_DEFAULT;
    }
    if (out->ai_pos_set > 1) {
        out->ai_pos_set = 0;
    }
}

esp_err_t nodelio_nvs_init(void)
{
    /* Xiaozhi already initialized NVS; do not erase on version mismatch. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return err;
    }
    return (err == ESP_OK || err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

static esp_err_t load_config(nodelio_nvs_config_t* out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->xiaozhi_en = 1;
    out->local_cmd_en = 0;
    out->power_save_en = 1;
    out->controls_en = 1;
    out->media_sleep_en = 1;
    out->idle_sleep_s = NODELIO_IDLE_SLEEP_DEFAULT;
    out->idle_wifi_s = NODELIO_IDLE_WIFI_DEFAULT;
    out->goodbye_sleep_s = NODELIO_BYE_SLEEP_DEFAULT;
    out->listen_idle_s = NODELIO_LISTEN_IDLE_DEFAULT;
    out->media_pause_s = NODELIO_MEDIA_PAUSE_DEFAULT;
    out->media_wake_duck_pct = NODELIO_MEDIA_WAKE_DUCK_DEFAULT;
    out->text_scale = NODELIO_TEXT_SCALE_DEFAULT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ram_put(out);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    get_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    get_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    get_str(h, "server_url", out->server_url, sizeof(out->server_url));
    get_str(h, "device_key", out->device_key, sizeof(out->device_key));
    get_str(h, "asst_ws", out->assistant_ws_url, sizeof(out->assistant_ws_url));
    get_str(h, "device_id", out->device_id, sizeof(out->device_id));
    get_str(h, "owner", out->owner, sizeof(out->owner));
    get_str(h, "mqtt_server", out->mqtt_server, sizeof(out->mqtt_server));
    get_str(h, "mqtt_port", out->mqtt_port, sizeof(out->mqtt_port));
    get_str(h, "mqtt_user", out->mqtt_user, sizeof(out->mqtt_user));
    get_str(h, "mqtt_pass", out->mqtt_pass, sizeof(out->mqtt_pass));
    get_str(h, "topic_prefix", out->topic_prefix, sizeof(out->topic_prefix));
    {
        uint8_t v = 0;
        if (nvs_get_u8(h, "brightness", &v) == ESP_OK) {
            out->brightness = v;
        }
        v = 0;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK) {
            out->volume = v;
        }
        v = 1;
        if (nvs_get_u8(h, "xz_en", &v) == ESP_OK) {
            out->xiaozhi_en = v ? 1 : 0;
        } else {
            out->xiaozhi_en = 1;
        }
        out->local_cmd_en = 0; /* MultiNet 已移除；忽略旧 NVS lc_en */
        v = 1;
        if (nvs_get_u8(h, "ps_en", &v) == ESP_OK) {
            out->power_save_en = v ? 1 : 0;
        } else {
            out->power_save_en = 1;
        }
        v = 1;
        if (nvs_get_u8(h, "ctrl_en", &v) == ESP_OK) {
            out->controls_en = v ? 1 : 0;
        } else {
            out->controls_en = 1;
        }
        uint16_t u16 = 0;
        if (nvs_get_u16(h, "idle_slp", &u16) == ESP_OK) {
            out->idle_sleep_s = u16;
        }
        u16 = 0;
        if (nvs_get_u16(h, "wifi_slp", &u16) == ESP_OK) {
            out->idle_wifi_s = u16;
        } else {
            /* 旧固件无此键：与息屏同值，行为与原先「息屏即断网」一致 */
            out->idle_wifi_s = out->idle_sleep_s ? out->idle_sleep_s : NODELIO_IDLE_WIFI_DEFAULT;
        }
        u16 = 0;
        if (nvs_get_u16(h, "bye_slp", &u16) == ESP_OK) {
            out->goodbye_sleep_s = u16;
        }
        u16 = 0;
        if (nvs_get_u16(h, "lis_idle", &u16) == ESP_OK) {
            out->listen_idle_s = u16;
        }
        v = 1;
        if (nvs_get_u8(h, "md_slp", &v) == ESP_OK) {
            out->media_sleep_en = v ? 1 : 0;
        } else {
            out->media_sleep_en = 1;
        }
        u16 = 0;
        if (nvs_get_u16(h, "md_pause", &u16) == ESP_OK) {
            out->media_pause_s = u16;
        }
        v = 0;
        if (nvs_get_u8(h, "md_duck", &v) == ESP_OK) {
            out->media_wake_duck_pct = v;
        }
        v = NODELIO_TEXT_SCALE_DEFAULT;
        if (nvs_get_u8(h, "txt_scl", &v) == ESP_OK) {
            out->text_scale = v;
        } else {
            out->text_scale = NODELIO_TEXT_SCALE_DEFAULT;
        }
        v = 0;
        if (nvs_get_u8(h, "ai_pos", &v) == ESP_OK && v == 1) {
            int16_t x = 0;
            int16_t y = 0;
            if (nvs_get_i16(h, "ai_x", &x) == ESP_OK && nvs_get_i16(h, "ai_y", &y) == ESP_OK) {
                out->ai_pos_set = 1;
                out->ai_x = x;
                out->ai_y = y;
            }
        }
    }
    nvs_close(h);
    apply_defaults(out);
    ram_put(out);
    return ESP_OK;
}

static esp_err_t save_config(const nodelio_nvs_config_t* in, bool preferences_only) {
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    nodelio_nvs_config_t tmp = *in;
    apply_defaults_for_save(&tmp);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* 凭证优先：先写 server_url，避免后面 blob/其它键占满后静默留旧地址 */
    esp_err_t first = ESP_OK;
    esp_err_t e;
#define NL_ACCUM(expr)                                                                                 \
    do {                                                                                               \
        e = (expr);                                                                                    \
        if (e != ESP_OK && first == ESP_OK) {                                                          \
            first = e;                                                                                 \
        }                                                                                              \
    } while (0)

    if (!preferences_only) {
        /* 先清可重建 blob，避免写完凭证后无空页，下次开机 nvs_flash_erase 把连接冲掉 */
        reclaim_ephemeral(h);
        NL_ACCUM(set_str_checked(h, "server_url", tmp.server_url));
        NL_ACCUM(set_str_checked(h, "owner", tmp.owner));
        NL_ACCUM(set_str_checked(h, "device_id", tmp.device_id));
        NL_ACCUM(set_str_checked(h, "device_key", tmp.device_key));
        NL_ACCUM(set_str_checked(h, "wifi_ssid", tmp.wifi_ssid));
        NL_ACCUM(set_str_checked(h, "wifi_pass", tmp.wifi_pass));
        NL_ACCUM(set_str_checked(h, "asst_ws", tmp.assistant_ws_url));
        NL_ACCUM(set_str_checked(h, "mqtt_server", tmp.mqtt_server));
        NL_ACCUM(set_str_checked(h, "mqtt_port", tmp.mqtt_port));
        NL_ACCUM(set_str_checked(h, "mqtt_user", tmp.mqtt_user));
        NL_ACCUM(set_str_checked(h, "mqtt_pass", tmp.mqtt_pass));
        NL_ACCUM(set_str_checked(h, "topic_prefix", tmp.topic_prefix));
    }
    NL_ACCUM(nvs_set_u8(h, "brightness", tmp.brightness));
    NL_ACCUM(nvs_set_u8(h, "volume", tmp.volume));
    NL_ACCUM(nvs_set_u8(h, "xz_en", tmp.xiaozhi_en ? 1 : 0));
    NL_ACCUM(nvs_set_u8(h, "lc_en", tmp.local_cmd_en ? 1 : 0));
    NL_ACCUM(nvs_set_u8(h, "ps_en", tmp.power_save_en ? 1 : 0));
    NL_ACCUM(nvs_set_u8(h, "ctrl_en", tmp.controls_en ? 1 : 0));
    NL_ACCUM(nvs_set_u8(h, "md_slp", tmp.media_sleep_en ? 1 : 0));
    NL_ACCUM(nvs_set_u16(h, "idle_slp", tmp.idle_sleep_s));
    NL_ACCUM(nvs_set_u16(h, "wifi_slp", tmp.idle_wifi_s));
    NL_ACCUM(nvs_set_u16(h, "bye_slp", tmp.goodbye_sleep_s));
    NL_ACCUM(nvs_set_u16(h, "lis_idle", tmp.listen_idle_s));
    NL_ACCUM(nvs_set_u16(h, "md_pause", tmp.media_pause_s));
    NL_ACCUM(nvs_set_u8(h, "md_duck", tmp.media_wake_duck_pct));
    NL_ACCUM(nvs_set_u8(h, "txt_scl", tmp.text_scale));
    NL_ACCUM(nvs_set_u8(h, "ai_pos", tmp.ai_pos_set ? 1 : 0));
    NL_ACCUM(nvs_set_i16(h, "ai_x", tmp.ai_x));
    NL_ACCUM(nvs_set_i16(h, "ai_y", tmp.ai_y));
#undef NL_ACCUM

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "commit: %s", esp_err_to_name(err));
        return err;
    }
    if (first != ESP_OK) {
        ESP_LOGW(TAG, "save partial fail: %s (server_url may be stale)", esp_err_to_name(first));
        return first;
    }
    ram_put(&tmp);
    log_nvs_stats(preferences_only ? "prefs" : "conn");
    if (!preferences_only && !nodelio_nvs_has_headroom()) {
        nvs_handle_t h2;
        if (nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h2) == ESP_OK) {
            reclaim_ephemeral(h2);
            (void)nvs_commit(h2);
            nvs_close(h2);
            log_nvs_stats("reclaim");
        }
    }
    return ESP_OK;
}

esp_err_t nodelio_nvs_load(nodelio_nvs_config_t* out) {
    _lock_acquire(&s_config_lock);
    esp_err_t err = load_config(out);
    _lock_release(&s_config_lock);
    return err;
}

esp_err_t nodelio_nvs_save(const nodelio_nvs_config_t* in) {
    _lock_acquire(&s_config_lock);
    esp_err_t err = save_config(in, false);
    _lock_release(&s_config_lock);
    return err;
}

esp_err_t nodelio_nvs_save_preferences(const nodelio_nvs_config_t* in) {
    if (!in)
        return ESP_ERR_INVALID_ARG;
    _lock_acquire(&s_config_lock);
    nodelio_nvs_config_t current;
    esp_err_t err = load_config(&current);
    if (err == ESP_OK) {
        copy_preferences(&current, in);
        err = save_config(&current, true);
    }
    _lock_release(&s_config_lock);
    return err;
}

esp_err_t nodelio_nvs_save_registration(const nodelio_nvs_config_t* in) {
    if (!in)
        return ESP_ERR_INVALID_ARG;
    _lock_acquire(&s_config_lock);
    nodelio_nvs_config_t current;
    esp_err_t err = load_config(&current);
    if (err == ESP_OK && (strcmp(current.server_url, in->server_url) != 0 ||
                          strcmp(current.owner, in->owner) != 0)) {
        // An HTTP response for the previous server/account must not undo a user save.
        ESP_LOGW(TAG, "Discard registration settings for a replaced connection");
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
#define COPY_FIELD(field) memcpy(current.field, in->field, sizeof(current.field))
        COPY_FIELD(device_id);
        COPY_FIELD(device_key);
        COPY_FIELD(mqtt_server);
        COPY_FIELD(mqtt_port);
        COPY_FIELD(mqtt_user);
        COPY_FIELD(mqtt_pass);
        COPY_FIELD(topic_prefix);
#undef COPY_FIELD
        err = save_config(&current, false);
    }
    _lock_release(&s_config_lock);
    return err;
}

esp_err_t nodelio_nvs_save_connection(const nodelio_nvs_config_t* in) {
    if (!in)
        return ESP_ERR_INVALID_ARG;
    _lock_acquire(&s_config_lock);
    nodelio_nvs_config_t current;
    esp_err_t err = load_config(&current);
    if (err == ESP_OK) {
        nodelio_nvs_config_t updated = *in;
        copy_preferences(&updated, &current);
        /* 连接表单不带注册凭证；空字段勿覆盖已有 device_id / key */
        if (updated.device_id[0] == '\0') {
            memcpy(updated.device_id, current.device_id, sizeof(updated.device_id));
        }
        if (updated.device_key[0] == '\0') {
            memcpy(updated.device_key, current.device_key, sizeof(updated.device_key));
        }
        if (strcmp(updated.server_url, current.server_url) == 0 &&
            strcmp(updated.owner, current.owner) == 0) {
            if (updated.mqtt_server[0] == '\0') {
                memcpy(updated.mqtt_server, current.mqtt_server, sizeof(updated.mqtt_server));
                memcpy(updated.mqtt_port, current.mqtt_port, sizeof(updated.mqtt_port));
                memcpy(updated.mqtt_user, current.mqtt_user, sizeof(updated.mqtt_user));
                memcpy(updated.mqtt_pass, current.mqtt_pass, sizeof(updated.mqtt_pass));
                memcpy(updated.topic_prefix, current.topic_prefix, sizeof(updated.topic_prefix));
            }
            if (updated.assistant_ws_url[0] == '\0') {
                memcpy(updated.assistant_ws_url, current.assistant_ws_url,
                       sizeof(updated.assistant_ws_url));
            }
        }
        err = save_config(&updated, false);
    }
    if (err == ESP_OK) {
        err = load_config(&current);
        if (err == ESP_OK && (strcmp(current.server_url, in->server_url) != 0 ||
                              strcmp(current.owner, in->owner) != 0 ||
                              strcmp(current.wifi_ssid, in->wifi_ssid) != 0 ||
                              strcmp(current.wifi_pass, in->wifi_pass) != 0)) {
            ESP_LOGE(TAG, "Connection readback does not match saved settings");
            err = ESP_FAIL;
        }
    }
    _lock_release(&s_config_lock);
    return err;
}

bool nodelio_xiaozhi_enabled(void)
{
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.xiaozhi_en != 0;
}

esp_err_t nodelio_xiaozhi_set_enabled(bool on)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.xiaozhi_en = on ? 1 : 0;
    return nodelio_nvs_save_preferences(&cfg);
}

bool nodelio_local_cmd_enabled(void)
{
    /* MultiNet 已从固件拿掉：始终关闭，忽略 NVS 旧值。 */
    (void)s_ram_ok;
    return false;
}

esp_err_t nodelio_local_cmd_set_enabled(bool on)
{
    (void)on;
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg.local_cmd_en == 0) {
        return ESP_OK;
    }
    cfg.local_cmd_en = 0;
    return nodelio_nvs_save_preferences(&cfg);
}

bool nodelio_power_save_enabled(void)
{
    if (!nodelio_display_profile()->network_sleep) {
        return false;
    }
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.power_save_en != 0;
}

esp_err_t nodelio_power_save_set_enabled(bool on)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.power_save_en = on ? 1 : 0;
    return nodelio_nvs_save_preferences(&cfg);
}

bool nodelio_controls_enabled(void)
{
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.controls_en != 0;
}

esp_err_t nodelio_controls_set_enabled(bool on)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.controls_en = on ? 1 : 0;
    return nodelio_nvs_save_preferences(&cfg);
}

uint16_t nodelio_idle_sleep_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_IDLE_SLEEP_DEFAULT;
    }
    return s_ram.idle_sleep_s;
}

uint16_t nodelio_idle_wifi_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_IDLE_WIFI_DEFAULT;
    }
    return s_ram.idle_wifi_s;
}

uint16_t nodelio_goodbye_sleep_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_BYE_SLEEP_DEFAULT;
    }
    return s_ram.goodbye_sleep_s;
}

uint16_t nodelio_listen_idle_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_LISTEN_IDLE_DEFAULT;
    }
    return s_ram.listen_idle_s;
}

bool nodelio_media_sleep_enabled(void)
{
    if (!s_ram_ok) {
        return true;
    }
    return s_ram.media_sleep_en != 0;
}

uint16_t nodelio_media_pause_seconds(void)
{
    if (!s_ram_ok) {
        return NODELIO_MEDIA_PAUSE_DEFAULT;
    }
    return s_ram.media_pause_s ? s_ram.media_pause_s : NODELIO_MEDIA_PAUSE_DEFAULT;
}

uint8_t nodelio_media_wake_duck_pct(void)
{
    if (!s_ram_ok) {
        return NODELIO_MEDIA_WAKE_DUCK_DEFAULT;
    }
    return s_ram.media_wake_duck_pct ? s_ram.media_wake_duck_pct
                                     : NODELIO_MEDIA_WAKE_DUCK_DEFAULT;
}

uint8_t nodelio_text_scale(void)
{
    if (!s_ram_ok) {
        return NODELIO_TEXT_SCALE_DEFAULT;
    }
    return s_ram.text_scale > NODELIO_TEXT_SCALE_MAX ? NODELIO_TEXT_SCALE_DEFAULT
                                                     : s_ram.text_scale;
}

esp_err_t nodelio_text_scale_set(uint8_t scale)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    cfg.text_scale = scale > NODELIO_TEXT_SCALE_MAX ? NODELIO_TEXT_SCALE_DEFAULT : scale;
    return nodelio_nvs_save_preferences(&cfg);
}

esp_err_t nodelio_set_sleep_seconds(uint16_t idle_s, uint16_t goodbye_s)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (idle_s != 0) {
        cfg.idle_sleep_s = idle_s;
    }
    if (goodbye_s != 0) {
        cfg.goodbye_sleep_s = goodbye_s;
    }
    apply_defaults_for_save(&cfg);
    return nodelio_nvs_save_preferences(&cfg);
}

esp_err_t nodelio_set_idle_wifi_seconds(uint16_t wifi_s)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (wifi_s != 0) {
        cfg.idle_wifi_s = wifi_s;
    }
    apply_defaults_for_save(&cfg);
    return nodelio_nvs_save_preferences(&cfg);
}

esp_err_t nodelio_set_listen_idle_seconds(uint16_t listen_s)
{
    nodelio_nvs_config_t cfg;
    esp_err_t err = nodelio_nvs_load(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    if (listen_s != 0) {
        cfg.listen_idle_s = listen_s;
    }
    apply_defaults_for_save(&cfg);
    return nodelio_nvs_save_preferences(&cfg);
}

esp_err_t nodelio_nvs_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void nodelio_nvs_clear_mqtt(nodelio_nvs_config_t *cfg)
{
    if (!cfg) {
        return;
    }
    cfg->mqtt_server[0] = '\0';
    cfg->mqtt_port[0] = '\0';
    cfg->mqtt_user[0] = '\0';
    cfg->mqtt_pass[0] = '\0';
    cfg->topic_prefix[0] = '\0';
}

bool nodelio_nvs_has_wifi(const nodelio_nvs_config_t *cfg)
{
    return cfg && cfg->wifi_ssid[0] != '\0';
}

static nodelio_conn_hist_t s_conn_hist;
static bool s_conn_hist_ok;
static const char *CONN_HIST_KEY = "conn_hist";

void nodelio_conn_hist_cache_get(nodelio_conn_hist_t *out)
{
    if (!out) {
        return;
    }
    if (s_conn_hist_ok) {
        *out = s_conn_hist;
        return;
    }
    memset(out, 0, sizeof(*out));
}

void nodelio_conn_hist_cache_put(const nodelio_conn_hist_t *in)
{
    if (!in) {
        return;
    }
    s_conn_hist = *in;
    s_conn_hist_ok = true;
}

esp_err_t nodelio_conn_hist_load(nodelio_conn_hist_t *out)
{
    nodelio_conn_hist_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        if (out) {
            *out = tmp;
        }
        s_conn_hist = tmp;
        s_conn_hist_ok = true;
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, CONN_HIST_KEY, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len < sizeof(uint8_t)) {
        memset(&tmp, 0, sizeof(tmp));
    } else if (tmp.n > NODELIO_CONN_HIST_MAX) {
        tmp.n = NODELIO_CONN_HIST_MAX;
    }
    s_conn_hist = tmp;
    s_conn_hist_ok = true;
    if (out) {
        *out = tmp;
    }
    return ESP_OK;
}

esp_err_t nodelio_conn_hist_save(const nodelio_conn_hist_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }
    nodelio_conn_hist_t tmp = *in;
    if (tmp.n > NODELIO_CONN_HIST_MAX) {
        tmp.n = NODELIO_CONN_HIST_MAX;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NODELIO_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (!nodelio_nvs_has_headroom()) {
        ESP_LOGW(TAG, "conn_hist skip: nvs low headroom");
        nvs_close(h);
        s_conn_hist = tmp;
        s_conn_hist_ok = true;
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    err = nvs_set_blob(h, CONN_HIST_KEY, &tmp, sizeof(tmp));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        reclaim_ephemeral(h);
        (void)nvs_erase_key(h, CONN_HIST_KEY);
        err = nvs_set_blob(h, CONN_HIST_KEY, &tmp, sizeof(tmp));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    } else {
        ESP_LOGW(TAG, "conn_hist save: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_conn_hist = tmp;
        s_conn_hist_ok = true;
    }
    return err;
}

void nodelio_conn_hist_push(nodelio_conn_hist_t *h, bool https, const char *host, const char *port,
                            const char *owner)
{
    if (!h || !host || !host[0]) {
        return;
    }
    const char *p = (port && port[0]) ? port : "8899";
    const char *o = owner ? owner : "";
    int found = -1;
    for (int i = 0; i < (int)h->n && i < NODELIO_CONN_HIST_MAX; i++) {
        if ((h->e[i].https ? 1 : 0) == (https ? 1 : 0) && strcmp(h->e[i].host, host) == 0 &&
            strcmp(h->e[i].port, p) == 0) {
            found = i;
            break;
        }
    }
    nodelio_conn_hist_entry_t ent;
    memset(&ent, 0, sizeof(ent));
    snprintf(ent.host, sizeof(ent.host), "%s", host);
    snprintf(ent.port, sizeof(ent.port), "%s", p);
    snprintf(ent.owner, sizeof(ent.owner), "%s", o);
    ent.https = https ? 1 : 0;
    if (found >= 0) {
        for (int i = found; i > 0; i--) {
            h->e[i] = h->e[i - 1];
        }
        h->e[0] = ent;
        return;
    }
    uint8_t n = h->n;
    if (n < NODELIO_CONN_HIST_MAX) {
        n++;
    }
    for (int i = (int)n - 1; i > 0; i--) {
        h->e[i] = h->e[i - 1];
    }
    h->e[0] = ent;
    h->n = n;
}
