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
extern "C" esp_err_t nodelio_sd_rescan_photos(void) { return ESP_ERR_INVALID_STATE; }
extern "C" int nodelio_sd_photo_count(void) { return 0; }
extern "C" int nodelio_sd_photo_batch(void) { return 0; }
extern "C" void nodelio_sd_photo_set_batch(int batch) { (void)batch; }
extern "C" int nodelio_sd_photo_batch_count(void) { return 0; }
extern "C" void nodelio_sd_photo_next_batch(void) {}
extern "C" int nodelio_sd_fill_photo_envelope(nl_content_envelope_t *env, int *total_out)
{
    (void)env;
    if (total_out) {
        *total_out = 0;
    }
    return 0;
}
extern "C" bool nodelio_sd_photo_path(int index, char *path, size_t path_len)
{
    (void)index;
    (void)path;
    (void)path_len;
    return false;
}

#else /* CONFIG_NODELIO_SD_MUSIC */

#include <dirent.h>
#include <driver/gpio.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_rom_sys.h>
#include <sdmmc_cmd.h>
#include <soc/soc_caps.h>

#if CONFIG_IDF_TARGET_ESP32P4
#include <soc/hp_sys_clkrst_struct.h>
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include <esp_ldo_regulator.h>
#endif
#endif

#define TAG "nl_sd"

/*
 * 对齐：
 *   - host_sdcard_with_hosted（Hosted Slot1 → host init/deinit noop）
 *   - IDF P4 SDMMC 文档 + IDFGH-18612：Slot0 固定脚的 SD_VREF 必须由
 *     片内 LDO VO4 供电；须先 esp_ldo_acquire_channel(4, 3300mV)，
 *     再拨 PWRn。勿用 sd_pwr_ctrl_new_on_chip_ldo 默认可调通道（易留 0mV，
 *     日志常见 send_op_cond TIMEOUT）。
 * 本板卡 VDD 走 AO3401(PWRn)，仅做 3.3V 非 UHS。
 */
#if defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) && \
    (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
#define NL_SD_HOSTED_OWNS_HOST 1
#else
#define NL_SD_HOSTED_OWNS_HOST 0
#endif

#if CONFIG_IDF_TARGET_ESP32P4 && SOC_SDMMC_IO_POWER_EXTERNAL
#define NL_SD_USE_VO4_LDO 1
#define NL_SD_VO4_CHAN 4
#define NL_SD_VO4_MV 3300
#else
#define NL_SD_USE_VO4_LDO 0
#endif

#if NL_SD_HOSTED_OWNS_HOST
static esp_err_t nl_sd_host_init_noop(void)
{
    return ESP_OK;
}
static esp_err_t nl_sd_host_deinit_noop(void)
{
    return ESP_OK;
}
static esp_err_t nl_sd_host_deinit_p_noop(int slot)
{
    (void)slot;
    return ESP_OK;
}
#endif

static sdmmc_delay_phase_t s_target_delay_phase = SDMMC_DELAY_PHASE_0;
static bool s_phase_configured = false;
static bool s_slot_registered = false;
static bool s_spi_bus_inited = false;

static void nl_sd_latch_p4_ls_phase(sdmmc_delay_phase_t phase)
{
#if CONFIG_IDF_TARGET_ESP32P4
    uint32_t sel = 0;
    switch (phase) {
    case SDMMC_DELAY_PHASE_1:
        sel = 1;
        break;
    case SDMMC_DELAY_PHASE_2:
        sel = 2;
        break;
    case SDMMC_DELAY_PHASE_3:
        sel = 3;
        break;
    default:
        sel = 0;
        break;
    }
    /* IDF v6.1 sdmmc_ll_set_din_delay_phase 漏打 reg_sdio_ls_clk_edge_cfg_update 脉冲，
     * 导致相位写入后分频器未锁存。此处显式打脉冲使采样沿生效。 */
    HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_sam_clk_edge_sel = sel;
    HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_cfg_update = 1;
    HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_cfg_update = 0;
#else
    (void)phase;
#endif
}

static esp_err_t nl_sd_do_transaction(int slot, sdmmc_command_t *cmdinfo)
{
    if (!s_phase_configured) {
        /* 先把 probing 400kHz 标记为 READY 并保持时钟常开，避免 IDF v6.1 sd_trans_sdmmc.c
         * 在每条命令（尤其 CMD55 与 ACMD51 之间）反复关停重设分频器。 */
        (void)sdmmc_host_set_card_clk(slot, SDMMC_FREQ_PROBING);
        (void)sdmmc_host_set_cclk_always_on(slot, true);
        const esp_err_t d_err = sdmmc_host_set_input_delay(slot, s_target_delay_phase);
        nl_sd_latch_p4_ls_phase(s_target_delay_phase);
        ESP_LOGI(TAG, "Configured slot=%d probing clk + input delay phase=%d (%s)", slot,
                 s_target_delay_phase, esp_err_to_name(d_err));
        s_phase_configured = true;
    } else {
        nl_sd_latch_p4_ls_phase(s_target_delay_phase);
    }
    const esp_err_t ret = sdmmc_host_do_transaction(slot, cmdinfo);
    if (cmdinfo) {
        const esp_err_t e = ret != ESP_OK ? ret : cmdinfo->error;
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "CMD%d arg=0x%08lx data=%s len=%d -> %s (0x%x) rsp0=0x%08lx",
                     (int)cmdinfo->opcode, (unsigned long)cmdinfo->arg,
                     cmdinfo->data ? "yes" : "no", (int)cmdinfo->datalen, esp_err_to_name(e), e,
                     (unsigned long)cmdinfo->response[0]);
        } else if (cmdinfo->opcode == 51 /* ACMD51 SEND_SCR */) {
            ESP_LOGI(TAG, "ACMD51 SEND_SCR ok, rsp0=0x%08lx", (unsigned long)cmdinfo->response[0]);
        }
    }
    return ret;
}

/** 挂载点：与官方 SDMMC 例程一致。 */
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
    bool photo_scan_tried;
    int count;
    int batch;
    int photo_count;
    int photo_batch;
    char (*paths)[NL_SD_PATH_MAX];
    char (*photos)[NL_SD_PATH_MAX];
    sdmmc_card_t *card;
#if NL_SD_USE_VO4_LDO
    esp_ldo_channel_handle_t vo4;
#endif
};

sd_state_t s_sd = {};

bool lower_ext(const char *name, char *ext, size_t ext_len)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1] || !ext || ext_len == 0) {
        return false;
    }
    size_t n = strlen(dot + 1);
    if (n >= ext_len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = dot[1 + i];
        ext[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    ext[n] = '\0';
    return true;
}

bool ext_supported(const char *name)
{
    char ext[8];
    if (!lower_ext(name, ext, sizeof(ext))) {
        return false;
    }
    /* 只列解码链路真能放的容器：mp3 / m4a / wav。
     * 曲库列表一旦列出就必须能播，宁可少列也不要「点了没声」。
     * flac/aac/ogg 目前没有可用的分帧解析器，暂不列出。 */
    return strcmp(ext, "mp3") == 0 || strcmp(ext, "m4a") == 0 || strcmp(ext, "wav") == 0;
}

bool photo_ext_supported(const char *name)
{
    char ext[8];
    if (!lower_ext(name, ext, sizeof(ext))) {
        return false;
    }
    return strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0;
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

bool ensure_table(char (**table)[NL_SD_PATH_MAX])
{
    if (*table) {
        return true;
    }
    *table = static_cast<char (*)[NL_SD_PATH_MAX]>(
        heap_caps_calloc(NL_SD_MAX_FILES, NL_SD_PATH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!*table) {
        *table = static_cast<char (*)[NL_SD_PATH_MAX]>(calloc(NL_SD_MAX_FILES, NL_SD_PATH_MAX));
    }
    return *table != nullptr;
}

bool ensure_paths(void) { return ensure_table(&s_sd.paths); }
bool ensure_photos(void) { return ensure_table(&s_sd.photos); }

void scan_dir(const char *dir, int depth, bool photos)
{
    int *count = photos ? &s_sd.photo_count : &s_sd.count;
    char (*table)[NL_SD_PATH_MAX] = photos ? s_sd.photos : s_sd.paths;
    if (depth > NL_SD_SCAN_DEPTH || !table || *count >= NL_SD_MAX_FILES) {
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent = nullptr;
    while ((ent = readdir(d)) != nullptr && *count < NL_SD_MAX_FILES) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char full[NL_SD_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n <= 0 || n >= static_cast<int>(sizeof(full))) {
            continue;
        }
        if (ent->d_type == DT_DIR) {
            scan_dir(full, depth + 1, photos);
            continue;
        }
        if (photos ? !photo_ext_supported(ent->d_name) : !ext_supported(ent->d_name)) {
            continue;
        }
        snprintf(table[*count], NL_SD_PATH_MAX, "%s", full);
        (*count)++;
    }
    closedir(d);
}

/** PWRn：active_low 时低电平供电（AO3401 高侧开关）；未挂载断电省电。 */
void set_card_power(const SdCardPins &pins, bool on)
{
    if (pins.power_en < 0) {
        return;
    }
    static bool s_pwr_cfg = false;
    if (!s_pwr_cfg) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << pins.power_en;
        cfg.mode = GPIO_MODE_OUTPUT;
        cfg.pull_up_en = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cfg) != ESP_OK) {
            return;
        }
        s_pwr_cfg = true;
    }
    const bool level = pins.power_active_low ? !on : on;
    gpio_set_level(static_cast<gpio_num_t>(pins.power_en), level ? 1 : 0);
}

/**
 * 彻底冷复位 TF 卡（消除 51kΩ 常电上拉经引脚 ESD 二极管对卡 VDD 10uF 电容的倒灌假断电）：
 *   1. 释放已有 SDMMC Slot0 / SPI2 总线占用
 *   2. 关断 Q1(AO3401, PWRn=1)，同时将 CLK/CMD/D0..D3 全部驱低 100ms 泄放 C52(10uF) 至 0V
 *   3. 恢复 CMD/D0..D2 上拉输入、D3(CS) 拉高、CLK 拉低
 *   4. 导通 Q1(PWRn=0) 并等待 80ms 让 VDD 干净爬升至 3.3V 完成 POR
 */
void nl_sd_true_power_cycle(const SdCardPins &pins)
{
    if (s_slot_registered) {
        (void)sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
        s_slot_registered = false;
    }
    if (s_spi_bus_inited) {
        (void)spi_bus_free(SPI2_HOST);
        s_spi_bus_inited = false;
    }

    const int all_pins[] = {pins.clk, pins.cmd, pins.d0, pins.d1, pins.d2, pins.d3};
    set_card_power(pins, false);
    for (int p : all_pins) {
        if (p >= 0) {
            const gpio_num_t g = static_cast<gpio_num_t>(p);
            gpio_reset_pin(g);
            gpio_pullup_dis(g);
            gpio_pulldown_en(g);
            gpio_set_direction(g, GPIO_MODE_OUTPUT);
            gpio_set_level(g, 0);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (pins.clk >= 0) {
        const gpio_num_t g_clk = static_cast<gpio_num_t>(pins.clk);
        gpio_pulldown_dis(g_clk);
        gpio_set_direction(g_clk, GPIO_MODE_OUTPUT);
        gpio_set_level(g_clk, 0);
    }
    const int pullup_inputs[] = {pins.cmd, pins.d0, pins.d1, pins.d2};
    for (int p : pullup_inputs) {
        if (p >= 0) {
            const gpio_num_t g = static_cast<gpio_num_t>(p);
            gpio_pulldown_dis(g);
            gpio_pullup_en(g);
            gpio_set_direction(g, GPIO_MODE_INPUT);
        }
    }
    if (pins.d3 >= 0) {
        const gpio_num_t g_cs = static_cast<gpio_num_t>(pins.d3);
        gpio_pulldown_dis(g_cs);
        gpio_pullup_en(g_cs);
        gpio_set_direction(g_cs, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(g_cs, 1);
    }

    set_card_power(pins, true);
    vTaskDelay(pdMS_TO_TICKS(80));
}

esp_err_t nl_sd_try_mount_sdspi(const SdCardPins &pins,
                                const esp_vfs_fat_sdmmc_mount_config_t &mount_cfg,
                                sdmmc_card_t **out_card)
{
    if (pins.d3 < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    static const int kSpiFreqsKhz[] = {SDMMC_FREQ_DEFAULT, 10000, 4000};
    esp_err_t err = ESP_FAIL;

    for (size_t i = 0; i < sizeof(kSpiFreqsKhz) / sizeof(kSpiFreqsKhz[0]); ++i) {
        nl_sd_true_power_cycle(pins);

        spi_bus_config_t bus_cfg = {};
        bus_cfg.mosi_io_num = pins.cmd;
        bus_cfg.miso_io_num = pins.d0;
        bus_cfg.sclk_io_num = pins.clk;
        bus_cfg.quadwp_io_num = -1;
        bus_cfg.quadhd_io_num = -1;
        bus_cfg.max_transfer_sz = 4096;

        err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SDSPI spi_bus_initialize failed: %s (0x%x)", esp_err_to_name(err), err);
            return err;
        }
        s_spi_bus_inited = true;

        /* spi_bus_initialize 会重置引脚上下拉，重新开启内部上拉与板载 51kΩ 并联 */
        const int pull_pins[] = {pins.cmd, pins.d0, pins.d1, pins.d2, pins.d3};
        for (int p : pull_pins) {
            if (p >= 0) {
                gpio_pullup_en(static_cast<gpio_num_t>(p));
                gpio_pulldown_dis(static_cast<gpio_num_t>(p));
            }
        }

        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        host.max_freq_khz = kSpiFreqsKhz[i];
        /* 51kΩ 弱上拉下忽略 SPI 数据阶段 CRC16 误报，保留命令阶段 CRC7 */
        host.flags |= SDMMC_HOST_FLAG_SPI_IGNORE_DATA_CRC;

        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_cfg.host_id = SPI2_HOST;
        slot_cfg.gpio_cs = static_cast<gpio_num_t>(pins.d3);
        slot_cfg.gpio_cd = SDSPI_SLOT_NO_CD;
        slot_cfg.gpio_wp = SDSPI_SLOT_NO_WP;

        ESP_LOGI(TAG, "SDSPI mount try %u/%u: host=SPI2 freq=%dkHz CS=%d MOSI=%d MISO=%d CLK=%d",
                 static_cast<unsigned>(i + 1),
                 static_cast<unsigned>(sizeof(kSpiFreqsKhz) / sizeof(kSpiFreqsKhz[0])),
                 host.max_freq_khz, pins.d3, pins.cmd, pins.d0, pins.clk);

        err = esp_vfs_fat_sdspi_mount(NL_SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, out_card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SDSPI mounted OK at %dkHz", host.max_freq_khz);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SDSPI try %u (%dkHz) failed: %s (0x%x)", static_cast<unsigned>(i + 1),
                 host.max_freq_khz, esp_err_to_name(err), err);
        (void)spi_bus_free(SPI2_HOST);
        s_spi_bus_inited = false;
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "SDSPI card init OK but FAT mount failed");
            return err;
        }
    }
    return err;
}

esp_err_t nl_sd_try_mount_sdmmc(const SdCardPins &pins,
                                const esp_vfs_fat_sdmmc_mount_config_t &mount_cfg,
                                sdmmc_card_t **out_card)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = pins.max_freq_khz > 0 ? pins.max_freq_khz : SDMMC_FREQ_DEFAULT;
#if NL_SD_HOSTED_OWNS_HOST
    host.init = &nl_sd_host_init_noop;
    host.deinit = &nl_sd_host_deinit_noop;
    host.deinit_p = &nl_sd_host_deinit_p_noop;
    host.flags &= ~SDMMC_HOST_FLAG_DEINIT_ARG;
#endif
#ifdef CONFIG_IDF_TARGET_ESP32P4
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
#endif
    host.flags &= ~SDMMC_HOST_FLAG_8BIT;
    host.flags &= ~SDMMC_HOST_FLAG_DDR;
    host.flags &= ~SDMMC_HOST_FLAG_4BIT;
    host.flags |= SDMMC_HOST_FLAG_1BIT;
    host.do_transaction = &nl_sd_do_transaction;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = static_cast<gpio_num_t>(pins.clk);
    slot.cmd = static_cast<gpio_num_t>(pins.cmd);
    slot.d0 = static_cast<gpio_num_t>(pins.d0);
    slot.d1 = pins.d1 >= 0 ? static_cast<gpio_num_t>(pins.d1) : GPIO_NUM_NC;
    slot.d2 = pins.d2 >= 0 ? static_cast<gpio_num_t>(pins.d2) : GPIO_NUM_NC;
    slot.d3 = pins.d3 >= 0 ? static_cast<gpio_num_t>(pins.d3) : GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    static const sdmmc_delay_phase_t kPhases[] = {SDMMC_DELAY_PHASE_0, SDMMC_DELAY_PHASE_1,
                                                  SDMMC_DELAY_PHASE_2, SDMMC_DELAY_PHASE_3};
    esp_err_t err = ESP_FAIL;
    for (int pi = 0; pi < 4; ++pi) {
        nl_sd_true_power_cycle(pins);
        s_target_delay_phase = kPhases[pi];
        s_phase_configured = false;
        ESP_LOGI(TAG, "SDMMC mount try %d/4: slot=%d width=1 freq=%dkHz sample-phase=%d", pi + 1,
                 host.slot, host.max_freq_khz, pi);
        err = esp_vfs_fat_sdmmc_mount(NL_SD_MOUNT_POINT, &host, &slot, &mount_cfg, out_card);
        s_slot_registered = true;
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SDMMC card ready with sample-phase=%d", pi);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SDMMC try %d/4 (phase %d) failed: %s (0x%x)", pi + 1, pi,
                 esp_err_to_name(err), err);
        (void)sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
        s_slot_registered = false;
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "SDMMC card init OK but FAT mount failed");
            break;
        }
    }
    return err;
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

    ESP_LOGI(TAG, "Initializing SD card: CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d PWR=%d CD=%d",
             pins.clk, pins.cmd, pins.d0, pins.d1, pins.d2, pins.d3, pins.power_en, pins.card_detect);

#if NL_SD_USE_VO4_LDO
    /* 先开 VO4@3.3V 给 SD_VREF / GPIO39..44 IO 域供电；再操作引脚与 PWRn（IDFGH-18612） */
    if (!s_sd.vo4) {
        esp_ldo_channel_config_t vo4_cfg = {
            .chan_id = NL_SD_VO4_CHAN,
            .voltage_mv = NL_SD_VO4_MV,
        };
        const esp_err_t ldo_err = esp_ldo_acquire_channel(&vo4_cfg, &s_sd.vo4);
        if (ldo_err != ESP_OK) {
            ESP_LOGE(TAG, "LDO VO4 @%dmV failed: %s", NL_SD_VO4_MV, esp_err_to_name(ldo_err));
            return ldo_err;
        }
        ESP_LOGI(TAG, "SD_VREF = LDO VO4 @ %dmV", NL_SD_VO4_MV);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif

    if (pins.card_detect >= 0) {
        gpio_config_t cd = {};
        cd.pin_bit_mask = 1ULL << pins.card_detect;
        cd.mode = GPIO_MODE_INPUT;
        cd.pull_up_en = GPIO_PULLUP_ENABLE;
        cd.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&cd) == ESP_OK) {
            const int level = gpio_get_level(static_cast<gpio_num_t>(pins.card_detect));
            const bool present = pins.cd_active_low ? (level == 0) : (level != 0);
            ESP_LOGI(TAG, "CD gpio=%d level=%d present(hint)=%d", pins.card_detect, level,
                     present ? 1 : 0);
        }
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    /* P4 + ESP-Hosted(Slot1 40MHz SDIO) 下，SDMMC0 与 Slot1 共用唯一 SDMMC 控制器和
     * HP_SYS_CLKRST.peri_clk_ctrl02 第一级分频器，双槽并发会互改分频并卡 ACMD51(SCR)。
     * 优先走独立外设 SPI2_HOST (SDSPI)，完全隔离 Wi-Fi SDIO；若不支持再回退 SDMMC0。 */
    esp_err_t err = nl_sd_try_mount_sdspi(pins, mount_cfg, &card);
    if (err != ESP_OK && err != ESP_FAIL) {
        ESP_LOGW(TAG, "SDSPI mount did not succeed (%s), falling back to SDMMC Slot0",
                 esp_err_to_name(err));
        err = nl_sd_try_mount_sdmmc(pins, mount_cfg, &card);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(err));
        return err;
    }

    s_sd.card = card;
    s_sd.mounted = true;
    s_sd.scan_tried = false;
    s_sd.photo_scan_tried = false;
    s_sd.count = 0;
    s_sd.batch = 0;
    s_sd.photo_count = 0;
    s_sd.photo_batch = 0;
    ESP_LOGI(TAG, "Filesystem mounted");
    if (card) {
        sdmmc_card_print_info(stdout, card);
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
    scan_dir(NL_SD_MOUNT_POINT, 0, false);
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

esp_err_t nodelio_sd_rescan_photos(void)
{
    if (!s_sd.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ensure_photos()) {
        return ESP_ERR_NO_MEM;
    }
    s_sd.photo_count = 0;
    scan_dir(NL_SD_MOUNT_POINT, 0, true);
    if (s_sd.photo_count > 1) {
        qsort(s_sd.photos, static_cast<size_t>(s_sd.photo_count), NL_SD_PATH_MAX, cmp_path);
    }
    s_sd.photo_scan_tried = true;
    const int batches = nodelio_sd_photo_batch_count();
    if (batches <= 0) {
        s_sd.photo_batch = 0;
    } else if (s_sd.photo_batch >= batches) {
        s_sd.photo_batch = batches - 1;
    }
    ESP_LOGI(TAG, "scan photos %s: %d jpeg(s)", NL_SD_MOUNT_POINT, s_sd.photo_count);
    return ESP_OK;
}

int nodelio_sd_photo_count(void)
{
    return s_sd.photo_scan_tried ? s_sd.photo_count : 0;
}

int nodelio_sd_photo_batch(void)
{
    return s_sd.photo_batch;
}

void nodelio_sd_photo_set_batch(int batch)
{
    s_sd.photo_batch = batch < 0 ? 0 : batch;
}

int nodelio_sd_photo_batch_count(void)
{
    if (s_sd.photo_count <= 0) {
        return 0;
    }
    return (s_sd.photo_count + NL_SD_BATCH_MAX - 1) / NL_SD_BATCH_MAX;
}

void nodelio_sd_photo_next_batch(void)
{
    const int n = nodelio_sd_photo_batch_count();
    if (n <= 1) {
        return;
    }
    s_sd.photo_batch = (s_sd.photo_batch + 1) % n;
}

int nodelio_sd_fill_photo_envelope(nl_content_envelope_t *env, int *total_out)
{
    if (total_out) {
        *total_out = s_sd.photo_count;
    }
    if (!env) {
        return 0;
    }
    if (s_sd.photo_count <= 0 || !s_sd.photos) {
        return 0;
    }
    const int batches = nodelio_sd_photo_batch_count();
    if (batches > 0 && s_sd.photo_batch >= batches) {
        s_sd.photo_batch = 0;
    }
    const int start = s_sd.photo_batch * NL_SD_BATCH_MAX;
    int n = 0;
    for (int i = start; i < s_sd.photo_count && n < NL_SD_BATCH_MAX; i++, n++) {
        nl_env_track_t *tr = &env->tracks[n];
        snprintf(tr->id, sizeof(tr->id), "%d", i);
        path_to_title(s_sd.photos[i], tr->title, sizeof(tr->title));
        snprintf(tr->artist, sizeof(tr->artist), "内存卡相册");
        tr->has_play_action = false;
        tr->invoke_n = 1;
        snprintf(tr->invokes[0].id, sizeof(tr->invokes[0].id), "open");
        snprintf(tr->invokes[0].title, sizeof(tr->invokes[0].title), "查看");
        snprintf(tr->invokes[0].app_id, sizeof(tr->invokes[0].app_id), "photo.sd");
        snprintf(tr->invokes[0].params_json, sizeof(tr->invokes[0].params_json),
                 "{\"index\":%d}", i);
    }
    env->track_n = n;
    return n;
}

bool nodelio_sd_photo_path(int index, char *path, size_t path_len)
{
    if (!path || path_len == 0 || index < 0 || index >= s_sd.photo_count || !s_sd.photos) {
        return false;
    }
    snprintf(path, path_len, "%s", s_sd.photos[index]);
    return true;
}

#endif /* CONFIG_NODELIO_SD_MUSIC */
