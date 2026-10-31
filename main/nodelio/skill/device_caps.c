#include "device_caps.h"
#include "board_info.h"

#include <stdio.h>
#include <string.h>

/* N0b：播控就绪后默认 1。回退 content-only：-DNL_CAPS_WITH_MEDIA=0 */
#ifndef NL_CAPS_WITH_MEDIA
#define NL_CAPS_WITH_MEDIA 1
#endif

/* 24 首 music_list + URL/actions 常见 8~20KB；过小会截断 JSON → 解析失败 → 模型瞎编「曲库故障」 */
#define NL_CAPS_MAX_BYTES 24576
#define NL_CAPS_MAX_ITEMS 24

static const char *const k_caps_base[] = {
    "content.text",
    "content.list",
    "content.music_list",
    "content.news_list",
    "content.weather",
    "content.error",
    "content.action",
    "action.invoke",
    "net.http_via_server",
    "input.touch",
    "input.voice",
    "display.color",
    NULL,
};

static const char *const k_caps_media[] = {
    "action.play",
    "action.pause",
    "media.play_local",
    NULL,
};

static const char *const k_surfaces_base[] = {
    "card",
    "home_tile",
    "notification",
    "voice_reply",
    NULL,
};

static const char *const k_surfaces_media[] = {
    "now_playing",
    NULL,
};

static bool list_has(const char *const *list, const char *name)
{
    if (!list || !name || !name[0]) {
        return false;
    }
    for (int i = 0; list[i]; i++) {
        if (strcmp(list[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool nodelio_caps_has_media(void)
{
    return NL_CAPS_WITH_MEDIA != 0;
}

bool nodelio_caps_has(const char *capability)
{
    if (capability && strcmp(capability, "input.touch") == 0) {
        return nodelio_board_get()->info.touch;
    }
    if (list_has(k_caps_base, capability)) {
        return true;
    }
    if (nodelio_caps_has_media() && list_has(k_caps_media, capability)) {
        return true;
    }
    return false;
}

bool nodelio_caps_has_surface(const char *surface)
{
    if (list_has(k_surfaces_base, surface)) {
        return true;
    }
    if (nodelio_caps_has_media() && list_has(k_surfaces_media, surface)) {
        return true;
    }
    return false;
}

int nodelio_caps_max_content_bytes(void)
{
    return NL_CAPS_MAX_BYTES;
}

int nodelio_caps_max_list_items(void)
{
    return NL_CAPS_MAX_ITEMS;
}

const char *nodelio_caps_profile(void)
{
    /* 仅本地模板名；线上上报不得带 profile（Protocol 00 §4.1 / PLAN K4） */
    return nodelio_display_profile()->nav_width ? "esp32-panel-1280x452" : "esp32-panel-480";
}

static int append_str_array(char *buf, size_t cap, int off, const char *const *items, bool *first)
{
    for (int i = 0; items && items[i]; i++) {
        if (strcmp(items[i], "input.touch") == 0 && !nodelio_board_get()->info.touch) {
            continue;
        }
        int n = snprintf(buf + off, (off < (int)cap) ? (cap - (size_t)off) : 0, "%s\"%s\"",
                         (*first) ? "" : ",", items[i]);
        if (n < 0) {
            return -1;
        }
        off += n;
        *first = false;
        if (off >= (int)cap) {
            return -1;
        }
    }
    return off;
}

int nodelio_caps_export_json(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 64) {
        return -1;
    }
    /* 线上形状：只写展开后的 capabilities（无 profile / disabled） */
    int off = snprintf(buf, buf_len,
                       "{\"schema_version\":1,\"protocol_version\":1,\"platform\":\"esp32-panel\","
                       "\"capabilities\":[");
    if (off < 0 || off >= (int)buf_len) {
        return -1;
    }
    bool first = true;
    off = append_str_array(buf, buf_len, off, k_caps_base, &first);
    if (off < 0) {
        return -1;
    }
    if (nodelio_caps_has_media()) {
        off = append_str_array(buf, buf_len, off, k_caps_media, &first);
        if (off < 0) {
            return -1;
        }
    }
    int n = snprintf(buf + off, buf_len - (size_t)off, "],\"surfaces\":[");
    if (n < 0) {
        return -1;
    }
    off += n;
    first = true;
    off = append_str_array(buf, buf_len, off, k_surfaces_base, &first);
    if (off < 0) {
        return -1;
    }
    if (nodelio_caps_has_media()) {
        off = append_str_array(buf, buf_len, off, k_surfaces_media, &first);
        if (off < 0) {
            return -1;
        }
    }
    if (nodelio_caps_has_media()) {
        /* play_local → limits.media 必填；fetch 与 net.http_via_server 一致 */
        n = snprintf(buf + off, buf_len - (size_t)off,
                     "],\"limits\":{\"max_content_bytes\":%d,\"max_list_items\":%d,"
                     "\"image\":{\"allowed\":true,\"max_edge_px\":480,\"formats\":[\"jpeg\",\"png\"]},"
                     "\"media\":{\"formats\":[\"audio/ogg\",\"audio/mpeg\",\"audio/mp4\",\"audio/aac\"],\"max_bitrate_kbps\":128,"
                     "\"max_duration_sec\":600,\"concurrent_with_voice_session\":false,"
                     "\"fetch\":\"http_via_server\"}}}",
                     NL_CAPS_MAX_BYTES, NL_CAPS_MAX_ITEMS);
    } else {
        /* 无 media.*：禁止出现 limits.media；也不写 disabled 凑数 */
        n = snprintf(buf + off, buf_len - (size_t)off,
                     "],\"limits\":{\"max_content_bytes\":%d,\"max_list_items\":%d,"
                     "\"image\":{\"allowed\":true,\"max_edge_px\":480,\"formats\":[\"jpeg\",\"png\"]}}}",
                     NL_CAPS_MAX_BYTES, NL_CAPS_MAX_ITEMS);
    }
    if (n < 0) {
        return -1;
    }
    off += n;
    if (off >= (int)buf_len) {
        return -1;
    }
    return off;
}
