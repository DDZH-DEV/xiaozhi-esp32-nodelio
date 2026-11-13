#include "overlay_priv.h"
#include "panel_jobs.h"
#include "ui_widgets.h"
#include "ui_work_stats.h"

#include "application.h"
#include "board_info.h"
#include "device_caps.h"
#include "layout_cache.h"
#include "media_player.h"
#include "nl_url_util.h"
#include "nodelio_http.h"
#include "content_envelope.h"
#include "panel_ui.h"
#include "sd_library.h"
#include "sd_photo_ui.h"
#include "skill_result_ui.h"
#include "skill_runtime.h"
#include "wifi_provision.h"

#include <cJSON.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

LV_FONT_DECLARE(font_material_symbols_30_4);

#define TAG "nl_apps"
#define APP_MAX 18 /* 16 cloud entries plus built-in daily and, on SD boards, files. */
/* 8 个样例应用带 inputSchema 时整包 >7KB；旧 4KB 截断后 cJSON 失败 →「暂无已安装」。 */
#define APP_LIST_BUF 24576

struct nl_app_item {
    char id[72];
    char name[40];
    char desc[72];
    bool enabled;
    bool outbound_bridge; /* invoke.transport=outbound_bridge */
};

struct nl_app_snap {
    nl_app_item items[APP_MAX];
    int n;
    char err[48];
};

static lv_obj_t *s_page;
static lv_obj_t *s_list_pane; /* 宽屏可收成左侧轨，给技能卡让位 */
static lv_obj_t *s_list;
static lv_obj_t *s_hint;
EXT_RAM_BSS_ATTR static nl_app_item s_items[APP_MAX];

static void with_builtin_daily(nl_app_item* items, int& n) {
    int keep = 0;
    for (int i = 0; i < n; ++i) {
        const char* bare = strstr(items[i].id, "::");
        bare = bare ? bare + 2 : items[i].id;
        if (strcmp(bare, "builtin.daily") == 0 || strcmp(bare, "builtin.files") == 0 ||
            strcmp(bare, "weather.today") == 0 || strcmp(bare, "air.now") == 0 ||
            strcmp(bare, "quote.daily") == 0)
            continue;
        items[keep++] = items[i];
    }
    const bool files = nodelio_sd_available();
    const int extra = files ? 2 : 1;
    if (keep > APP_MAX - extra) {
        keep = APP_MAX - extra;
    }
    memmove(items + extra, items, sizeof(*items) * (size_t)keep);
    items[0] = {};
    snprintf(items[0].id, sizeof(items[0].id), "builtin.daily");
    snprintf(items[0].name, sizeof(items[0].name), "日历与天气");
    snprintf(items[0].desc, sizeof(items[0].desc), "月历 · 天气 · 空气");
    items[0].enabled = true;
    if (files) {
        items[1] = {};
        snprintf(items[1].id, sizeof(items[1].id), "builtin.files");
        snprintf(items[1].name, sizeof(items[1].name), "文件");
        snprintf(items[1].desc, sizeof(items[1].desc), "内存卡歌曲与照片");
        items[1].enabled = true;
    }
    n = keep + extra;
}

/** 进度卡内部动作：审批按钮与派活投递。不进应用列表 / 语音打开。 */
static bool skill_is_button_only(const char* id) { return nl_workbench_helper(id); }
static int s_item_n;
static bool s_loading;
static bool s_daily_loading;
static bool s_have_list;
static bool s_split;
/** 列表视觉是否左轨；关闭动画拉宽期间保持 true，避免 ROW_WRAP 闪成一行多卡。 */
static bool s_split_vis;
static int s_focus_idx = -1;

static constexpr int kAppsPagePadH = 16;
static constexpr int kAppsAnimMs = 280;

static bool apps_wide(void)
{
    return nodelio_display_profile()->nav_width != 0;
}

/** 仅 sila-86（480 方屏）；508/P4 不走此分支。 */
static bool apps_sila86(void)
{
    const auto *b = nodelio_board_get();
    return b && b->info.board_id && strcmp(b->info.board_id, "sila_86") == 0;
}

static int apps_body_w(void)
{
    const auto *p = nodelio_display_profile();
    return (int)p->width - (int)p->nav_width;
}

static int apps_pane_target_w(bool split)
{
    const int full = apps_body_w() - 2 * kAppsPagePadH;
    if (apps_wide() && split) {
        /* P4 左轨固定紧凑宽：只够色 Pod + 两行字，右侧技能窗吃满余量。 */
        int rail = 220;
        if (rail > full) {
            rail = full;
        }
        if (rail < 168) {
            rail = full > 168 ? 168 : full;
        }
        return rail;
    }
    return full > 120 ? full : 120;
}

static void apps_apply_pane(int w, bool split)
{
    if (!s_list_pane) {
        return;
    }
    /* 宽度不用 flex_grow，否则会被撑回整页；高度必须撑满，否则列表只剩一张半就被裁掉。 */
    lv_obj_set_height(s_list_pane, lv_pct(100));
    if (apps_wide() && split) {
        lv_obj_set_flex_grow(s_list_pane, 0);
        lv_obj_set_width(s_list_pane, w > 80 ? w : 80);
        return;
    }
    lv_obj_set_flex_grow(s_list_pane, 1);
    lv_obj_set_width(s_list_pane, lv_pct(100));
}

static void apps_exec_pane_w(void *obj, int32_t v)
{
    lv_obj_t *pane = static_cast<lv_obj_t *>(obj);
    lv_obj_set_flex_grow(pane, 0);
    lv_obj_set_height(pane, lv_pct(100));
    lv_obj_set_width(pane, v);
}

static void apps_set_split(bool on, bool animate);
static void render_list(void);

static void copy_field(char *dst, size_t n, const cJSON *obj, const char *key)
{
    dst[0] = '\0';
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring && n) {
        snprintf(dst, n, "%s", v->valuestring);
    }
}

/** 应用列表标题与服务端 skills[].name 一致，不做截断改写。 */
static void copy_app_label(char *dst, size_t n, const cJSON *obj)
{
    copy_field(dst, n, obj, "name");
}

static void set_hint(const char *text)
{
    if (!s_hint) {
        return;
    }
    /* 已有应用时不占一行；仅空列表显示加载/失败/暂无 */
    if (s_item_n > 0 || text == nullptr || text[0] == '\0') {
        lv_label_set_text(s_hint, "");
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_hint, text);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

static bool apps_list_eq(const nl_app_item *items, int n)
{
    if (n != s_item_n) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (items[i].enabled != s_items[i].enabled ||
            items[i].outbound_bridge != s_items[i].outbound_bridge) {
            return false;
        }
        if (strcmp(items[i].id, s_items[i].id) != 0 || strcmp(items[i].name, s_items[i].name) != 0 ||
            strcmp(items[i].desc, s_items[i].desc) != 0) {
            return false;
        }
    }
    return true;
}

static void apps_split_failed(void)
{
    const auto epoch = nodelio_panel_job_result_epoch();
    Application::GetInstance().Schedule([epoch]() {
        DisplayLockGuard lock(nl_ov_disp());
        if (lock && nodelio_panel_skill_view_current(epoch))
            nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
    });
}

static void open_app_at(int idx, bool reset_daily = true, bool force_weather_refresh = false) {
    if (idx < 0 || idx >= s_item_n) {
        return;
    }
    if (!s_items[idx].enabled) {
        nl_ov_toast("本机暂不支持");
        return;
    }
    /* 派活无独立入口；误点则打开进度 */
    if (strstr(s_items[idx].id, "dsh.task") != nullptr) {
        nl_ov_apps_invoke_dsh_status(nullptr, nullptr);
        return;
    }
    s_focus_idx = idx;
    if (strcmp(s_items[idx].id, "builtin.files") == 0) {
        /* 已打开其它应用时只更新选中；没确认读到卡之前不收列表。 */
        if (apps_wide() && s_split) {
            render_list();
        }
        nodelio_skill_request_sd_library(false);
        return;
    }
    /* 播歌中再点曲库：恢复列表，勿重新 invoke（会换一批并误切歌） */
    if (nodelio_skill_id_is_music(s_items[idx].id) &&
        (nodelio_media_is_playing() || nodelio_media_is_paused()) &&
        nodelio_skill_try_reopen_music_list()) {
        if (apps_wide() && s_split) {
            render_list();
        }
        return;
    }
    const bool daily = strcmp(s_items[idx].id, "builtin.daily") == 0;
    if (daily && s_daily_loading) {
        nl_ov_toast("天气正在更新，请稍候");
        return;
    }
    const uint32_t view_epoch = nodelio_panel_begin_skill_view();
    const uint32_t daily_generation = daily ? nodelio_skill_daily_open(reset_daily) : 0;
    if (daily && !daily_generation)
        return;
    /* 宽屏先不收列表。结果卡真正出现时再折叠；打不开则保持整列。 */
    if (apps_wide() && s_split) {
        render_list();
    }
    if (daily && !nodelio_wifi_is_connected()) {
        nl_ov_toast("当前离线，日历可用；联网后刷新天气");
        return;
    }
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        nl_ov_toast("内存不足");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    if (!nl_ov_cfg_http_ready(cfgp, true)) {
        free(cfgp);
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    struct invoke_arg {
        nodelio_nvs_config_t *cfg;
        char id[72];
        uint32_t daily_generation;
        bool force_weather_refresh;
    };
    auto *arg = static_cast<invoke_arg *>(malloc(sizeof(invoke_arg)));
    if (!arg) {
        free(cfgp);
        nl_ov_toast("内存不足");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
        return;
    }
    arg->cfg = cfgp;
    arg->daily_generation = daily_generation;
    arg->force_weather_refresh = daily && force_weather_refresh;
    snprintf(arg->id, sizeof(arg->id), "%s", s_items[idx].id);
    nl_ov_toast(daily && force_weather_refresh ? "正在刷新天气…" : "正在打开…");
    if (daily)
        s_daily_loading = true;
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive,
            [](void* p) {
                auto *a = static_cast<invoke_arg *>(p);
                const bool music_app = nodelio_skill_id_is_music(a->id);
                if (music_app) {
                    Application::GetInstance().Schedule([]() {
                        auto st = Application::GetInstance().GetDeviceState();
                        if (st == kDeviceStateListening || st == kDeviceStateSpeaking ||
                            st == kDeviceStateConnecting) {
                            Application::GetInstance().EndVoiceSessionAfterLocalSkill();
                        }
                    });
                } else if (!a->daily_generation && nodelio_skill_id_preempts_media(a->id) &&
                           nodelio_media_preempt_for_heavy_app()) {
                    nodelio_skill_clear_music_stash();
                    nodelio_panel_ui_toast_defer("已停止音乐以打开应用");
                }
                char base[160];
                nl_url_strip_base(a->cfg->server_url, base, sizeof(base));
                char url[400];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills/%s/invoke", base,
                         a->cfg->device_id, a->id);
                const int cap = nodelio_caps_max_content_bytes() + 512;
                char *resp = static_cast<char *>(
                    heap_caps_malloc((size_t)cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!resp) {
                    resp = static_cast<char *>(malloc((size_t)cap));
                }
                bool ok = false;
                if (!resp) {
                    nodelio_panel_ui_toast_defer("内存不足");
                } else {
                    resp[0] = '\0';
                    int status = 0;
                    const char *body =
                        a->force_weather_refresh ? "{\"refresh\":true}" : "{\"keyword\":\"\"}";
                    esp_err_t err = nodelio_http_post_json_ms(
                        url, a->cfg->device_key, body, &status, resp, cap - 1,
                        a->daily_generation ? 16000 : 10000);
                    if (err == ESP_OK && resp[0] &&
                        (status == 200 || status == 201 ||
                         (status >= 400 && strstr(resp, "\"schema_version\"") != nullptr))) {
                        ESP_LOGI(TAG, "deliver id=%s music=%d status=%d", a->id, music_app ? 1 : 0,
                                 status);
                        if (a->daily_generation) {
                            auto* env = static_cast<nl_content_envelope_t*>(
                                heap_caps_calloc(1, sizeof(nl_content_envelope_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                            if (env && nodelio_envelope_parse(resp, env) &&
                                strcmp(env->card_type, "calendar_weather") == 0) {
                                const uint32_t generation = a->daily_generation;
                                const auto cfg = *a->cfg;
                                char spoken[NL_ENV_SPOKEN];
                                spoken[0] = '\0';
                                if (env->spoken[0]) {
                                    snprintf(spoken, sizeof(spoken), "%s", env->spoken);
                                }
                                Application::GetInstance().Schedule([env, generation, cfg]() {
                                    nodelio_skill_daily_update(&cfg, env, generation);
                                });
                                /* 问天气时通道应仍开着（open_app 不再误掐），云端 TTS 继续念；
                                 * 这里把服务端 spoken 再 toast/字幕一次，方便核对 API 结果。 */
                                if (spoken[0]) {
                                    nodelio_panel_ui_announce_spoken(spoken);
                                }
                                ok = true;
                            } else {
                                if (env)
                                    free(env->detail_body);
                                free(env);
                            }
                        } else {
                            char spoken[NL_ENV_SPOKEN];
                            spoken[0] = '\0';
                            nodelio_skill_deliver_json(a->cfg, resp, false, spoken, sizeof(spoken));
                            if (nl_workbench_app(a->id) && spoken[0]) {
                                const char* au = nodelio_skill_last_spoken_audio_url();
                                nodelio_panel_ui_announce_spoken_ex(spoken, au);
                            }
                            ok = true;
                        }
                    } else if (err != ESP_OK || status == 0 || !resp[0]) {
                        nl_ov_toast_invoke_unreachable(a->id);
                    } else {
                        nodelio_panel_ui_toast_defer("打开失败");
                    }
                    free(resp);
                }
                if (!ok && !a->daily_generation) {
                    apps_split_failed();
                } else if (!ok) {
                    nodelio_panel_ui_toast_defer("天气未更新，日历仍可使用");
                }
                if (a->daily_generation) {
                    Application::GetInstance().Schedule([]() { s_daily_loading = false; });
                }
                free(a->cfg);
                free(a);
            },
            arg,
            [](void* p) {
                auto* a = static_cast<invoke_arg*>(p);
                if (a->daily_generation)
                    Application::GetInstance().Schedule([]() { s_daily_loading = false; });
                free(a->cfg);
                free(a);
            },
            view_epoch)) {
        free(arg->cfg);
        free(arg);
        if (daily)
            s_daily_loading = false;
        nl_ov_toast("无法打开");
        if (!daily)
            apps_set_split(nodelio_skill_result_card_visible(), true);
    }
}

void nl_ov_apps_refresh_daily(void) {
    for (int i = 0; i < s_item_n; ++i) {
        if (strcmp(s_items[i].id, "builtin.daily") == 0) {
            open_app_at(i, false, true);
            return;
        }
    }
}

static const char *apps_glyph_for(const char *id)
{
    if (!id || !id[0]) {
        return MATERIAL_SYMBOLS_EXPLORE;
    }
    if (strstr(id, "builtin.daily") != nullptr) {
        return MATERIAL_SYMBOLS_CALENDAR_MONTH;
    }
    if (strstr(id, "builtin.files") != nullptr) {
        return MATERIAL_SYMBOLS_SD_CARD;
    }
    if (strstr(id, "podcast") != nullptr) {
        return MATERIAL_SYMBOLS_HEADPHONES;
    }
    if (strstr(id, "radio") != nullptr) {
        return MATERIAL_SYMBOLS_VOLUME_UP;
    }
    if (nodelio_skill_id_is_music(id)) {
        return MATERIAL_SYMBOLS_MUSIC_NOTE;
    }
    if (strstr(id, "news") != nullptr || strstr(id, "rss") != nullptr) {
        return MATERIAL_SYMBOLS_LINK;
    }
    if (strstr(id, "article") != nullptr) {
        return MATERIAL_SYMBOLS_MAIL;
    }
    if (strstr(id, "support") != nullptr || strstr(id, "inbox") != nullptr ||
        strstr(id, "gochat") != nullptr || strstr(id, "go-chat") != nullptr) {
        return MATERIAL_SYMBOLS_CONTACT_SUPPORT;
    }
    if (strstr(id, "codex") != nullptr) {
        return MATERIAL_SYMBOLS_EDIT_SQUARE; /* 代码工作台 */
    }
    if (strstr(id, "deepseek") != nullptr) {
        return MATERIAL_SYMBOLS_MEMORY; /* 推理 / 算力 */
    }
    if (strstr(id, "workbuddy") != nullptr) {
        return MATERIAL_SYMBOLS_PERSON; /* 协作助手 */
    }
    if (strstr(id, "openai") != nullptr || strstr(id, "gpt") != nullptr) {
        return MATERIAL_SYMBOLS_ROBOT_2; /* 对话模型 */
    }
    if (strstr(id, "chat") != nullptr) {
        return MATERIAL_SYMBOLS_CHAT_BUBBLE;
    }
    return MATERIAL_SYMBOLS_EXPLORE;
}

static uint32_t apps_color_for(const char *id)
{
    if (!id || !id[0]) {
        return 0x3D5A80;
    }
    if (strstr(id, "builtin.daily") != nullptr || strstr(id, "weather") != nullptr) {
        return 0x2F80ED; /* 天空蓝：日历与天气 */
    }
    if (strstr(id, "builtin.files") != nullptr || strstr(id, "sd") != nullptr) {
        return 0xF2994A; /* 暖阳橙：文件存储 */
    }
    if (strstr(id, "podcast") != nullptr) {
        return 0x8E44AD; /* 雅致紫：播客 */
    }
    if (strstr(id, "radio") != nullptr) {
        return 0xFF5722; /* 活力赤橙：网络电台 */
    }
    if (nodelio_skill_id_is_music(id)) {
        return 0xFF2D55; /* 音乐粉红：曲库 */
    }
    if (strstr(id, "news") != nullptr || strstr(id, "rss") != nullptr) {
        return 0x00B4D8; /* 清透松石青：RSS与新闻 */
    }
    if (strstr(id, "article") != nullptr) {
        return 0x5B5EA6; /* 文艺青蓝：每日一篇 */
    }
    if (strstr(id, "support") != nullptr || strstr(id, "inbox") != nullptr ||
        strstr(id, "gochat") != nullptr || strstr(id, "go-chat") != nullptr) {
        return 0x34C759; /* 品牌翡翠绿：智能客服 */
    }
    if (strstr(id, "deepseek") != nullptr) {
        return 0x0284C7; /* 蔚蓝：DeepSeek */
    }
    if (strstr(id, "workbuddy") != nullptr) {
        return 0x7C3AED; /* 极客亮紫：WorkBuddy */
    }
    if (strstr(id, "openai") != nullptr || strstr(id, "gpt") != nullptr) {
        return 0x10A37F; /* OpenAI 标志薄荷绿 */
    }
    if (strstr(id, "codex") != nullptr) {
        return 0x4F46E5; /* 科技深靛蓝：Codex */
    }
    if (strstr(id, "chat") != nullptr) {
        return 0x10B981; /* 消息清新绿：对话 */
    }
    /* 未知应用散列到现代调色板 */
    static const uint32_t k_palette[] = {
        0x2F80ED, 0x34C759, 0xFF5722, 0x8E44AD,
        0x00B4D8, 0xF2994A, 0x4F46E5, 0xFF2D55
    };
    uint32_t h = 5381;
    for (const char *p = id; *p; p++) {
        h = ((h << 5) + h) + (uint8_t)*p;
    }
    return k_palette[h % (sizeof(k_palette) / sizeof(k_palette[0]))];
}

struct app_view_t {
    lv_obj_t *card = nullptr, *pod = nullptr, *icon = nullptr;
    lv_obj_t *text = nullptr, *title = nullptr, *subtitle = nullptr;
    nl_app_item item{};
    int width = 0, height = 0, pod_size = 0, line = 0;
    const lv_font_t* font = nullptr;
    bool focused = false;
    int8_t rail = -1;
};
EXT_RAM_BSS_ATTR static app_view_t s_app_views[APP_MAX];
static bool s_apps_dirty;
static bool s_force_paint;

static void render_list(void)
{
    if (!s_list) {
        return;
    }
    if (s_ready && s_page && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
        s_apps_dirty = true;
        return;
    }
    NlUiWorkScope work("apps.update");
    s_apps_dirty = false;
    const bool rail = s_split_vis && apps_wide();
    const int list_w = rail ? apps_pane_target_w(true) : apps_pane_target_w(false);
    /* 86：5 列图标网格。P4 全宽：6 列居中启动台。P4 分栏：紧凑色 Pod 横条。 */
    const bool wide_grid = apps_wide() && !rail;
    const int line = nl_ov_ui_line_box();
    const lv_font_t* font = nl_ov_ui_font();
    const int font_h = nl_ov_ui_font_line_h();
    /* 0=标准；1/2 同大字库，间距按 ≥1 */
    const bool large = nodelio_text_scale() >= 1;
    const bool sila86 = apps_sila86();
    const int gap = wide_grid ? (large ? 28 : 22) : (large ? 10 : 8);
    int card_w, card_h;
    /* 86 大字：略缩 Pod，把宽度留给两行标题；508 方屏加大色 Pod。
     * 禁止 transform 放大位图图标（会糊）；字库最大 30，居中留白。 */
    const int pod_size =
        rail ? 52 : (wide_grid ? 72 : (sila86 && large ? 52 : (sila86 ? 58 : 84)));
    const int icon_scale = LV_SCALE_NONE;
    if (rail) {
        card_w = list_w;
        /* 仅标题，相对色 Pod 垂直居中 */
        card_h = pod_size + (large ? 16 : 12);
        /* 单列：关闭动画拉宽时若仍是 ROW_WRAP，会短暂排成一行很多个。 */
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(s_list, large ? 10 : 8, 0);
        lv_obj_set_style_pad_column(s_list, 0, 0);
    } else if (wide_grid) {
        /* 按可用宽强制 6 列（勿写死小格宽，否则会排成 8 个一行）。首排靠上，两排行距加大。 */
        constexpr int kWideCols = 6;
        card_w = (list_w - gap * (kWideCols - 1)) / kWideCols;
        if (card_w < pod_size + 8) {
            card_w = pod_size + 8;
        }
        const int icon_title_gap = large ? 12 : 10;
        card_h = 4 + pod_size + icon_title_gap + font_h + 6;
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_top(s_list, 4, 0);
        lv_obj_set_style_pad_row(s_list, large ? 40 : 32, 0);
        lv_obj_set_style_pad_column(s_list, gap, 0);
    } else {
        /* 方屏默认 5 列；86 大字改 4 列，避免窄列把全名拆成怪换行。标题高含行距。 */
        const int cols = (sila86 && large) ? 4 : 5;
        const int title_h = font_h + line; /* 两行：line_h + (line_h+line_space) */
        const int icon_gap = (sila86 && large) ? 6 : 8;
        card_w = (list_w - gap * (cols - 1)) / cols;
        if (card_w < pod_size + 4) {
            card_w = pod_size + 4;
        }
        card_h = 4 + pod_size + icon_gap + title_h + 4;
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        /* 508 行距略收；86 保持原样 */
        const int row_gap = sila86 ? (large ? 14 : 16) : (large ? 8 : 6);
        lv_obj_set_style_pad_row(s_list, row_gap, 0);
        lv_obj_set_style_pad_column(s_list, gap, 0);
    }
    for (int i = 0; i < APP_MAX; ++i) {
        auto& view = s_app_views[i];
        if (i >= s_item_n) {
            if (view.card) {
                lv_obj_delete(view.card);
                view = {};
            }
            continue;
        }
        const auto& item = s_items[i];
        const bool created = !view.card;
        if (created) {
            view.card = lv_obj_create(s_list);
            lv_obj_remove_style_all(view.card);
            lv_obj_set_style_bg_opa(view.card, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(view.card, UI_RADIUS_MD, 0);
            lv_obj_clear_flag(view.card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_ext_click_area(view.card, 8);
            lv_obj_set_style_bg_color(view.card, UI_SURFACE_PRESSED, LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(view.card, LV_OPA_40, LV_STATE_PRESSED);
            lv_obj_set_style_translate_y(view.card, 1, LV_STATE_PRESSED);
            // Slots keep their index when cloud items are reordered. The callback
            // resolves the current item instead of retaining a stale item pointer.
            lv_obj_add_event_cb(
                view.card,
                [](lv_event_t* e) { open_app_at((int)(intptr_t)lv_event_get_user_data(e)); },
                LV_EVENT_CLICKED, (void*)(intptr_t)i);
            view.pod = lv_obj_create(view.card);
            lv_obj_remove_style_all(view.pod);
            lv_obj_set_style_radius(view.pod, UI_RADIUS_SM, 0);
            lv_obj_clear_flag(view.pod, LV_OBJ_FLAG_CLICKABLE);
            view.icon = lv_label_create(view.pod);
            lv_obj_set_style_text_font(view.icon, &font_material_symbols_30_4, 0);
            lv_obj_center(view.icon);
            view.text = lv_obj_create(view.card);
            lv_obj_remove_style_all(view.text);
            lv_obj_set_height(view.text, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(view.text, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(view.text, 2, 0);
            lv_obj_clear_flag(view.text, LV_OBJ_FLAG_CLICKABLE);
            view.title = lv_label_create(view.text);
            view.subtitle = lv_label_create(view.text);
            lv_label_set_long_mode(view.title, LV_LABEL_LONG_DOT);
            lv_label_set_long_mode(view.subtitle, LV_LABEL_LONG_DOT);
            lv_obj_set_width(view.title, lv_pct(100));
            lv_obj_set_width(view.subtitle, lv_pct(100));
        }
        const bool focused = s_split && i == s_focus_idx;
        const bool paint_chrome = created || s_force_paint || view.focused != focused;
        if (paint_chrome) {
            if (rail) {
                ui_style_card(view.card);
                const bool light = ui_palette_is_dark() == 0;
                lv_obj_set_style_bg_color(view.card,
                                          focused ? (light ? UI_ACCENT : UI_SELECTION) : UI_SURFACE, 0);
                lv_obj_set_style_bg_opa(view.card, LV_OPA_COVER, 0);
                lv_obj_set_style_border_width(view.card, focused && !light ? 1 : 0, 0);
                lv_obj_set_style_border_color(view.card, focused ? UI_ACCENT : UI_TILE_OFF_BORDER, 0);
                lv_obj_set_style_border_opa(view.card, focused ? LV_OPA_COVER : LV_OPA_80, 0);
            } else {
                /* 全宽网格：卡片无背景无边框无阴影 */
                lv_obj_set_style_bg_opa(view.card, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(view.card, 0, 0);
                lv_obj_set_style_shadow_width(view.card, 0, 0);
            }
            view.focused = focused;
        }
        const bool changed = created || s_force_paint || strcmp(view.item.id, item.id) ||
                             strcmp(view.item.name, item.name) ||
                             strcmp(view.item.desc, item.desc) || view.item.enabled != item.enabled;
        const char *desc = !item.enabled ? "不可用" : (item.desc[0] ? item.desc : "点按打开");
        if (changed || paint_chrome) {
            if (item.enabled) {
                lv_obj_add_flag(view.card, LV_OBJ_FLAG_CLICKABLE);
            } else {
                lv_obj_clear_flag(view.card, LV_OBJ_FLAG_CLICKABLE);
            }
            const bool light = ui_palette_is_dark() == 0;
            const bool light_on = light && focused && item.enabled;
            /* 全宽网格与分栏左轨统一色 Pod；浅色选中时反白字形保对比。 */
            const lv_color_t pod_color =
                item.enabled ? lv_color_hex(apps_color_for(item.id)) : UI_CAPSULE_OFF;
            lv_obj_set_style_bg_color(view.pod, light_on ? UI_SURFACE : pod_color, 0);
            lv_obj_set_style_bg_opa(view.pod, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(view.pod, UI_RADIUS_SM, 0);
            lv_obj_set_style_text_color(view.icon,
                                        light_on ? UI_ACCENT
                                                 : (item.enabled ? lv_color_white() : UI_TEXT_MUTED),
                                        0);
            lv_label_set_text(view.icon, apps_glyph_for(item.id));
            lv_label_set_text(view.title, item.name[0] ? item.name : item.id);
            lv_label_set_text(view.subtitle, desc);
            lv_obj_set_style_text_color(view.title,
                                        light_on ? UI_ON_TILE
                                                 : (item.enabled ? UI_TEXT : UI_TEXT_MUTED),
                                        0);
            lv_obj_set_style_text_color(
                view.subtitle,
                light_on ? UI_ON_TILE : (item.enabled ? UI_TEXT_DIM : UI_TEXT_MUTED), 0);
            view.item = item;
        }
        if (created || changed || view.width != card_w || view.height != card_h ||
            view.pod_size != pod_size || view.line != line || view.font != font ||
            view.rail != (int8_t)rail) {
            lv_obj_set_size(view.card, card_w, card_h);
            lv_obj_set_size(view.pod, pod_size, pod_size);
            lv_obj_center(view.icon);
            lv_obj_update_layout(view.icon);
            lv_obj_set_style_transform_pivot_x(view.icon, lv_obj_get_width(view.icon) / 2, 0);
            lv_obj_set_style_transform_pivot_y(view.icon, lv_obj_get_height(view.icon) / 2, 0);
            lv_obj_set_style_transform_scale_x(view.icon, icon_scale, 0);
            lv_obj_set_style_transform_scale_y(view.icon, icon_scale, 0);
            nl_ov_set_font(view.title);
            nl_ov_set_font(view.subtitle);
            if (rail) {
                lv_obj_add_flag(view.subtitle, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_long_mode(view.title, LV_LABEL_LONG_DOT);
                lv_obj_set_height(view.title, font_h);
                lv_obj_set_flex_flow(view.card, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(view.card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_hor(view.card, 10, 0);
                lv_obj_set_style_pad_ver(view.card, 0, 0);
                lv_obj_set_style_pad_column(view.card, 12, 0);
                lv_obj_set_style_pad_row(view.card, 0, 0);
                lv_obj_set_flex_grow(view.text, 1);
                lv_obj_set_width(view.text, 10);
                lv_obj_set_flex_align(view.text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_text_align(view.title, LV_TEXT_ALIGN_LEFT, 0);
            } else if (wide_grid) {
                /* P4 全宽：单行打点；图标与标题间距略加大 */
                lv_obj_add_flag(view.subtitle, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_long_mode(view.title, LV_LABEL_LONG_DOT);
                lv_obj_set_height(view.title, font_h);
                lv_obj_set_flex_flow(view.card, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_flex_align(view.card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_hor(view.card, 0, 0);
                lv_obj_set_style_pad_top(view.card, 2, 0);
                lv_obj_set_style_pad_bottom(view.card, 4, 0);
                lv_obj_set_style_pad_row(view.card, large ? 12 : 10, 0);
                lv_obj_set_style_pad_column(view.card, 0, 0);
                lv_obj_set_flex_grow(view.text, 0);
                lv_obj_set_width(view.text, lv_pct(100));
                lv_obj_set_flex_align(view.text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_text_align(view.title, LV_TEXT_ALIGN_CENTER, 0);
            } else {
                /* 方屏：两行居中；全名不截断。86 大字行高含 line_space，避免第二行被裁。 */
                const int title_h = font_h + line;
                const int icon_gap = (sila86 && large) ? 6 : 8;
                lv_obj_add_flag(view.subtitle, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_long_mode(view.title, LV_LABEL_LONG_WRAP);
                lv_obj_set_height(view.title, title_h);
                lv_obj_set_flex_flow(view.card, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_flex_align(view.card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_hor(view.card, sila86 && large ? 2 : 0, 0);
                lv_obj_set_style_pad_top(view.card, 4, 0);
                lv_obj_set_style_pad_bottom(view.card, 4, 0);
                lv_obj_set_style_pad_row(view.card, icon_gap, 0);
                lv_obj_set_style_pad_column(view.card, 0, 0);
                lv_obj_set_flex_grow(view.text, 0);
                lv_obj_set_width(view.text, lv_pct(100));
                lv_obj_set_flex_align(view.text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_text_align(view.title, LV_TEXT_ALIGN_CENTER, 0);
            }
            view.rail = (int8_t)rail;
            view.width = card_w;
            view.height = card_h;
            view.pod_size = pod_size;
            view.line = line;
            view.font = font;
        }
    }
    s_force_paint = false;
}

void nl_ov_apps_reapply_appearance(void)
{
    if (s_page) {
        lv_obj_set_style_bg_color(s_page, UI_BG, 0);
        lv_obj_set_style_bg_opa(s_page, ui_palette_is_immersive() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
    if (s_hint) {
        lv_obj_set_style_text_color(s_hint, UI_TEXT_MUTED, 0);
    }
    if (!s_list) {
        return;
    }
    s_force_paint = true;
    if (s_ready && s_page && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
        s_apps_dirty = true;
        return;
    }
    render_list();
}

static void apps_split_anim_ready(lv_anim_t *a)
{
    (void)a;
    s_split_vis = s_split;
    apps_apply_pane(apps_pane_target_w(s_split), s_split);
    render_list();
    nodelio_skill_result_layout();
    nl_ov_apps_layout_chrome();
}

static void apps_set_split(bool on, bool animate)
{
    if (!apps_wide()) {
        on = false;
    }
    if (!s_list_pane) {
        s_split = on;
        s_split_vis = on;
        return;
    }
    if (s_split == on && s_split_vis == on) {
        return;
    }
    s_split = on;
    if (!on) {
        s_focus_idx = -1;
    }
    const int to_w = apps_pane_target_w(on);
    lv_anim_delete(s_list_pane, apps_exec_pane_w);
    if (!animate || !apps_wide() || !s_screen_on || lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
        s_split_vis = on;
        apps_apply_pane(to_w, on);
        render_list();
        nodelio_skill_result_layout();
        nl_ov_apps_layout_chrome();
        return;
    }
    /* 开：先切单列再收窄。关：视觉仍单列拉宽，结束再切六列（避免一行多卡闪一下）。 */
    if (on) {
        s_split_vis = true;
        render_list();
    } else if (s_list) {
        s_split_vis = true;
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
    }
    int from_w = (int)lv_obj_get_width(s_list_pane);
    if (from_w < 80) {
        from_w = apps_pane_target_w(!on);
    }
    lv_obj_set_flex_grow(s_list_pane, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_list_pane);
    lv_anim_set_values(&a, from_w, to_w);
    lv_anim_set_time(&a, kAppsAnimMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, apps_exec_pane_w);
    lv_anim_set_completed_cb(&a, apps_split_anim_ready);
    lv_anim_start(&a);
    nodelio_skill_result_layout();
    nl_ov_apps_layout_chrome();
}

void nl_ov_apps_relayout(void)
{
    if (!s_list) {
        return;
    }
    if (s_hint) {
        nl_ov_set_font(s_hint);
    }
    render_list();
    nl_ov_apps_layout_chrome();
}

int nl_ov_apps_split_rail_w(void)
{
    if (!s_split || !apps_wide()) {
        return 0;
    }
    return apps_pane_target_w(true);
}

void nl_ov_apps_notify_skill_open(bool open) {
    /* 已分栏时技能卡静默刷新勿重摆列表 */
    if (open && s_split) {
        return;
    }
    apps_set_split(open, true);
}

static bool items_from_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    int n = 0;
    const cJSON *it = nullptr;
    nl_app_item tmp[APP_MAX];
    memset(tmp, 0, sizeof(tmp));
    cJSON_ArrayForEach(it, root) {
        if (n >= APP_MAX) {
            break;
        }
        copy_field(tmp[n].id, sizeof(tmp[n].id), it, "id");
        if (!tmp[n].id[0]) {
            continue;
        }
        copy_field(tmp[n].name, sizeof(tmp[n].name), it, "name");
        copy_field(tmp[n].desc, sizeof(tmp[n].desc), it, "desc");
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "en");
        tmp[n].enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
        const cJSON *ob = cJSON_GetObjectItemCaseSensitive(it, "ob");
        tmp[n].outbound_bridge = cJSON_IsTrue(ob);
        n++;
    }
    cJSON_Delete(root);
    with_builtin_daily(tmp, n);
    s_item_n = n;
    if (n > 0) {
        memcpy(s_items, tmp, sizeof(nl_app_item) * (size_t)n);
    }
    s_have_list = true;
    return true;
}

static void persist_list(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }
    for (int i = 0; i < s_item_n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            continue;
        }
        cJSON_AddStringToObject(o, "id", s_items[i].id);
        cJSON_AddStringToObject(o, "name", s_items[i].name);
        cJSON_AddStringToObject(o, "desc", s_items[i].desc);
        cJSON_AddBoolToObject(o, "en", s_items[i].enabled);
        if (s_items[i].outbound_bridge) {
            cJSON_AddBoolToObject(o, "ob", true);
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        return;
    }
    (void)nodelio_apps_cache_save(printed, strlen(printed));
    cJSON_free(printed);
}

static void load_disk_cache(void)
{
    if (s_have_list) {
        return;
    }
    char *json = nullptr;
    if (nodelio_apps_cache_load(&json) != ESP_OK || !json) {
        return;
    }
    if (!items_from_json(json)) {
        ESP_LOGW(TAG, "apps cache parse fail");
    }
    free(json);
}

/** @param skills_json 成功时的 /panel/skills 原文（PSRAM，所有权转给本函数）；失败可空 */
static void apply_snap(nl_app_snap *snap, char *skills_json)
{
    Application::GetInstance().Schedule([snap, skills_json]() {
        s_loading = false;
        auto feed_home_pins = [skills_json]() mutable {
            if (!skills_json) {
                nl_ov_home_skills_invalidate();
                return;
            }
            cJSON *root = cJSON_Parse(skills_json);
            heap_caps_free(skills_json);
            skills_json = nullptr;
            if (root) {
                nl_ov_home_skills_ingest_skills_root(root);
                cJSON_Delete(root);
            } else {
                nl_ov_home_skills_invalidate();
            }
        };
        if (snap->err[0]) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                if (!s_have_list) {
                    s_item_n = 0;
                    with_builtin_daily(s_items, s_item_n);
                    set_hint(snap->err);
                    render_list();
                } else {
                    nl_ov_toast(snap->err);
                }
            }
            if (skills_json) {
                heap_caps_free(skills_json);
            } else {
                nl_ov_home_skills_invalidate();
            }
            free(snap);
            return;
        }
        const int n = snap->n > APP_MAX ? APP_MAX : snap->n;
        /* 清单未变：不 clean/重建、不反复写 NVS（否则刷新必闪屏 + NVS 空间告警） */
        if (s_have_list && apps_list_eq(snap->items, n)) {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            }
            feed_home_pins();
            free(snap);
            return;
        }
        /* 先落盘再刷 UI：MQTT skills 推送时应用页可能未显示，锁失败也不能丢缓存 */
        s_item_n = n;
        if (s_item_n > 0) {
            memcpy(s_items, snap->items, sizeof(nl_app_item) * (size_t)s_item_n);
        }
        s_have_list = true;
        persist_list();
        feed_home_pins();
        {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
                render_list();
            }
        }
        free(snap);
    });
}

static void fill_snap_from_skills_root(const cJSON *root, nl_app_snap *snap)
{
    if (!root || !snap) {
        return;
    }
    const cJSON *skills = cJSON_GetObjectItemCaseSensitive(root, "skills");
    if (cJSON_IsArray(skills)) {
        const cJSON *it = nullptr;
        cJSON_ArrayForEach(it, skills) {
            if (snap->n >= APP_MAX) {
                break;
            }
            nl_app_item *row = &snap->items[snap->n];
            copy_field(row->id, sizeof(row->id), it, "skillId");
            if (!row->id[0]) {
                copy_field(row->id, sizeof(row->id), it, "instanceId");
            }
            if (!row->id[0] || skill_is_button_only(row->id)) {
                continue;
            }
            copy_app_label(row->name, sizeof(row->name), it);
            copy_field(row->desc, sizeof(row->desc), it, "description");
            const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
            row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
            const cJSON *tr = cJSON_GetObjectItemCaseSensitive(it, "invokeTransport");
            row->outbound_bridge =
                cJSON_IsString(tr) && tr->valuestring &&
                strcmp(tr->valuestring, "outbound_bridge") == 0;
            snap->n++;
        }
    }
    /* skills 截断/异常时回退瘦身 apps[] */
    if (snap->n == 0) {
        const cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
        if (cJSON_IsArray(apps)) {
            const cJSON *it = nullptr;
            cJSON_ArrayForEach(it, apps) {
                if (snap->n >= APP_MAX) {
                    break;
                }
                nl_app_item *row = &snap->items[snap->n];
                copy_field(row->id, sizeof(row->id), it, "instance_id");
                if (!row->id[0]) {
                    copy_field(row->id, sizeof(row->id), it, "app_id");
                }
                if (!row->id[0] || skill_is_button_only(row->id)) {
                    continue;
                }
                copy_app_label(row->name, sizeof(row->name), it);
                copy_field(row->desc, sizeof(row->desc), it, "summary");
                const cJSON *en = cJSON_GetObjectItemCaseSensitive(it, "enabled");
                row->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
                const cJSON *tr = cJSON_GetObjectItemCaseSensitive(it, "invoke_transport");
                row->outbound_bridge =
                    cJSON_IsString(tr) && tr->valuestring &&
                    strcmp(tr->valuestring, "outbound_bridge") == 0;
                snap->n++;
            }
        }
    }
    with_builtin_daily(snap->items, snap->n);
}

void nl_ov_apps_ingest_skills_root(const cJSON *root)
{
    if (!root) {
        return;
    }
    auto *snap = static_cast<nl_app_snap *>(calloc(1, sizeof(nl_app_snap)));
    if (!snap) {
        return;
    }
    fill_snap_from_skills_root(root, snap);
    const int n = snap->n > APP_MAX ? APP_MAX : snap->n;
    const bool same = s_have_list && apps_list_eq(snap->items, n);
    if (!same) {
        /* 同步落盘；UI 延后。不 invalidate 钉选——bootstrap 会紧接着 ingest pins */
        s_item_n = n;
        if (s_item_n > 0) {
            memcpy(s_items, snap->items, sizeof(nl_app_item) * (size_t)s_item_n);
        } else {
            s_item_n = 0;
        }
        s_have_list = true;
        persist_list();
    } else {
        s_have_list = true;
    }
    Application::GetInstance().Schedule([snap, same]() {
        s_loading = false;
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            if (!same) {
                render_list();
            }
        }
        free(snap);
    });
}

void nl_ov_apps_ensure(void)
{
    if (!s_page) {
        return;
    }
    if (!s_have_list) {
        load_disk_cache();
    }
    if (s_have_list) {
        /* 已画过则只显示，不重拉、不重建（避免每次进页都滚回顶部） */
        if (s_list && lv_obj_get_child_cnt(s_list) == 0) {
            set_hint(s_item_n > 0 ? "点按打开" : "暂无已安装应用");
            render_list();
        }
        return;
    }
    nl_ov_apps_refresh();
}

void nl_ov_apps_refresh(void)
{
    /* 不依赖 s_page：云端装/卸推 skills 时也要拉清单并 persist，否则重启仍读旧缓存 */
    if (s_loading) {
        return;
    }
    nodelio_nvs_config_t http_cfg{};
    nl_ov_cfg_for_http(&http_cfg);
    if (!http_cfg.server_url[0] || !http_cfg.device_id[0]) {
        s_item_n = 0;
        with_builtin_daily(s_items, s_item_n);
        set_hint(!http_cfg.server_url[0] ? "未绑定服务器" : "设备未注册");
        render_list();
        return;
    }
    s_loading = true;
    set_hint("正在加载…");
    auto *cfgp = static_cast<nodelio_nvs_config_t *>(malloc(sizeof(nodelio_nvs_config_t)));
    if (!cfgp) {
        s_loading = false;
        set_hint("内存不足");
        return;
    }
    *cfgp = http_cfg;
    /* Bounded background lane owns the worker stack. */
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Background,
            [](void* p) {
                auto *cfg = static_cast<nodelio_nvs_config_t *>(p);
                auto *snap = static_cast<nl_app_snap *>(calloc(1, sizeof(nl_app_snap)));
                if (!snap) {
                    nodelio_panel_ui_toast_defer("内存不足");
                    free(cfg);
                    Application::GetInstance().Schedule([]() { s_loading = false; });
                    return;
                }
                char base[160];
                nl_url_strip_base(cfg->server_url, base, sizeof(base));
                char url[320];
                snprintf(url, sizeof(url), "%s/api/devices/%s/panel/skills", base, cfg->device_id);
                char *buf = static_cast<char *>(heap_caps_malloc(APP_LIST_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!buf) {
                    buf = static_cast<char *>(malloc(APP_LIST_BUF));
                }
                if (!buf) {
                    snprintf(snap->err, sizeof(snap->err), "内存不足");
                    apply_snap(snap, nullptr);
                    free(cfg);
                    return;
                }
                buf[0] = '\0';
                int status = 0;
                esp_err_t err = ESP_FAIL;
                /* nl_http 已 grace+空收重试；外层再隔开 1 次兜底（新设备首开） */
                static const uint32_t k_retry_ms[] = {0, 1200};
                for (unsigned attempt = 0; attempt < sizeof(k_retry_ms) / sizeof(k_retry_ms[0]); attempt++) {
                    if (k_retry_ms[attempt]) {
                        vTaskDelay(pdMS_TO_TICKS(k_retry_ms[attempt]));
                    }
                    if (nodelio_wifi_is_asleep() || !nodelio_wifi_has_ip()) {
                        err = ESP_ERR_INVALID_STATE;
                        break;
                    }
                    buf[0] = '\0';
                    status = 0;
                    err = nodelio_http_get_ms(url, cfg->device_key, nullptr, &status, buf, APP_LIST_BUF - 1, 8000);
                    if (err == ESP_OK && (status == 200 || status == 201)) {
                        break;
                    }
                    ESP_LOGW(TAG, "list skills attempt %u status=%d err=%d", attempt + 1, status, err);
                }
                char *skills_json = nullptr;
                if (err != ESP_OK || (status != 200 && status != 201)) {
                    snprintf(snap->err, sizeof(snap->err), "加载失败");
                    ESP_LOGW(TAG, "list skills status=%d err=%d", status, err);
                } else {
                    cJSON *root = cJSON_Parse(buf);
                    if (!root) {
                        snprintf(snap->err, sizeof(snap->err), "列表过大");
                        ESP_LOGW(TAG, "list skills JSON parse fail len=%u", (unsigned)strlen(buf));
                    } else {
                        fill_snap_from_skills_root(root, snap);
                        cJSON_Delete(root);
                        const size_t blen = strlen(buf) + 1;
                        skills_json = static_cast<char *>(
                            heap_caps_malloc(blen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                        if (skills_json) {
                            memcpy(skills_json, buf, blen);
                        }
                    }
                }
                free(buf);
                free(cfg);
                apply_snap(snap, skills_json);
            },
            cfgp, [](void* p) { free(p); })) {
        free(cfgp);
        s_loading = false;
        set_hint("无法加载");
    }
}

lv_obj_t *nl_ov_apps_scroll_obj(void)
{
    if (!s_page || !s_list) {
        return nullptr;
    }
    if (lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    if (!lv_obj_has_flag(s_list, LV_OBJ_FLAG_SCROLLABLE)) {
        return nullptr;
    }
    return s_list;
}

void nl_ov_apps_layout_chrome(void)
{
    /* 应用列表刷新 FAB 已去掉；保留空实现供音乐球/技能卡回调。 */
}

void nl_ov_apps_set_visible(bool on)
{
    if (!s_page) {
        return;
    }
    if (on) {
        lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
        if (s_apps_dirty) {
            render_list();
        }
    } else {
        lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
        if (lv_anim_delete(s_list_pane, apps_exec_pane_w)) {
            apps_apply_pane(apps_pane_target_w(s_split), s_split);
            s_apps_dirty = true;
        }
    }
}

void nl_ov_apps_reserve_bottom(int bottom) {
    if (s_page && apps_wide()) {
        lv_obj_set_height(s_page, nodelio_display_profile()->height - bottom);
    }
}

/** 在已装列表里找 app_id 后缀匹配（兼容 market::app_id）。prefer_status：DSH 浏览优先进度。 */
static const char *find_installed_app_id(const char *app_key, bool prefer_status)
{
    if (!app_key || !app_key[0]) {
        return nullptr;
    }
    const char *fallback = nullptr;
    for (int i = 0; i < s_item_n; i++) {
        if (!s_items[i].enabled || !s_items[i].id[0]) {
            continue;
        }
        const char *id = s_items[i].id;
        const char *tail = strstr(id, "::");
        const char *key = tail ? tail + 2 : id;
        if (strcmp(id, app_key) != 0 && strcmp(key, app_key) != 0) {
            continue;
        }
        if (prefer_status && strstr(id, "dsh.status") != nullptr) {
            return id;
        }
        if (!fallback) {
            fallback = id;
        }
    }
    return fallback;
}

/* cJSON 保留指令、路径和会话 ID 中的引号/反斜杠；队列拒绝超长 JSON，绝不截断。 */
static bool invoke_dsh(const char* skill, const char* instruction, const char* workspace,
                       const char* session, const char* bridge = nullptr,
                       const char* provider = nullptr) {
    cJSON* params = cJSON_CreateObject();
    if (!params) {
        nl_ov_toast("内存不足，未发送");
        return false;
    }
    const bool valid =
        (!instruction || !strstr(nl_workbench_key(skill), ".status") ||
         cJSON_AddStringToObject(params, "operation", "task")) &&
        (!instruction || cJSON_AddStringToObject(params, "instruction", instruction)) &&
        (!workspace || !workspace[0] || cJSON_AddStringToObject(params, "workspace", workspace)) &&
        (!session || !session[0] || cJSON_AddStringToObject(params, "session", session)) &&
        (!bridge || !bridge[0] || cJSON_AddStringToObject(params, "bridge_id", bridge)) &&
        (!provider || !provider[0] || cJSON_AddStringToObject(params, "provider", provider));
    char* body = valid ? cJSON_PrintUnformatted(params) : nullptr;
    cJSON_Delete(params);
    if (!body) {
        nl_ov_toast("内存不足，未发送");
        return false;
    }
    const bool queued = nl_ov_invoke_skill_async(skill, body);
    cJSON_free(body);
    return queued;
}

void nl_ov_apps_invoke_dsh_status(const char *workspace, const char *session)
{
    const char *sid = find_installed_app_id("dsh.status", true);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.status";
    }
    (void)invoke_dsh(sid, nullptr, workspace, session);
}

bool nl_ov_apps_invoke_workbench_task(const char* app, const char* instruction,
                                      const char* workspace, const char* session,
                                      const char* bridge, const char* provider) {
    if (!nl_workbench_app(app) || !instruction || !instruction[0]) {
        nl_ov_toast("请选择工作台并输入指令");
        return false;
    }
    const char* installed = find_installed_app_id(app, false);
    if (!invoke_dsh(installed ? installed : app, instruction, workspace, session, bridge, provider))
        return false;
    nodelio_skill_dsh_on_dispatched(instruction);
    return true;
}

bool nl_ov_apps_invoke_dsh_task(const char* instruction, const char* workspace,
                                const char* session) {
    if (!instruction || !instruction[0]) {
        nodelio_panel_ui_announce_spoken("请说让它，再加上要做的事。");
        return true;
    }
    const char *sid = find_installed_app_id("dsh.task", false);
    if (!sid) {
        sid = "com.nodelio.dsh::dsh.task";
    }
    if (!invoke_dsh(sid, instruction, workspace, session)) {
        return false;
    }
    nodelio_skill_dsh_on_dispatched(instruction);
    /* dsh.task 已返回含目标会话的进度列表。定时补拉会与本次响应竞态，覆盖真实结果。 */
    return true;
}

enum {
    OPEN_PLAN_NONE = 0,
    OPEN_PLAN_PAGE,
    OPEN_PLAN_SD,
    OPEN_PLAN_SD_PHOTO,
    OPEN_PLAN_SKILL,
};

struct OpenPlan {
    int kind;
    char sid[72];
};

/** 只判断打不打得开，不切页、不请求。sid 拷进 plan，避免指向列表内部。 */
static int plan_open_query(const char *query, OpenPlan *plan, bool log)
{
    plan->kind = OPEN_PLAN_NONE;
    plan->sid[0] = '\0';
    if (!query || !query[0]) {
        if (log) {
            ESP_LOGW(TAG, "open_by_query: empty");
        }
        return OPEN_PLAN_NONE;
    }
    if (log) {
        ESP_LOGI(TAG, "open_by_query q='%s' installed=%d", query, s_item_n);
    }

    /* 「打开面板曲库」→ 口语「面板」前缀；「取库」≈「曲库」ASR */
    char qbuf[64];
    snprintf(qbuf, sizeof(qbuf), "%s", query);
    if (strncmp(qbuf, "面板", 6) == 0 && qbuf[6] != '\0') {
        memmove(qbuf, qbuf + 6, strlen(qbuf + 6) + 1);
    }
    if (strcmp(qbuf, "取库") == 0 || strcmp(qbuf, "去库") == 0 || strcmp(qbuf, "趣库") == 0) {
        snprintf(qbuf, sizeof(qbuf), "%s", "曲库");
    }
    if (strcmp(qbuf, "应用") == 0 || strcmp(qbuf, "面板应用") == 0) {
        plan->kind = OPEN_PLAN_PAGE;
        return OPEN_PLAN_PAGE;
    }
    query = qbuf;

    /* 内置文件：不经技能市场。有卡时应用页常驻。 */
    if (strcmp(query, "文件") == 0 || strstr(query, "文件管理") || strstr(query, "文件管理器") ||
        strstr(query, "内存卡文件")) {
        plan->kind = OPEN_PLAN_SD;
        return OPEN_PLAN_SD;
    }
    /* 内存卡相册优先于曲库：本机读卡 jpg，不经服务端。 */
    if (strstr(query, "内存卡相册") || strstr(query, "本地相册") || strstr(query, "卡相册") ||
        strstr(query, "SD相册") || strstr(query, "sd相册")) {
        plan->kind = OPEN_PLAN_SD_PHOTO;
        return OPEN_PLAN_SD_PHOTO;
    }
    /* 内存卡曲库：本机读卡，不经服务端已装技能；未接卡时给出明确提示。 */
    if (strstr(query, "内存卡") || strstr(query, "存储卡") || strstr(query, "SD卡") ||
        strstr(query, "sd卡") || strstr(query, "TF卡") || strstr(query, "tf卡") ||
        strstr(query, "本地音乐") || strstr(query, "本地曲库")) {
        plan->kind = OPEN_PLAN_SD;
        return OPEN_PLAN_SD;
    }

    /* 明确平台优先；泛称工作台使用已装默认入口，兼容仅安装 DSH 的旧配置。 */
    const char* alias_id = nl_workbench_named_app(query);
    char explicit_status[72] = {0};
    if (nl_workbench_app(query)) {
        nl_workbench_status_id(query, explicit_status, sizeof(explicit_status));
        alias_id = explicit_status;
    }
    if (alias_id) {
        // Explicit platform selection is already resolved.
    } else if (strstr(query, "派活") != nullptr || strstr(query, "dsh.task") != nullptr ||
               strstr(query, "dsh_task") != nullptr || strstr(query, "任务情况") != nullptr ||
               strstr(query, "查看任务") != nullptr || strstr(query, "任务列表") != nullptr ||
               strstr(query, "任务详情") != nullptr || strstr(query, "任务应用") != nullptr ||
               strstr(query, "任务表情") != nullptr || strstr(query, "dsh.status") != nullptr ||
               strstr(query, "进度") != nullptr || strstr(query, "工作台") != nullptr ||
               strstr(query, "deepseek") != nullptr || strstr(query, "DeepSeek") != nullptr ||
               strstr(query, "DSH") != nullptr || strstr(query, "dsh") != nullptr ||
               strstr(query, "任务") != nullptr) {
        /* 打开 DSH / 查看任务 / 派活 都进进度列表。带指令的投递走「让它…」。 */
        alias_id =
            find_installed_app_id("workbench.status", false) ? "workbench.status" : "dsh.status";
    } else if (strstr(query, "曲库") || strstr(query, "取库") || strstr(query, "去库") ||
               strstr(query, "趣库") || strstr(query, "音乐") || strstr(query, "music") ||
               strstr(query, "听歌") || strstr(query, "放歌") || strstr(query, "爱音乐")) {
        alias_id = "music.search";
    } else if (strstr(query, "RSS订阅") || strstr(query, "rss订阅") || strstr(query, "RSS 订阅") ||
               strstr(query, "新闻") || strstr(query, "news")) {
        alias_id = "news.latest";
    } else if (strstr(query, "天气") || strstr(query, "weather") || strstr(query, "日历") ||
               strstr(query, "calendar")) {
        alias_id = "builtin.daily";
    } else if (strstr(query, "电台") || strstr(query, "radio")) {
        alias_id = "radio.stations";
    } else if (strstr(query, "播客") || strstr(query, "podcast")) {
        alias_id = "podcast.latest";
    } else if (strstr(query, "空气") || strstr(query, "air")) {
        alias_id = "builtin.daily";
    } else if (strstr(query, "一篇") || strstr(query, "美文") || strstr(query, "article") ||
               strstr(query, "每日一文")) {
        alias_id = "article.daily";
    } else if (strstr(query, "一句") || strstr(query, "名言") || strstr(query, "quote")) {
        alias_id = "article.daily";
    }

    const char *sid = nullptr;
    if (alias_id) {
        const bool dsh_browse = strcmp(alias_id, "dsh.status") == 0;
        sid = find_installed_app_id(alias_id, dsh_browse);
        if (!sid) {
            sid = alias_id;
        }
        if (log) {
            ESP_LOGI(TAG, "open_by_query alias=%s → sid=%s", alias_id, sid ? sid : "(null)");
        }
    } else {
        /* 单字/过短不 fuzzy：「电」会命中「网络电台」 */
        const size_t qn = strlen(query);
        int qchars = 0;
        for (const unsigned char *p = (const unsigned char *)query; *p; ) {
            if (*p < 0x80) {
                p++;
                qchars++;
            } else if ((*p & 0xE0) == 0xC0 && p[1]) {
                p += 2;
                qchars++;
            } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
                p += 3;
                qchars++;
            } else {
                p++;
                qchars++;
            }
        }
        if (qchars >= 2 && qn >= 4) {
            for (int i = 0; i < s_item_n; i++) {
                if (!s_items[i].enabled || skill_is_button_only(s_items[i].id)) {
                    continue;
                }
                if (strstr(s_items[i].name, query) || strstr(s_items[i].id, query) ||
                    strstr(query, s_items[i].name) || strstr(query, s_items[i].id)) {
                    sid = s_items[i].id;
                    break;
                }
            }
        }
        if (log) {
            ESP_LOGI(TAG, "open_by_query fuzzy sid=%s qchars=%d", sid ? sid : "(none)", qchars);
        }
    }
    if (!sid || !sid[0]) {
        if (log) {
            ESP_LOGW(TAG, "open_by_query FAIL q='%s'", query);
        }
        return OPEN_PLAN_NONE;
    }
    snprintf(plan->sid, sizeof(plan->sid), "%.*s", (int)sizeof(plan->sid) - 1, sid);
    plan->kind = OPEN_PLAN_SKILL;
    return OPEN_PLAN_SKILL;
}

bool nl_ov_apps_query_resolves(const char *query)
{
    OpenPlan plan;
    return plan_open_query(query, &plan, false) != OPEN_PLAN_NONE;
}

bool nl_ov_apps_open_by_query(const char *query)
{
    OpenPlan plan;
    const int kind = plan_open_query(query, &plan, true);
    if (kind == OPEN_PLAN_PAGE) {
        ESP_LOGI(TAG, "open_by_query: apps page intent q='%s'", query);
        nl_ov_show_page(NL_TAB_APPS);
        nl_ov_toast("应用");
        return true;
    }
    if (kind == OPEN_PLAN_SD) {
        ESP_LOGI(TAG, "open_by_query: sd library");
        nodelio_skill_request_sd_library(false);
        return true;
    }
    if (kind == OPEN_PLAN_SD_PHOTO) {
        ESP_LOGI(TAG, "open_by_query: sd photos");
        nodelio_skill_request_sd_photos();
        return true;
    }
    if (kind != OPEN_PLAN_SKILL || !plan.sid[0]) {
        return false;
    }
    ESP_LOGI(TAG, "open_by_query invoke sid=%s", plan.sid);
    /* 新闻/每日一篇已在前台：云端二次 open_app 勿再 invoke，否则列表重刷并再问「要读吗」 */
    if (nodelio_skill_news_ui_active() &&
        (strstr(query, "新闻") || strstr(query, "news") || strstr(query, "一篇") ||
         strstr(query, "美文") || strstr(query, "article") || strstr(plan.sid, "news") ||
         strstr(plan.sid, "daily") || strstr(plan.sid, "article") ||
         strcmp(plan.sid, "builtin.daily") == 0)) {
        ESP_LOGI(TAG, "open_by_query: news already open, skip re-invoke sid=%s", plan.sid);
        return true;
    }
    if (strcmp(plan.sid, "builtin.daily") == 0) {
        for (int i = 0; i < s_item_n; ++i) {
            if (strcmp(s_items[i].id, plan.sid) == 0) {
                open_app_at(i);
                return true;
            }
        }
    }
    nl_ov_invoke_skill_async(plan.sid, "{}");
    return true;
}

void nl_ov_build_apps(void)
{
    if (s_page || !s_body) {
        return;
    }
    s_page = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_page, ui_palette_is_immersive() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_page, kAppsPagePadH, 0);
    const bool is_wide = apps_wide();
    /* P4 状态栏悬浮不占位：顶垫略紧，首排更靠上仍避开状态栏。 */
    lv_obj_set_style_pad_top(s_page, is_wide ? (NL_UI_STATUS_H + 4) : 32, 0);
    lv_obj_set_style_pad_bottom(s_page, is_wide ? 8 : 52, 0);
    lv_obj_set_style_pad_row(s_page, is_wide ? 8 : 4, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    load_disk_cache();
    with_builtin_daily(s_items, s_item_n);

    s_list_pane = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_list_pane);
    lv_obj_set_width(s_list_pane, apps_pane_target_w(false));
    lv_obj_set_flex_grow(s_list_pane, 1);
    lv_obj_set_flex_flow(s_list_pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_pane, 4, 0);
    lv_obj_clear_flag(s_list_pane, LV_OBJ_FLAG_SCROLLABLE);

    s_hint = lv_label_create(s_list_pane);
    nl_ov_set_font(s_hint);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, UI_TEXT_MUTED, 0);
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    s_list = lv_obj_create(s_list_pane);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    const bool large = nodelio_text_scale() >= 1;
    const int apps_gap_col = is_wide ? (large ? 20 : 16) : (large ? 10 : 8);
    const int apps_gap_row = is_wide ? (large ? 18 : 14) : (large ? 18 : 16);
    lv_obj_set_style_pad_row(s_list, apps_gap_row, 0);
    lv_obj_set_style_pad_column(s_list, apps_gap_col, 0);
    lv_obj_set_style_pad_bottom(s_list, is_wide ? 56 : 72, 0); /* 避让右下音乐球 */
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    if (s_split) {
        apps_apply_pane(apps_pane_target_w(true), true);
    }
    render_list();
}

bool nl_ov_skill_needs_outbound_bridge(const char *skill_id)
{
    if (!skill_id || !skill_id[0] || !s_have_list) {
        return false;
    }
    for (int i = 0; i < s_item_n; i++) {
        if (!s_items[i].id[0]) {
            continue;
        }
        if (strcmp(s_items[i].id, skill_id) == 0) {
            return s_items[i].outbound_bridge;
        }
        /* market::app 与裸 app_id 均可匹配 */
        const char *p = strstr(s_items[i].id, "::");
        const char *bare = p ? p + 2 : s_items[i].id;
        if (strcmp(bare, skill_id) == 0 || strstr(skill_id, bare) != nullptr) {
            return s_items[i].outbound_bridge;
        }
        if (strstr(s_items[i].id, skill_id) != nullptr) {
            return s_items[i].outbound_bridge;
        }
    }
    return false;
}

void nl_ov_toast_invoke_unreachable(const char *skill_id)
{
    if (nl_ov_skill_needs_outbound_bridge(skill_id)) {
        nodelio_panel_ui_toast_defer("本地桥未连接");
    } else {
        nodelio_panel_ui_toast_defer("服务器无响应");
    }
}
