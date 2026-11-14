#include "sd_files_ui.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "content_envelope.h"
#include "display.h"
#include "overlay_priv.h"
#include "panel_jobs.h"
#include "panel_ui.h"
#include "sd_library.h"
#include "sd_photo_ui.h"
#include "skill_result_ui.h"
#include "ui_theme.h"
#include "ui_widgets.h"

#include <esp_heap_caps.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <material_symbols.h>

LV_FONT_DECLARE(font_material_symbols_20_4);
LV_FONT_DECLARE(font_material_symbols_30_4);

namespace {

constexpr int kMaxDepth = 3;
constexpr int kListMax = NODELIO_SD_LIST_MAX;
/* 单页最多建这么多行 obj；再多才分页。列表可滑，不再写死 6 行。 */
constexpr int kPageObjBudget = 48;
constexpr int kRowH = 40;
constexpr int kRowGap = 4;
constexpr int kIconW = 28;
constexpr int kSizeW = 72;
constexpr int kHdrBudget = 56;
constexpr int kFootBudget = 44;
constexpr int kCardPad = 12;
/* 与 overlay_apps apps_pane_target_w(true) 一致；勿等 s_split 再量宽。 */
constexpr int kAppsRailW = 220;

struct FilesUi {
    lv_obj_t *scrim = nullptr;
    lv_obj_t *card = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *sub = nullptr;
    lv_obj_t *up_btn = nullptr;
    lv_obj_t *prev_btn = nullptr;
    lv_obj_t *next_btn = nullptr;
    lv_obj_t *foot = nullptr;
    lv_obj_t *list = nullptr;
    char cwd[NODELIO_SD_ENTRY_PATH_MAX]{};
    nodelio_sd_entry_t entries[kListMax]{};
    int entry_n = 0;
    int page = 0;
    bool busy = false;
};

FilesUi s_ui;

/** 与应用页技能卡同宽：侧轨 + 应用左列表 + 右缘缝；禁止退回 result_width 留白。 */
int files_panel_w()
{
    const auto *p = nodelio_display_profile();
    if (!p || !p->nav_width) {
        return p ? static_cast<int>(p->width) : 480;
    }
    const int gap = (nodelio_card_radius_px() == 0) ? 0 : 12;
    int rail = nl_ov_apps_split_rail_w();
    if (rail <= 0) {
        /* 首帧布局时 split 常未置位；按应用页目标左轨宽算，避免卡停在 720。 */
        const int body = static_cast<int>(p->width) - static_cast<int>(p->nav_width) - 32;
        rail = kAppsRailW;
        if (rail > body) {
            rail = body > 168 ? 168 : body;
        }
    }
    const int left = static_cast<int>(p->nav_width) + 16 + rail + 16;
    int w = static_cast<int>(p->width) - left - gap;
    if (w < 320) {
        w = 320;
    }
    return w;
}

int files_panel_h()
{
    const auto *p = nodelio_display_profile();
    if (!p) {
        return 480;
    }
    if (!p->nav_width) {
        return static_cast<int>(p->height);
    }
    const int gap = (nodelio_card_radius_px() == 0) ? 0 : 12;
    int h = static_cast<int>(p->height) - gap;
    if (h < 180) {
        h = 180;
    }
    return h;
}

int files_page_size()
{
    const int card_h = files_panel_h();
    const int list_h = card_h - 2 * kCardPad - kHdrBudget - kCardPad - kFootBudget;
    int fit = 1;
    if (list_h > 0) {
        fit = (list_h + kRowGap) / (kRowH + kRowGap);
        if (fit < 1) {
            fit = 1;
        }
    }
    /* 可见行之上允许再多建一截可滑内容；整目录不超过预算时一次铺完。 */
    int page = fit * 3;
    if (page < fit) {
        page = fit;
    }
    if (page > kPageObjBudget) {
        page = kPageObjBudget;
    }
    if (s_ui.entry_n > 0 && s_ui.entry_n <= kPageObjBudget) {
        return s_ui.entry_n;
    }
    return page;
}

const char *kind_icon(nodelio_sd_entry_kind_t k)
{
    switch (k) {
        case NODELIO_SD_ENTRY_DIR:
            return MATERIAL_SYMBOLS_SD_CARD; /* 字库无 folder，用 SD 示文件夹 */
        case NODELIO_SD_ENTRY_AUDIO:
            return MATERIAL_SYMBOLS_MUSIC_NOTE;
        case NODELIO_SD_ENTRY_PHOTO:
            return MATERIAL_SYMBOLS_IMAGE;
        default:
            return MATERIAL_SYMBOLS_EDIT_SQUARE;
    }
}

lv_color_t kind_icon_color(nodelio_sd_entry_kind_t k)
{
    switch (k) {
        case NODELIO_SD_ENTRY_DIR:
            return UI_ACCENT;
        case NODELIO_SD_ENTRY_AUDIO:
            return UI_ACCENT;
        case NODELIO_SD_ENTRY_PHOTO:
            return UI_INFO;
        default:
            return UI_TEXT_MUTED;
    }
}

void format_size(uint64_t bytes, bool is_dir, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    if (is_dir) {
        snprintf(out, out_len, "—");
        return;
    }
    if (bytes == 0) {
        snprintf(out, out_len, "—");
        return;
    }
    if (bytes < 1024ull) {
        snprintf(out, out_len, "%u B", static_cast<unsigned>(bytes));
        return;
    }
    if (bytes < 1024ull * 1024ull) {
        snprintf(out, out_len, "%u KB", static_cast<unsigned>((bytes + 512ull) / 1024ull));
        return;
    }
    if (bytes < 1024ull * 1024ull * 1024ull) {
        const unsigned mb10 =
            static_cast<unsigned>((bytes * 10ull + 512ull * 1024ull) / (1024ull * 1024ull));
        snprintf(out, out_len, "%u.%u MB", mb10 / 10u, mb10 % 10u);
        return;
    }
    const unsigned gb10 =
        static_cast<unsigned>((bytes * 10ull + 512ull * 1024ull * 1024ull) /
                              (1024ull * 1024ull * 1024ull));
    snprintf(out, out_len, "%u.%u GB", gb10 / 10u, gb10 % 10u);
}

void path_basename(const char *path, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    const char *base = strrchr(path, '/');
    base = (base && base[1]) ? base + 1 : path;
    snprintf(out, out_len, "%s", base);
}

bool parent_path(const char *cwd, char *out, size_t out_len)
{
    if (!nodelio_sd_normalize_path(cwd, out, out_len)) {
        return false;
    }
    const char *mount = nodelio_sd_mount_point();
    if (strcmp(out, mount) == 0) {
        return false;
    }
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        return false;
    }
    *slash = '\0';
    if (out[0] == '\0' || strcmp(out, "/") == 0) {
        snprintf(out, out_len, "%s", mount);
    }
    return nodelio_sd_normalize_path(out, out, out_len);
}

void layout_shell()
{
    if (!s_ui.card) {
        return;
    }
    const auto *profile = nodelio_display_profile();
    if (!profile) {
        return;
    }
    const bool wide = profile->nav_width != 0;
    const int gap = (wide && nodelio_card_radius_px() == 0) ? 0 : 12;
    const int card_w = files_panel_w();
    const int card_h = files_panel_h();
    lv_obj_set_size(s_ui.card, card_w, card_h);
    if (wide) {
        lv_obj_align(s_ui.card, LV_ALIGN_TOP_RIGHT, -gap, gap);
        if (s_ui.scrim) {
            lv_obj_set_size(s_ui.scrim, card_w + gap, profile->height);
            lv_obj_align(s_ui.scrim, LV_ALIGN_TOP_RIGHT, 0, 0);
        }
        nl_ov_workspace_reserve(card_w + gap, 0);
    } else {
        lv_obj_align(s_ui.card, LV_ALIGN_TOP_LEFT, 0, 0);
        if (s_ui.scrim) {
            lv_obj_set_size(s_ui.scrim, profile->width, profile->height);
            lv_obj_align(s_ui.scrim, LV_ALIGN_TOP_LEFT, 0, 0);
        }
        nl_ov_workspace_reserve(card_w, 0);
    }
}

lv_obj_t *make_chip_btn(lv_obj_t *parent, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, 40);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *lb = lv_label_create(btn);
    nl_ov_set_font(lb);
    lv_label_set_text(lb, label ? label : "");
    lv_obj_set_style_text_color(lb, UI_TEXT, 0);
    lv_obj_center(lb);
    return btn;
}

lv_obj_t *make_close_btn(lv_obj_t *parent)
{
    constexpr int kSz = 48;
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSz, kSz);
    lv_obj_set_style_radius(btn, kSz / 2, 0);
    lv_obj_set_style_bg_color(btn, UI_CAPSULE_OFF, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 16);
    lv_obj_add_event_cb(btn, [](lv_event_t *) { nodelio_skill_sd_files_hide(); }, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t *ic = lv_label_create(btn);
    lv_obj_set_style_text_font(ic, &font_material_symbols_30_4, 0);
    lv_label_set_text(ic, MATERIAL_SYMBOLS_CLOSE);
    lv_obj_set_style_text_color(ic, UI_TEXT_SUB, 0);
    lv_obj_center(ic);
    return btn;
}

void hide_locked()
{
    if (s_ui.card) {
        lv_obj_delete(s_ui.card);
        s_ui.card = nullptr;
        s_ui.title = nullptr;
        s_ui.sub = nullptr;
        s_ui.up_btn = nullptr;
        s_ui.prev_btn = nullptr;
        s_ui.next_btn = nullptr;
        s_ui.foot = nullptr;
        s_ui.list = nullptr;
    }
    if (s_ui.scrim) {
        lv_obj_delete(s_ui.scrim);
        s_ui.scrim = nullptr;
    }
    s_ui.entry_n = 0;
    s_ui.page = 0;
    s_ui.cwd[0] = '\0';
    nl_ov_workspace_reserve(0, 0);
}

int page_count()
{
    if (s_ui.entry_n <= 0) {
        return 1;
    }
    const int ps = files_page_size();
    if (ps <= 0) {
        return 1;
    }
    return (s_ui.entry_n + ps - 1) / ps;
}

void clamp_page()
{
    const int pages = page_count();
    if (s_ui.page < 0) {
        s_ui.page = 0;
    }
    if (s_ui.page >= pages) {
        s_ui.page = pages - 1;
    }
}

void style_frame(lv_obj_t *card)
{
    const bool wide = nodelio_display_profile()->nav_width != 0;
    if (!wide) {
        lv_obj_set_style_bg_color(card, UI_BG, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 0, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        return;
    }
    lv_obj_set_style_bg_color(card, UI_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    const int rad = nodelio_card_radius_px();
    lv_obj_set_style_radius(card, rad, 0);
    lv_obj_set_style_clip_corner(card, rad > 0, 0);
    lv_obj_set_style_border_width(card, (rad > 0 && ui_palette_is_dark()) ? 1 : 0, 0);
    lv_obj_set_style_border_color(card, UI_BORDER, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_70, 0);
    if (rad > 0) {
        ui_style_card(card);
    } else {
        lv_obj_set_style_shadow_width(card, 0, 0);
    }
}

void fill_list_locked();
void ensure_shell();

struct ListBuf {
    nodelio_sd_entry_t *data = nullptr;
    bool spiram = false;
};

ListBuf alloc_list_buf()
{
    ListBuf b;
    b.data = static_cast<nodelio_sd_entry_t *>(
        heap_caps_malloc(sizeof(nodelio_sd_entry_t) * kListMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (b.data) {
        b.spiram = true;
        return b;
    }
    b.data = static_cast<nodelio_sd_entry_t *>(malloc(sizeof(nodelio_sd_entry_t) * kListMax));
    b.spiram = false;
    return b;
}

void free_list_buf(ListBuf b)
{
    if (!b.data) {
        return;
    }
    if (b.spiram) {
        heap_caps_free(b.data);
    } else {
        free(b.data);
    }
}

/** Job 线程：列举 path，成功后在 LVGL 线程刷新；path 由本函数释放。fail_toast 可空。 */
void list_dir_job(void *p)
{
    char *path = static_cast<char *>(p);
    const char *fail_toast = "无法打开文件夹";
    ListBuf buf = alloc_list_buf();
    int n = 0;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (buf.data) {
        err = nodelio_sd_list_dir(path, buf.data, kListMax, &n);
    }
    Application::GetInstance().Schedule([path, buf, n, err, fail_toast]() {
        DisplayLockGuard lock(nl_ov_disp());
        s_ui.busy = false;
        if (!lock || !s_ui.card) {
            free(path);
            free_list_buf(buf);
            return;
        }
        if (err != ESP_OK) {
            nl_ov_toast(fail_toast);
            free(path);
            free_list_buf(buf);
            return;
        }
        snprintf(s_ui.cwd, sizeof(s_ui.cwd), "%s", path);
        s_ui.entry_n = n;
        s_ui.page = 0;
        if (n > 0 && buf.data) {
            memcpy(s_ui.entries, buf.data, sizeof(nodelio_sd_entry_t) * static_cast<size_t>(n));
        } else {
            s_ui.entry_n = 0;
        }
        free(path);
        free_list_buf(buf);
        fill_list_locked();
    });
}

bool start_list_job(const char *abs_path, const char *job_key)
{
    if (s_ui.busy) {
        return false;
    }
    char *path_copy = static_cast<char *>(malloc(NODELIO_SD_ENTRY_PATH_MAX));
    if (!path_copy) {
        nl_ov_toast("内存不足");
        return false;
    }
    snprintf(path_copy, NODELIO_SD_ENTRY_PATH_MAX, "%s", abs_path);
    s_ui.busy = true;
    if (!nodelio::SubmitPanelJob(nodelio::PanelJobLane::Interactive, list_dir_job, path_copy,
                                 [](void *p) {
                                     free(p);
                                     s_ui.busy = false;
                                 },
                                 0, job_key)) {
        free(path_copy);
        s_ui.busy = false;
        nl_ov_toast("打开失败");
        return false;
    }
    return true;
}

void on_entry_click(lv_event_t *e)
{
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (idx < 0 || idx >= s_ui.entry_n) {
        return;
    }
    Board::GetInstance().NoteUserActivity();
    const nodelio_sd_entry_t *ent = &s_ui.entries[idx];
    if (ent->kind == NODELIO_SD_ENTRY_DIR) {
        const int depth = nodelio_sd_path_depth(ent->path);
        if (depth < 0 || depth > kMaxDepth) {
            nl_ov_toast("目录太深");
            return;
        }
        (void)start_list_job(ent->path, "sd_files_nav");
        return;
    }
    if (ent->kind == NODELIO_SD_ENTRY_AUDIO) {
        /* 组本夹歌单 → 技能播放器；勿裸 play_track（无 s_env，播完切下一首会崩）。 */
        nl_content_envelope_t *env = static_cast<nl_content_envelope_t *>(
            heap_caps_calloc(1, sizeof(nl_content_envelope_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!env) {
            env = static_cast<nl_content_envelope_t *>(calloc(1, sizeof(nl_content_envelope_t)));
        }
        if (!env) {
            nl_ov_toast("内存不足");
            return;
        }
        snprintf(env->status, sizeof(env->status), "ok");
        snprintf(env->app_id, sizeof(env->app_id), "music.sd");
        snprintf(env->card_type, sizeof(env->card_type), "music_list");
        snprintf(env->display_title, sizeof(env->display_title), "文件");
        snprintf(env->media_kind, sizeof(env->media_kind), "track");
        env->auto_play = true;

        auto fill_track = [](nl_env_track_t *tr, const nodelio_sd_entry_t *src) {
            path_basename(src->path, tr->title, sizeof(tr->title));
            char *dot = strrchr(tr->title, '.');
            if (dot && dot != tr->title) {
                *dot = '\0';
            }
            snprintf(tr->artist, sizeof(tr->artist), "内存卡");
            nodelio_sd_make_url(src->path, tr->playback_url, sizeof(tr->playback_url));
            tr->has_play_action = true;
        };

        /* 点中的放第 0 首（auto_play）；其后按目录顺序环绕，上一首/下一首自然。 */
        fill_track(&env->tracks[0], ent);
        int n = 1;
        for (int pass = 0; pass < 2 && n < NL_ENV_TRACK_MAX; pass++) {
            const int begin = (pass == 0) ? (idx + 1) : 0;
            const int endi = (pass == 0) ? s_ui.entry_n : idx;
            for (int i = begin; i < endi && n < NL_ENV_TRACK_MAX; i++) {
                if (s_ui.entries[i].kind != NODELIO_SD_ENTRY_AUDIO) {
                    continue;
                }
                fill_track(&env->tracks[n], &s_ui.entries[i]);
                n++;
            }
        }
        env->track_n = n;
        /* 勿在当前行 CLICKED 里删父卡；下一拍再关文件、开技能播放器。 */
        Application::GetInstance().Schedule([env]() {
            nodelio_skill_sd_files_hide();
            nodelio_skill_result_show(nullptr, env, true);
        });
        return;
    }
    if (ent->kind == NODELIO_SD_ENTRY_PHOTO) {
        nodelio_skill_sd_photo_show_path(ent->path);
        return;
    }
    nl_ov_toast("暂不支持打开");
}

void fill_list_locked()
{
    if (!s_ui.list || !s_ui.card) {
        return;
    }
    lv_obj_clean(s_ui.list);
    clamp_page();

    const char *mount = nodelio_sd_mount_point();
    const bool at_root = (s_ui.cwd[0] == '\0') || (strcmp(s_ui.cwd, mount) == 0);
    if (s_ui.up_btn) {
        if (at_root) {
            lv_obj_add_flag(s_ui.up_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.up_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const int pages = page_count();
    if (s_ui.foot) {
        if (pages <= 1) {
            lv_obj_add_flag(s_ui.foot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.foot, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.prev_btn) {
        if (pages <= 1 || s_ui.page <= 0) {
            lv_obj_add_flag(s_ui.prev_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.prev_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.next_btn) {
        if (pages <= 1 || s_ui.page + 1 >= pages) {
            lv_obj_add_flag(s_ui.next_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.next_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.sub) {
        char line[160];
        const char *rel = "/";
        if (!at_root) {
            rel = s_ui.cwd;
            const size_t mount_len = strlen(mount);
            if (strncmp(rel, mount, mount_len) == 0) {
                rel = s_ui.cwd + mount_len;
                if (*rel == '/') {
                    rel++;
                }
            }
            if (!rel[0]) {
                rel = "/";
            }
        }
        /* 路径用 %.*s 截断，避免 -Werror=format-truncation */
        constexpr int kRelMax = 96;
        const int n = s_ui.entry_n > 0 ? s_ui.entry_n : 0;
        if (n <= 0) {
            snprintf(line, sizeof(line), "%.*s · 空", kRelMax, rel);
        } else if (pages > 1) {
            snprintf(line, sizeof(line), "%.*s · %d项 · %d/%d页", kRelMax, rel, n, s_ui.page + 1,
                     pages);
        } else {
            snprintf(line, sizeof(line), "%.*s · %d项", kRelMax, rel, n);
        }
        lv_label_set_text(s_ui.sub, line);
    }

    if (s_ui.entry_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_ui.list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "此文件夹为空");
        lv_obj_set_style_text_color(empty, UI_TEXT_TIP, 0);
        return;
    }

    const int ps = files_page_size();
    const int start = s_ui.page * ps;
    int end = start + ps;
    if (end > s_ui.entry_n) {
        end = s_ui.entry_n;
    }
    for (int i = start; i < end; i++) {
        const nodelio_sd_entry_t *ent = &s_ui.entries[i];
        lv_obj_t *row = lv_obj_create(s_ui.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kRowH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        /* 直角、无阴影：避免大量 ext_draw 计算拖死 LVGL */
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_bg_color(row, UI_SURFACE_OFF, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, on_entry_click, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *ic = lv_label_create(row);
        lv_obj_set_style_text_font(ic, &font_material_symbols_20_4, 0);
        lv_label_set_text(ic, kind_icon(ent->kind));
        lv_obj_set_style_text_color(ic, kind_icon_color(ent->kind), 0);
        lv_obj_set_width(ic, kIconW);

        lv_obj_t *name = lv_label_create(row);
        nl_ov_set_font(name);
        lv_label_set_text(name, ent->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_min_width(name, 0, 0);
        lv_obj_set_style_text_color(name, UI_TEXT, 0);

        char sz[16];
        format_size(ent->size_bytes, ent->kind == NODELIO_SD_ENTRY_DIR, sz, sizeof(sz));
        lv_obj_t *size = lv_label_create(row);
        nl_ov_set_font(size);
        lv_label_set_text(size, sz);
        lv_obj_set_width(size, kSizeW);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_color(size, UI_TEXT_MUTED, 0);
    }
}

void ensure_shell()
{
    if (s_ui.card) {
        layout_shell();
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    if (!parent) {
        return;
    }
    const auto *profile = nodelio_display_profile();
    if (!profile) {
        return;
    }
    const bool wide = profile->nav_width != 0;

    s_ui.scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.scrim);
    lv_obj_set_style_bg_color(s_ui.scrim, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_ui.scrim, wide ? LV_OPA_70 : LV_OPA_COVER, 0);
    lv_obj_add_flag(s_ui.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        s_ui.scrim, [](lv_event_t *) { nodelio_skill_sd_files_hide(); }, LV_EVENT_CLICKED, nullptr);

    s_ui.card = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.card);
    style_frame(s_ui.card);
    lv_obj_set_style_pad_all(s_ui.card, kCardPad, 0);
    lv_obj_set_style_pad_row(s_ui.card, 8, 0);
    lv_obj_set_flex_flow(s_ui.card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ui.card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(s_ui.card);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, lv_pct(100));
    lv_obj_set_height(hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr_txt = lv_obj_create(hdr);
    lv_obj_remove_style_all(hdr_txt);
    lv_obj_set_flex_grow(hdr_txt, 1);
    lv_obj_set_height(hdr_txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr_txt, 2, 0);
    lv_obj_set_style_min_width(hdr_txt, 0, 0);
    lv_obj_clear_flag(hdr_txt, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.title = lv_label_create(hdr_txt);
    nl_ov_set_font(s_ui.title);
    lv_label_set_text(s_ui.title, "文件");
    lv_obj_set_style_text_color(s_ui.title, UI_TEXT, 0);

    s_ui.sub = lv_label_create(hdr_txt);
    nl_ov_set_font(s_ui.sub);
    lv_label_set_text(s_ui.sub, "/");
    lv_label_set_long_mode(s_ui.sub, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_ui.sub, lv_pct(100));
    lv_obj_set_style_text_color(s_ui.sub, UI_TEXT_MUTED, 0);

    /* 顶栏只放「上级」+ 关闭：勿用 ui_make_text_btn（会 width=100% 挤掉关闭） */
    lv_obj_t *btns = lv_obj_create(hdr);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.up_btn = make_chip_btn(btns, "上级", [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        char parent[NODELIO_SD_ENTRY_PATH_MAX];
        if (!parent_path(s_ui.cwd, parent, sizeof(parent))) {
            return;
        }
        (void)start_list_job(parent, "sd_files_up");
    });
    (void)make_close_btn(btns);

    s_ui.list = lv_obj_create(s_ui.card);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_width(s_ui.list, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, kRowGap, 0);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);

    s_ui.foot = lv_obj_create(s_ui.card);
    lv_obj_remove_style_all(s_ui.foot);
    lv_obj_set_width(s_ui.foot, lv_pct(100));
    lv_obj_set_height(s_ui.foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_ui.foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.foot, LV_OBJ_FLAG_HIDDEN);

    s_ui.prev_btn = make_chip_btn(s_ui.foot, "上一页", [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        if (s_ui.page <= 0) {
            return;
        }
        s_ui.page--;
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            fill_list_locked();
        }
    });
    s_ui.next_btn = make_chip_btn(s_ui.foot, "下一页", [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        if (s_ui.page + 1 >= page_count()) {
            return;
        }
        s_ui.page++;
        DisplayLockGuard lock(nl_ov_disp());
        if (lock) {
            fill_list_locked();
        }
    });

    layout_shell();
}

void show_listing(const char *cwd, const nodelio_sd_entry_t *entries, int n)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        return;
    }
    ensure_shell();
    if (!s_ui.card) {
        return;
    }
    snprintf(s_ui.cwd, sizeof(s_ui.cwd), "%s", cwd && cwd[0] ? cwd : nodelio_sd_mount_point());
    s_ui.entry_n = n < 0 ? 0 : (n > kListMax ? kListMax : n);
    s_ui.page = 0;
    if (entries && s_ui.entry_n > 0) {
        memcpy(s_ui.entries, entries, sizeof(nodelio_sd_entry_t) * static_cast<size_t>(s_ui.entry_n));
    } else {
        s_ui.entry_n = 0;
    }
    /* 先缩左轨再量宽，避免首帧按未分栏退回 720。 */
    nl_ov_apps_notify_skill_open(true);
    layout_shell();
    fill_list_locked();
    if (s_ui.card) {
        lv_obj_move_foreground(s_ui.card);
    }
    if (s_ui.scrim) {
        lv_obj_move_foreground(s_ui.scrim);
        lv_obj_move_foreground(s_ui.card);
    }
}

struct OpenArg {
    char path[NODELIO_SD_ENTRY_PATH_MAX];
};

void open_worker(void *p)
{
    OpenArg *arg = static_cast<OpenArg *>(p);
    nodelio_sd_mount();
    if (!nodelio_sd_is_mounted()) {
        s_ui.busy = false;
        nodelio_panel_ui_toast_defer("未检测到内存卡");
        Application::GetInstance().Schedule([]() {
            DisplayLockGuard lock(nl_ov_disp());
            if (lock) {
                nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
            }
        });
        free(arg);
        return;
    }
    const char *dir = (arg && arg->path[0]) ? arg->path : nodelio_sd_mount_point();
    ListBuf buf = alloc_list_buf();
    int n = 0;
    esp_err_t err = ESP_ERR_NO_MEM;
    if (buf.data) {
        err = nodelio_sd_list_dir(dir, buf.data, kListMax, &n);
    }
    char *cwd_copy = static_cast<char *>(malloc(NODELIO_SD_ENTRY_PATH_MAX));
    if (cwd_copy) {
        snprintf(cwd_copy, NODELIO_SD_ENTRY_PATH_MAX, "%s", dir);
    }
    free(arg);
    s_ui.busy = false;
    if (err != ESP_OK || !cwd_copy) {
        free_list_buf(buf);
        free(cwd_copy);
        nodelio_panel_ui_toast_defer(err != ESP_OK ? "读取目录失败" : "内存不足");
        return;
    }
    Application::GetInstance().Schedule([cwd_copy, buf, n]() {
        show_listing(cwd_copy, buf.data, n);
        free(cwd_copy);
        free_list_buf(buf);
    });
}

}  // namespace

extern "C" void nodelio_skill_request_sd_files(void)
{
    if (!nodelio_sd_available()) {
        nl_ov_toast("本机没有内存卡");
        return;
    }
    if (s_ui.busy) {
        nl_ov_toast("正在读取内存卡…");
        return;
    }
    nodelio_skill_result_hide();
    nodelio_skill_sd_photo_hide();
    s_ui.busy = true;
    nl_ov_toast("正在打开文件…");
    OpenArg *arg = static_cast<OpenArg *>(calloc(1, sizeof(OpenArg)));
    if (arg) {
        snprintf(arg->path, sizeof(arg->path), "%s", nodelio_sd_mount_point());
    }
    if (!nodelio::SubmitPanelJob(
            nodelio::PanelJobLane::Interactive, open_worker, arg,
            [](void *p) {
                free(p);
                s_ui.busy = false;
            },
            nodelio_panel_begin_skill_view(), "sd_files")) {
        free(arg);
        s_ui.busy = false;
        nl_ov_toast("打开文件失败");
    }
}

extern "C" bool nodelio_skill_sd_files_visible(void) { return s_ui.card != nullptr; }

extern "C" void nodelio_skill_sd_files_hide(void)
{
    DisplayLockGuard lock(nl_ov_disp());
    if (!lock) {
        hide_locked();
        if (nodelio_skill_result_card_visible()) {
            nodelio_skill_result_layout();
        }
        nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
        return;
    }
    hide_locked();
    if (nodelio_skill_result_card_visible()) {
        nodelio_skill_result_layout();
    }
    nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
}
