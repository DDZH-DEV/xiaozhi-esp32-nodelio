#include "skill_media_cache.h"

#include "sd_library.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <esp_log.h>

namespace {

constexpr char kTag[] = "nl_mcache";

uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    if (!s) {
        return h;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

bool ensure_dirs(const char *dir)
{
    if (!dir || !dir[0]) {
        return false;
    }
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    const size_t n = strlen(tmp);
    for (size_t i = 1; i < n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        mkdir(tmp, 0755);
        tmp[i] = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

}  // namespace

extern "C" bool nodelio_media_cache_available(void)
{
    return nodelio_sd_is_mounted();
}

extern "C" bool nodelio_media_cache_dir(const char *title, const char *artist, char *out, size_t out_len)
{
    if (!out || out_len < 48 || !title || !title[0]) {
        return false;
    }
    if (!nodelio_media_cache_available()) {
        return false;
    }
    char key[192];
    snprintf(key, sizeof(key), "%s|%s", title, artist && artist[0] ? artist : "");
    const uint32_t h = fnv1a(key);
    const char *mp = nodelio_sd_mount_point();
    snprintf(out, out_len, "%s/NodeLio/cache/media/%08lx", mp ? mp : "/sdcard",
             static_cast<unsigned long>(h));
    return true;
}

extern "C" bool nodelio_media_cache_load_cover(const char *title, const char *artist, uint8_t *jpeg,
                                               size_t cap, size_t *out_n)
{
    if (out_n) {
        *out_n = 0;
    }
    if (!jpeg || cap < 64) {
        return false;
    }
    char dir[160];
    if (!nodelio_media_cache_dir(title, artist, dir, sizeof(dir))) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/cover.jpg", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const size_t n = fread(jpeg, 1, cap, f);
    fclose(f);
    if (n < 32) {
        return false;
    }
    if (out_n) {
        *out_n = n;
    }
    ESP_LOGI(kTag, "cover cache hit %s", path);
    return true;
}

extern "C" bool nodelio_media_cache_save_cover(const char *title, const char *artist,
                                               const uint8_t *jpeg, size_t jpeg_n)
{
    if (!jpeg || jpeg_n < 32) {
        return false;
    }
    char dir[160];
    if (!nodelio_media_cache_dir(title, artist, dir, sizeof(dir))) {
        return false;
    }
    if (!ensure_dirs(dir)) {
        ESP_LOGW(kTag, "mkdir fail %s", dir);
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/cover.jpg", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const size_t w = fwrite(jpeg, 1, jpeg_n, f);
    fclose(f);
    if (w != jpeg_n) {
        return false;
    }
    ESP_LOGI(kTag, "cover cache save %s (%u)", path, (unsigned)jpeg_n);
    return true;
}

extern "C" bool nodelio_media_cache_load_lyrics(const char *title, const char *artist, char *json,
                                                size_t cap)
{
    if (!json || cap < 16) {
        return false;
    }
    json[0] = '\0';
    char dir[160];
    if (!nodelio_media_cache_dir(title, artist, dir, sizeof(dir))) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/lyrics.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const size_t n = fread(json, 1, cap - 1, f);
    fclose(f);
    json[n] = '\0';
    if (n < 8) {
        json[0] = '\0';
        return false;
    }
    ESP_LOGI(kTag, "lyrics cache hit %s", path);
    return true;
}

extern "C" bool nodelio_media_cache_save_lyrics(const char *title, const char *artist, const char *json)
{
    if (!json || !json[0]) {
        return false;
    }
    char dir[160];
    if (!nodelio_media_cache_dir(title, artist, dir, sizeof(dir))) {
        return false;
    }
    if (!ensure_dirs(dir)) {
        return false;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/lyrics.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    const size_t n = strlen(json);
    const size_t w = fwrite(json, 1, n, f);
    fclose(f);
    ESP_LOGI(kTag, "lyrics cache save %s (%u)", path, (unsigned)w);
    return w == n;
}
