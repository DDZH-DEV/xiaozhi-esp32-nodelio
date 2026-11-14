#include "sd_files_ui.h"

#include "application.h"
#include "board.h"
#include "board_info.h"
#include "content_envelope.h"
#include "display.h"
#include "media_player.h"
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

namespace {

constexpr int kMaxDepth = 3;
constexpr int kListMax = NODELIO_SD_LIST_MAX;

struct FilesUi {
    lv_obj_t *scrim = nullptr;
    lv_obj_t *card = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *sub = nullptr;
    lv_obj_t *up_btn = nullptr;
    lv_obj_t *list = nullptr;
    char cwd[NODELIO_SD_ENTRY_PATH_MAX]{};
    nodelio_sd_entry_t entries[kListMax]{};
    int entry_n = 0;
    bool busy = false;
};

FilesUi s_ui;

const char *kind_label(nodelio_sd_entry_kind_t k)
{
    switch (k) {
        case NODELIO_SD_ENTRY_DIR:
            return "文件夹";
        case NODELIO_SD_ENTRY_AUDIO:
            return "歌曲";
        case NODELIO_SD_ENTRY_PHOTO:
            return "图片";
        default:
            return "其它";
    }
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

void hide_locked()
{
    if (s_ui.card) {
        lv_obj_delete(s_ui.card);
        s_ui.card = nullptr;
        s_ui.title = nullptr;
        s_ui.sub = nullptr;
        s_ui.up_btn = nullptr;
        s_ui.list = nullptr;
    }
    if (s_ui.scrim) {
        lv_obj_delete(s_ui.scrim);
        s_ui.scrim = nullptr;
    }
    s_ui.entry_n = 0;
    s_ui.cwd[0] = '\0';
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
        nl_env_track_t tr{};
        path_basename(ent->path, tr.title, sizeof(tr.title));
        char *dot = strrchr(tr.title, '.');
        if (dot && dot != tr.title) {
            *dot = '\0';
        }
        snprintf(tr.artist, sizeof(tr.artist), "内存卡");
        nodelio_sd_make_url(ent->path, tr.playback_url, sizeof(tr.playback_url));
        tr.has_play_action = true;
        if (nodelio_media_play_track(nullptr, &tr) != ESP_OK) {
            nl_ov_toast("无法播放");
        }
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

    const char *mount = nodelio_sd_mount_point();
    const bool at_root = (s_ui.cwd[0] == '\0') || (strcmp(s_ui.cwd, mount) == 0);
    if (s_ui.up_btn) {
        if (at_root) {
            lv_obj_add_flag(s_ui.up_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_ui.up_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ui.sub) {
        if (at_root) {
            lv_label_set_text(s_ui.sub, "/");
        } else {
            const char *rel = s_ui.cwd;
            const size_t mount_len = strlen(mount);
            if (strncmp(rel, mount, mount_len) == 0) {
                rel = s_ui.cwd + mount_len;
                if (*rel == '/') {
                    rel++;
                }
            }
            lv_label_set_text(s_ui.sub, rel[0] ? rel : "/");
        }
    }

    if (s_ui.entry_n <= 0) {
        lv_obj_t *empty = lv_label_create(s_ui.list);
        nl_ov_set_font(empty);
        lv_label_set_text(empty, "此文件夹为空");
        lv_obj_set_style_text_color(empty, UI_TEXT_TIP, 0);
        return;
    }

    for (int i = 0; i < s_ui.entry_n; i++) {
        const nodelio_sd_entry_t *ent = &s_ui.entries[i];
        lv_obj_t *row = lv_obj_create(s_ui.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 56);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, UI_SURFACE_OFF, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(row, 4);
        lv_obj_add_event_cb(row, on_entry_click, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *badge = lv_label_create(row);
        nl_ov_set_font(badge);
        lv_label_set_text(badge, kind_label(ent->kind));
        lv_obj_set_style_text_color(badge, UI_TEXT_MUTED, 0);
        lv_obj_set_width(badge, 56);

        lv_obj_t *name = lv_label_create(row);
        nl_ov_set_font(name);
        lv_label_set_text(name, ent->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_style_min_width(name, 0, 0);
        lv_obj_set_style_text_color(name, UI_TEXT, 0);
    }
}

void ensure_shell()
{
    if (s_ui.card) {
        return;
    }
    lv_obj_t *parent = lv_layer_top();
    if (!parent) {
        return;
    }
    const auto *profile = nodelio_display_profile();
    const bool wide = profile && profile->nav_width != 0;
    const int gap = UI_GAP;
    const int card_w =
        wide ? static_cast<int>(profile->result_width) : static_cast<int>(profile->width);
    const int card_h =
        wide ? static_cast<int>(profile->height) - 2 * gap : static_cast<int>(profile->height);

    s_ui.scrim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.scrim);
    lv_obj_set_size(s_ui.scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(s_ui.scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    if (wide) {
        lv_obj_set_width(s_ui.scrim, card_w + 2 * gap);
        lv_obj_align(s_ui.scrim, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_set_style_bg_opa(s_ui.scrim, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_bg_opa(s_ui.scrim, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(s_ui.scrim, UI_BG, 0);
    lv_obj_add_flag(s_ui.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        s_ui.scrim, [](lv_event_t *) { nodelio_skill_sd_files_hide(); }, LV_EVENT_CLICKED, nullptr);

    s_ui.card = lv_obj_create(parent);
    lv_obj_remove_style_all(s_ui.card);
    lv_obj_set_size(s_ui.card, card_w, card_h);
    lv_obj_align(s_ui.card, wide ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, wide ? -gap : 0,
                 wide ? gap : 0);
    style_frame(s_ui.card);
    lv_obj_set_style_pad_all(s_ui.card, 12, 0);
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

    lv_obj_t *hdr_txt = lv_obj_create(hdr);
    lv_obj_remove_style_all(hdr_txt);
    lv_obj_set_flex_grow(hdr_txt, 1);
    lv_obj_set_height(hdr_txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr_txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(hdr_txt, 2, 0);
    lv_obj_set_style_min_width(hdr_txt, 0, 0);

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

    lv_obj_t *btns = lv_obj_create(hdr);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 6, 0);

    s_ui.up_btn = ui_make_text_btn(btns, "上级", 1, [](lv_event_t *) {
        Board::GetInstance().NoteUserActivity();
        char parent[NODELIO_SD_ENTRY_PATH_MAX];
        if (!parent_path(s_ui.cwd, parent, sizeof(parent))) {
            return;
        }
        (void)start_list_job(parent, "sd_files_up");
    });

    (void)ui_make_text_btn(btns, "关闭", 1, [](lv_event_t *) { nodelio_skill_sd_files_hide(); });

    s_ui.list = lv_obj_create(s_ui.card);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_width(s_ui.list, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, 6, 0);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);
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
    if (entries && s_ui.entry_n > 0) {
        memcpy(s_ui.entries, entries, sizeof(nodelio_sd_entry_t) * static_cast<size_t>(s_ui.entry_n));
    } else {
        s_ui.entry_n = 0;
    }
    fill_list_locked();
    nl_ov_apps_notify_skill_open(true);
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
        nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
        return;
    }
    hide_locked();
    nl_ov_apps_notify_skill_open(nodelio_skill_result_card_visible());
}
