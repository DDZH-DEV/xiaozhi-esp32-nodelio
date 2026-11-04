#include "sd_library.h"

#include "board.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sdkconfig.h>

#if !CONFIG_NODELIO_SD_MUSIC

/* 未启用内存卡曲库的板型：保持同一套 API，全部按「无卡」降级。
 * 这样 skill/MCP/语音层无需到处 #if，也不会把 sdmmc 依赖带进其它板。 */
extern "C" bool nodelio_sd_available(void) { return false; }
extern "C" esp_err_t nodelio_sd_mount(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool nodelio_sd_is_mounted(void) { return false; }
extern "C" esp_err_t nodelio_sd_rescan(void) { return ESP_ERR_INVALID_STATE; }
extern "C" int nodelio_sd_track_count(void) { return 0; }
extern "C" int nodelio_sd_batch(void) { return 0; }
extern "C" void nodelio_sd_set_batch(int batch) { (void)batch; }
extern "C" int nodelio_sd_batch_count(void) { return 0; }
extern "C" int nodelio_sd_fill_envelope(nl_content_envelope_t *env, int *total_out)
{
    (void)env;
    if (total_out) {
        *total_out = 0;
    }
    return 0;
}
extern "C" bool nodelio_sd_resolve_url(const char *url, char *path, size_t path_len)
{
    (void)url;
    (void)path;
    (void)path_len;
    return false;
}
extern "C" void nodelio_sd_make_url(const char *path, char *url, size_t url_len)
{
    (void)path;
    if (url && url_len) {
        url[0] = '\0';
    }
}
extern "C" void nodelio_sd_next_batch(void) {}

#else /* CONFIG_NODELIO_SD_MUSIC */

#include <dirent.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

#define TAG "nl_sd"

/** 挂载点：与多数 ESP-IDF 例程一致，用户把卡格成 FAT32/exFAT 后可直接拷歌。 */
#define NL_SD_MOUNT_POINT "/sdcard"
#define NL_SD_URL_PREFIX "sd://"

/* 扫描上限：一张 32GB 卡塞满无损也不止这么多；限制是为了 PSRAM 占用可预期。
 * 每项 160B，200 项约 31KB PSRAM，只在第一次扫描时分配。 */
#define NL_SD_MAX_FILES 200
#define NL_SD_PATH_MAX 160
#define NL_SD_SCAN_DEPTH 3

/* 批次大小与 Envelope 的 track 上限对齐，避免二次分配。 */
#define NL_SD_BATCH_MAX NL_ENV_TRACK_MAX

namespace {

struct sd_state_t {
    bool mounted;
    bool scan_tried;
    int count;
    int batch;
    char (*paths)[NL_SD_PATH_MAX];
};

sd_state_t s_sd = {false, false, 0, 0, nullptr};

bool ext_supported(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) {
        return false;
    }
    char ext[8];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(ext)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = dot[1 + i];
        ext[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    ext[n] = '\0';
    /* 只列解码链路真能放的容器：mp3 / m4a / wav。
     * 曲库列表一旦列出就必须能播，宁可少列也不要「点了没声」。
     * flac/aac/ogg 目前没有可用的分帧解析器，暂不列出。 */
    return strcmp(ext, "mp3") == 0 || strcmp(ext, "m4a") == 0 || strcmp(ext, "wav") == 0;
}

/** 「01 - 周杰伦 - 晴天.mp3」→「01 - 周杰伦 - 晴天」；标题由 UI 直接显示。 */
void path_to_title(const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, out_len, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';
    }
}

/** 目录项按路径字典序；仅在扫描结束时排一次。 */
int cmp_path(const void *a, const void *b)
{
    return strcmp(static_cast<const char *>(a), static_cast<const char *>(b));
}

bool ensure_paths(void)
{
    if (s_sd.paths) {
        return true;
    }
    s_sd.paths = static_cast<char (*)[NL_SD_PATH_MAX]>(
        heap_caps_calloc(NL_SD_MAX_FILES, NL_SD_PATH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_sd.paths) {
        s_sd.paths = static_cast<char (*)[NL_SD_PATH_MAX]>(
            calloc(NL_SD_MAX_FILES, NL_SD_PATH_MAX));
    }
    return s_sd.paths != nullptr;
}

void scan_dir(const char *dir, int depth)
{
    if (depth > NL_SD_SCAN_DEPTH || s_sd.count >= NL_SD_MAX_FILES) {
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(d)) != nullptr && s_sd.count < NL_SD_MAX_FILES) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char full[NL_SD_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n <= 0 || n >= static_cast<int>(sizeof(full))) {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            scan_dir(full, depth + 1);
            continue;
        }
        if (!ext_supported(ent->d_name)) {
            continue;
        }
        snprintf(s_sd.paths[s_sd.count], NL_SD_PATH_MAX, "%s", full);
        s_sd.count++;
    }
    closedir(d);
}

/** 上电/插卡时把 PWRn 拉高有效，未挂载则断电省电并避免误识别。 */
void set_card_power(const SdCardPins &pins, bool on)
{
    if (pins.power_en < 0) {
        return;
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pins.power_en;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&cfg) != ESP_OK) {
        return;
    }
    const bool level = pins.power_active_low ? !on : on;
    gpio_set_level(static_cast<gpio_num_t>(pins.power_en), level ? 1 : 0);
}

}  // namespace

bool nodelio_sd_available(void)
{
    const SdCardPins pins = Board::GetInstance().GetSdCardPins();
    return pins.clk >= 0 && pins.cmd >= 0 && pins.d0 >= 0;
}

bool nodelio_sd_is_mounted(void)
{
    return s_sd.mounted;
}

esp_err_t nodelio_sd_mount(void)
{
    if (s_sd.mounted) {
        return ESP_OK;
    }
    const SdCardPins pins = Board::GetInstance().GetSdCardPins();
    if (pins.clk < 0 || pins.cmd < 0 || pins.d0 < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    set_card_power(pins, true);
    /* 卡内电源稳定后再发 CMD0/CMD8；部分卡上电 100ms 内不响应。 */
    vTaskDelay(pdMS_TO_TICKS(120));
    if (pins.card_detect >= 0) {
        gpio_config_t cd = {};
        cd.pin_bit_mask = 1ULL << pins.card_detect;
        cd.mode = GPIO_MODE_INPUT;
        cd.pull_up_en = GPIO_PULLUP_ENABLE;
        cd.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cd) == ESP_OK) {
            const int level = gpio_get_level(static_cast<gpio_num_t>(pins.card_detect));
            const bool present = pins.cd_active_low ? (level == 0) : (level != 0);
            ESP_LOGI(TAG, "card detect gpio=%d level=%d present(hint)=%d (not used for mount)",
                     pins.card_detect, level, present ? 1 : 0);
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = pins.max_freq_khz > 0 ? pins.max_freq_khz : 20000;

    sdmmc_slot_config_t slot = {};
    slot.clk = static_cast<gpio_num_t>(pins.clk);
    slot.cmd = static_cast<gpio_num_t>(pins.cmd);
    slot.d0 = static_cast<gpio_num_t>(pins.d0);
    slot.d1 = pins.d1 >= 0 ? static_cast<gpio_num_t>(pins.d1) : GPIO_NUM_NC;
    slot.d2 = pins.d2 >= 0 ? static_cast<gpio_num_t>(pins.d2) : GPIO_NUM_NC;
    slot.d3 = pins.d3 >= 0 ? static_cast<gpio_num_t>(pins.d3) : GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    /* CD 只作诊断日志：本板 PWRn/CD 极性尚未上机确认，交给 SDMMC 判卡会在
     * CD 接线/极性不符时把整张卡挡在门外（ESP_ERR_NOT_FOUND）。
     * 让驱动直接尝试初始化，最坏情况只是多等一次超时。 */
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = (pins.four_bit && pins.d3 >= 0) ? 4 : 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false; /* 绝不格式化用户卡 */
    mount_cfg.max_files = 4;
    mount_cfg.allocation_unit_size = 16 * 1024;
    mount_cfg.disk_status_check_enable = false;
    mount_cfg.use_one_fat = false;

    sdmmc_card_t *card = nullptr;
    const esp_err_t err =
        esp_vfs_fat_sdmmc_mount(NL_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s (no card?)", esp_err_to_name(err));
        set_card_power(pins, false);
        return err;
    }
    s_sd.mounted = true;
    s_sd.scan_tried = false;
    s_sd.count = 0;
    s_sd.batch = 0;
    if (card) {
        ESP_LOGI(TAG, "mounted %s name=%s size=%lluMB", NL_SD_MOUNT_POINT, card->cid.name,
                 static_cast<unsigned long long>(card->csd.capacity) *
                     static_cast<unsigned long long>(card->csd.sector_size) / (1024ull * 1024ull));
    }
    return ESP_OK;
}

esp_err_t nodelio_sd_rescan(void)
{
    if (!s_sd.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ensure_paths()) {
        return ESP_ERR_NO_MEM;
    }
    s_sd.count = 0;
    scan_dir(NL_SD_MOUNT_POINT, 0);
    if (s_sd.count > 1) {
        qsort(s_sd.paths, static_cast<size_t>(s_sd.count), NL_SD_PATH_MAX, cmp_path);
    }
    s_sd.scan_tried = true;
    /* 重新扫卡（「刷新」按钮）保留当前批次：拷了新歌不该把用户甩回第 1 批。
     * 批次数量减少时钳到最后一页；新挂的卡在 nodelio_sd_mount() 里已归零。 */
    const int batches = nodelio_sd_batch_count();
    if (batches <= 0) {
        s_sd.batch = 0;
    } else if (s_sd.batch >= batches) {
        s_sd.batch = batches - 1;
    }
    ESP_LOGI(TAG, "scan %s: %d audio file(s)", NL_SD_MOUNT_POINT, s_sd.count);
    return ESP_OK;
}

int nodelio_sd_track_count(void)
{
    return s_sd.scan_tried ? s_sd.count : 0;
}

int nodelio_sd_batch(void)
{
    return s_sd.batch;
}

void nodelio_sd_set_batch(int batch)
{
    s_sd.batch = batch < 0 ? 0 : batch;
}

int nodelio_sd_batch_count(void)
{
    if (s_sd.count <= 0) {
        return 0;
    }
    return (s_sd.count + NL_SD_BATCH_MAX - 1) / NL_SD_BATCH_MAX;
}

void nodelio_sd_next_batch(void)
{
    const int n = nodelio_sd_batch_count();
    if (n <= 1) {
        return;
    }
    s_sd.batch = (s_sd.batch + 1) % n;
}

int nodelio_sd_fill_envelope(nl_content_envelope_t *env, int *total_out)
{
    if (total_out) {
        *total_out = s_sd.count;
    }
    if (!env) {
        return 0;
    }
    if (s_sd.count <= 0 || !s_sd.paths) {
        return 0;
    }
    const int batches = nodelio_sd_batch_count();
    if (batches > 0 && s_sd.batch >= batches) {
        s_sd.batch = 0;
    }
    const int start = s_sd.batch * NL_SD_BATCH_MAX;
    int n = 0;
    for (int i = start; i < s_sd.count && n < NL_SD_BATCH_MAX; i++, n++) {
        nl_env_track_t *tr = &env->tracks[n];
        snprintf(tr->id, sizeof(tr->id), "sd%d", i);
        path_to_title(s_sd.paths[i], tr->title, sizeof(tr->title));
        snprintf(tr->artist, sizeof(tr->artist), "内存卡");
        nodelio_sd_make_url(s_sd.paths[i], tr->playback_url, sizeof(tr->playback_url));
        tr->has_play_action = true;
    }
    env->track_n = n;
    return n;
}

bool nodelio_sd_resolve_url(const char *url, char *path, size_t path_len)
{
    if (!url || !path || path_len == 0) {
        return false;
    }
    const size_t prefix = sizeof(NL_SD_URL_PREFIX) - 1;
    if (strncmp(url, NL_SD_URL_PREFIX, prefix) != 0) {
        return false;
    }
    /* sd:///sdcard/x → /sdcard/x：前缀后是绝对路径（可能多一个 /）。 */
    const char *rest = url + prefix;
    while (*rest == '/') {
        rest++;
    }
    snprintf(path, path_len, "/%s", rest);
    return true;
}

void nodelio_sd_make_url(const char *path, char *url, size_t url_len)
{
    if (!url || url_len == 0) {
        return;
    }
    url[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    snprintf(url, url_len, "%s%s", NL_SD_URL_PREFIX, path);
}

#endif /* CONFIG_NODELIO_SD_MUSIC */
