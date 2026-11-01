"""Run retained-panel lifecycle code against real LVGL on a host.

Set LVGL_SOURCE to an LVGL 9 checkout and CXX to a host C++ compiler.
No display hardware, ESP-IDF toolchain or network access is used by these tests.
"""
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    name = signature.split("(", 1)[0].split()[-1].lstrip("*")
    match = re.search(r"\b" + name + r"\s*\([^;{}]*\)\s*\{", source)
    if not match:
        raise AssertionError("definition missing: " + name)
    start = source.rfind("\n", 0, match.start()) + 1
    if not source[start:match.start()].strip():
        start = source.rfind("\n", 0, start - 1) + 1
    brace = source.index("{", match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class PanelUiLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        lvgl = Path(os.environ.get("LVGL_SOURCE", ROOT / "managed_components/lvgl__lvgl"))
        compiler = shlex.split(os.environ.get("CXX", ""))
        if not compiler:
            compiler = [shutil.which("c++") or shutil.which("clang++") or ""]
        if not compiler[0] or not (lvgl / "lvgl.h").is_file():
            raise unittest.SkipTest("requires host CXX and LVGL_SOURCE (LVGL 9)")
        cls.temp = tempfile.TemporaryDirectory()
        def cleanup():
            # Windows scanners can briefly retain a just-executed .exe handle.
            for attempt in range(6):
                try:
                    cls.temp.cleanup()
                    return
                except PermissionError:
                    if attempt == 5:
                        raise
                    time.sleep(0.2 * (attempt + 1))
        cls.addClassCleanup(cleanup)
        work = Path(cls.temp.name)
        (work / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
        home = (ROOT / "main/nodelio/overlay_home_tiles.cc").read_text(encoding="utf-8")
        apps = (ROOT / "main/nodelio/overlay_apps.cc").read_text(encoding="utf-8")
        # Compile the production functions verbatim. Only external business effects
        # and the expensive whole-page builder are substituted by counters.
        app_views = apps[apps.index("struct app_view_t {"):apps.index(function(apps, "static void render_list(void)\n{"))]
        cache_type = home[home.index("struct home_cached_view_t {"):home.index("static void drop_home_cache")]
        driver = r'''
#include <lvgl.h>
#include "ui_theme.h"
#include "panel_shell.h"
#include "panel_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#define ESP_LOGD(...)
#define ESP_LOGW(...)
struct NlUiWorkScope { explicit NlUiWorkScope(const char*) {} };
#define APP_MAX 17
#define font_material_symbols_30_4 lv_font_montserrat_14
#define font_material_symbols_20_4 lv_font_montserrat_14
struct profile_t { int width=1280, height=452, nav_width=96; } profile;
const profile_t* nodelio_display_profile() { return &profile; }
constexpr int kAppsPagePadH=16, kAppsBrowseTileH=84, kAppsRailTileH=84, kAppsAnimMs=280;
constexpr int NL_UI_STATUS_H=30;
struct nl_app_item { char id[72], name[40], desc[72]; bool enabled, outbound_bridge; };
nl_app_item s_items[APP_MAX]{};
int s_item_n=3, s_focus_idx=-1;
bool s_split=false, s_ready=false, s_screen_on=true;
lv_obj_t *s_page, *s_list_pane, *s_list, *s_hint=nullptr, *s_refresh=nullptr;
int clicked=-1, chrome=0, layouts=0, prefetches=0;
void open_app_at(int i) { if(i<s_item_n && s_items[i].enabled) clicked=i; }
const char* apps_glyph_for(const char*) { return "A"; }
int font_size=14;
const lv_font_t* nl_ov_ui_font() { return font_size==14 ? &lv_font_montserrat_14 : &lv_font_montserrat_20; }
int nl_ov_ui_line_box() { return font_size+4; }
int nl_ov_ui_font_line_h() { return font_size; }
int nl_ov_wrapped_text_h(const char*, int) { return font_size*2; }
void nl_ov_set_font(lv_obj_t* o) { lv_obj_set_style_text_font(o,nl_ov_ui_font(),0); }
void nl_ov_apps_layout_chrome() { ++chrome; }
void nodelio_skill_result_layout() { ++layouts; }
void nl_ov_skill_widget_prefetch(const char*,const char*,int) { ++prefetches; }
'''
        driver += "\n".join(function(apps, name) for name in (
            "static bool apps_wide(", "static int apps_body_w(", "static int apps_pane_target_w(",
            "static void apps_apply_pane(", "static void apps_exec_pane_w("))
        driver += app_views + function(apps, "static void render_list(void)\n{")
        driver += "\n".join(function(apps, name) for name in (
            "static void apps_split_anim_ready(", "static void apps_set_split(bool on, bool animate)\n{",
            "void nl_ov_apps_set_visible("))
        driver += r'''
lv_obj_t *s_body, *s_home;
int s_grid_ox=0, s_grid_oy=0, s_home_user_n=3, s_home_user_cur=0, s_photo_rt_n=0;
int s_home_user_idxs[NL_LAYOUT_MAX_USER_PAGES]={0,1,2};
char s_home_page_id[NL_LAYOUT_STR]="p0";
nodelio_panel_layout_t layout{};
nodelio_panel_layout_t* s_layout=&layout;
uint32_t s_home_gen=0;
bool s_home_structure_dirty=false, s_home_skills_dirty=false, s_home_state_dirty=false;
bool s_home_cacheable=false;
int builds=0, state_updates=0, skill_updates=0;
struct test_device { int countdown=4; uint8_t ch_n=1; int ch_cd[1]={4}; } device;
auto* s_devs=&device;
int s_dev_n=1;
constexpr int TILE_CHILD_HINT=77;
int nl_ov_widget_countdown(const nl_widget_t* w) {
    return w->type==NL_WIDGET_SWITCH_TILE ? device.countdown : 0;
}
void format_countdown(int n, char* out, size_t cap) { snprintf(out,cap,"%d",n); }
void style_tile_visual(lv_obj_t*,const nl_widget_t*) { ++state_updates; }
void build_home_view();
void nl_ov_refresh_home_tiles() { ++state_updates; s_home_state_dirty=false; }
struct nl_widget_preview {
    bool ready, ok, list;
    char head[64];
    int n;
    char title[4][160], sub[4][64];
};
nl_widget_preview preview{};
bool nl_ov_skill_widget_preview(const char*, nl_widget_preview* out) {
    *out=preview; return preview.ready;
}
#define MATERIAL_SYMBOLS_CHAT_BUBBLE "W"
const char* material_symbols_get_utf8(const char*) { return "W"; }
static void render_preview_content(lv_obj_t*, nl_widget_t*);
void refresh_skill_widget(lv_obj_t* tile,nl_widget_t* widget) {
    ++skill_updates;
    render_preview_content(tile,widget);
}
'''
        driver += function(home, "static lv_obj_t *widget_icon_pod(")
        driver += function(home, "static void refresh_skill_widget(lv_obj_t* tile, nl_widget_t* w)\n{").replace(
            "refresh_skill_widget", "render_preview_content", 1)
        visual_start = home.index("struct tile_visual_state_t {")
        visual_end = home.index("static void style_tile_visual(lv_obj_t", visual_start)
        driver += home[visual_start:visual_end]
        driver += function(home, "static void countdown_tick_ui(")
        driver += function(home, "void nl_ov_request_home_state_refresh(")
        driver += cache_type
        driver += "\n".join(function(home, name) for name in (
            "static void home_collect_user_pages(", "static void home_resolve_current(",
            "static void drop_home_cache(", "void nl_ov_home_refresh_skill(",
            "void nl_ov_home_prepare(", "void nl_ov_fill_home(",
            "static void home_remember_current_id(", "static void home_apply_delta("))
        driver += r'''
void build_home_view() {
    ++builds; ++s_home_gen;
    lv_obj_clean(s_home);
    s_home_structure_dirty=s_home_skills_dirty=s_home_state_dirty=false;
    s_home_cacheable=s_home_user_cur!=2; // Page 2 represents a photo page.
    s_photo_rt_n=s_home_cacheable?0:1;
    lv_obj_t* tile=lv_obj_create(s_home);
    auto* w=&layout.pages[s_home_user_cur].sections[0].widgets[0];
    lv_obj_set_user_data(tile,w);
    lv_label_set_text(lv_label_create(tile),"initial");
}
void init() {
    lv_init();
    lv_display_t* display=lv_display_create(profile.width,profile.height);
    assert(display);
    s_page=lv_obj_create(lv_screen_active());
    s_list_pane=lv_obj_create(s_page);
    s_list=lv_obj_create(s_list_pane);
    lv_obj_set_flex_flow(s_list,LV_FLEX_FLOW_ROW_WRAP);
    for(int i=0;i<APP_MAX;++i) {
        snprintf(s_items[i].id,sizeof(s_items[i].id),"app%d",i);
        snprintf(s_items[i].name,sizeof(s_items[i].name),"App %d",i);
        strcpy(s_items[i].desc,"description");
        s_items[i].enabled=true;
    }
    s_body=lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_body,profile.width,profile.height);
    s_home=lv_obj_create(s_body);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home,profile.width,profile.height);
    layout.page_count=3;
    for(int i=0;i<3;++i) {
        snprintf(layout.pages[i].id,NL_LAYOUT_STR,"p%d",i);
        auto& w=layout.pages[i].sections[0].widgets[0];
        w.type=NL_WIDGET_SKILL_WIDGET;
        strcpy(w.skill_id,"weather");
    }
    lv_obj_update_layout(s_body);
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    if(mode=="square") { profile={480,480,0}; }
    init();
    if(mode=="apps" || mode=="square") {
        render_list();
        auto* card=s_app_views[1].card;
        auto* title=s_app_views[1].title;
        auto* subtitle=s_app_views[1].subtitle;
        auto* text=lv_label_get_text(title);
        s_focus_idx=1;
        apps_set_split(true,false);
        render_list();
        assert(s_app_views[1].card==card && s_app_views[1].title==title);
        assert(lv_label_get_text(title)==text); // selection does not reset text
        assert(s_split==(profile.nav_width!=0));
        assert(lv_obj_get_child_count(s_list)==3);
        s_items[1].enabled=false;
        render_list();
        assert(!lv_obj_has_flag(card,LV_OBJ_FLAG_CLICKABLE));
        s_items[1].enabled=true;
        strcpy(s_items[1].id,"replacement");
        strcpy(s_items[1].name,"New app");
        render_list();
        lv_obj_send_event(card,LV_EVENT_CLICKED,nullptr);
        assert(clicked==1 && strcmp(lv_label_get_text(title),"New app")==0);
        font_size=20; render_list();
        assert(lv_obj_get_style_text_font(title,LV_PART_MAIN)==nl_ov_ui_font());
        assert(s_app_views[1].subtitle==subtitle);
        s_ready=true; nl_ov_apps_set_visible(false);
        strcpy(s_items[1].name,"Hidden update"); render_list();
        assert(s_apps_dirty && strcmp(lv_label_get_text(title),"New app")==0);
        nl_ov_apps_set_visible(true);
        assert(!s_apps_dirty && strcmp(lv_label_get_text(title),"Hidden update")==0);
        if(profile.nav_width) {
            apps_set_split(false,false);
            apps_set_split(true,true);
            assert(lv_anim_get(s_list_pane,apps_exec_pane_w));
            nl_ov_apps_set_visible(false);
            assert(!lv_anim_get(s_list_pane,apps_exec_pane_w));
            nl_ov_apps_set_visible(true);
            assert(s_app_views[1].card==card);
        }
        s_item_n=1; render_list(); assert(!s_app_views[1].card);
        s_item_n=APP_MAX; render_list(); assert(lv_obj_get_child_count(s_list)==APP_MAX);
        for(int i=0;i<200;++i) { apps_set_split(i%2,false); render_list(); }
        assert(lv_obj_get_child_count(s_list)==APP_MAX);
    } else if(mode=="cache") {
        build_home_view(); s_ready=true;
        auto* first=s_home; auto* first_tile=lv_obj_get_child(s_home,0);
        home_apply_delta(1); auto* second=s_home;
        assert(builds==2 && s_cached_home.root==first && first!=second);
        home_apply_delta(-1);
        assert(builds==2 && s_home==first && lv_obj_get_child(s_home,0)==first_tile);
        assert(state_updates>0 && skill_updates>0 && prefetches>0);
        for(int i=0;i<200;++i) home_apply_delta(i%2 ? -1 : 1);
        assert(builds==2 && lv_obj_get_child_count(s_body)==2);
        home_apply_delta(1); home_apply_delta(1); // photo page
        assert(!s_home_cacheable && builds==3);
        auto generation=s_home_gen;
        home_apply_delta(-1);
        assert(s_home_gen>generation && s_photo_rt_n==0 && s_home==second);
        assert(!s_cached_home.root && lv_obj_get_child_count(s_body)==1);
        home_apply_delta(-1); assert(builds==4); // evicted p0 rebuilds
        home_apply_delta(1); assert(builds==4);
        lv_obj_add_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        generation=s_home_gen;
        nl_ov_fill_home(); nl_ov_fill_home();
        assert(builds==4 && !s_cached_home.root && s_home_gen>generation);
        lv_obj_clear_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        nl_ov_home_prepare(); assert(builds==5 && !s_home_structure_dirty);
        lv_obj_add_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        layout.page_count=1;
        strcpy(layout.pages[0].id,"new-layout-page");
        nl_ov_fill_home();
        assert(builds==5 && s_home_user_n==1 && s_home_user_cur==0);
        assert(strcmp(s_home_page_id,"new-layout-page")==0);
        home_apply_delta(1); assert(builds==5 && s_home_user_cur==0);
        lv_obj_clear_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        nl_ov_home_prepare(); assert(builds==6);
    } else if(mode=="countdown") {
        build_home_view(); s_ready=true;
        auto* tile=lv_obj_get_child(s_home,0);
        auto* w=static_cast<nl_widget_t*>(lv_obj_get_user_data(tile));
        w->type=NL_WIDGET_SWITCH_TILE;
        auto* hint=lv_obj_get_child(tile,0);
        lv_obj_set_user_data(hint,(void*)(intptr_t)TILE_CHILD_HINT);
        lv_label_set_text(hint,"4");
        countdown_tick_ui(); assert(device.countdown==3);
        assert(strcmp(lv_label_get_text(hint),"3")==0);
        lv_obj_add_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        countdown_tick_ui(); assert(device.countdown==2 && s_home_state_dirty);
        assert(strcmp(lv_label_get_text(hint),"3")==0);
        lv_obj_clear_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        s_home_structure_dirty=true;
        countdown_tick_ui(); assert(device.countdown==1);
        assert(strcmp(lv_label_get_text(hint),"3")==0);
        s_home_structure_dirty=false;
        countdown_tick_ui(); assert(device.countdown==0 && device.ch_cd[0]==0);
        assert(state_updates==1);
    } else if(mode=="skills") {
        build_home_view(); s_ready=true;
        auto* skill=lv_obj_get_child(s_home,0);
        auto* control=lv_obj_create(s_home);
        nl_widget_t control_data{};
        control_data.type=NL_WIDGET_SWITCH_TILE;
        lv_obj_set_user_data(control,&control_data);
        auto* label=lv_label_create(control);
        nl_ov_home_refresh_skill("unrelated"); assert(skill_updates==0);
        nl_ov_home_refresh_skill("weather");
        assert(skill_updates==1 && builds==1);
        preview.ready=preview.ok=preview.list=true; preview.n=2;
        strcpy(preview.head,"Weather"); strcpy(preview.title[0],"Today");
        layout.pages[0].sections[0].widgets[0].gh=2;
        nl_ov_home_refresh_skill("weather");
        assert(lv_obj_get_child_count(skill)==2 && skill_updates==2);
        preview.list=false; strcpy(preview.title[0],"Sunny");
        nl_ov_home_refresh_skill("weather");
        assert(strcmp(lv_label_get_text(lv_obj_get_child(skill,1)),"Sunny")==0);
        assert(lv_obj_get_child(s_home,0)==skill && lv_obj_get_child(control,0)==label);
        lv_obj_add_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        nl_ov_home_refresh_skill("weather"); nl_ov_home_refresh_skill("weather");
        assert(skill_updates==3 && s_home_skills_dirty);
        lv_obj_clear_flag(s_home,LV_OBJ_FLAG_HIDDEN);
        nl_ov_home_prepare(); assert(skill_updates==4 && builds==1);
    } else if(mode=="visual_state") {
        auto* tile=lv_obj_create(s_home);
        tile_visual_state_t state{true,false,false,0,0,&lv_font_montserrat_14};
        assert(!tile_visual_unchanged(tile,state));
        assert(tile_visual_unchanged(tile,state));
        state.on=true; assert(!tile_visual_unchanged(tile,state));
        state.countdown=10; assert(!tile_visual_unchanged(tile,state));
        state.duty=50; assert(!tile_visual_unchanged(tile,state));
        state.online=false; assert(!tile_visual_unchanged(tile,state));
        state.font=&lv_font_montserrat_20; assert(!tile_visual_unchanged(tile,state));
        lv_obj_delete(tile);
        for(int i=0;i<200;++i) {
            tile=lv_obj_create(s_home);
            assert(!tile_visual_unchanged(tile,state));
            assert(tile_visual_unchanged(tile,state));
            lv_obj_delete(tile);
        }
    } else if(mode=="shell") {
        auto* screen=lv_screen_active();
        auto before=lv_obj_get_child_count(screen);
        nl_panel_shell_setup(&lv_font_montserrat_14);
        assert(lv_obj_get_child_count(screen)==before);
        auto* top=lv_layer_top(); auto layers=lv_obj_get_child_count(top);
        nl_panel_shell_notify("hello", &lv_font_montserrat_14, 60, 100);
        assert(lv_obj_get_child_count(top)==layers+1);
        auto* toast=lv_obj_get_child(top,-1);
        nl_panel_shell_notify("second", &lv_font_montserrat_20, 30, 100);
        assert(lv_obj_get_child_count(top)==layers+1);
        assert(strcmp(lv_label_get_text(lv_obj_get_child(toast,0)),"second")==0);
        nl_panel_shell_notify(nullptr, &lv_font_montserrat_14, 60, 100);
        assert(lv_obj_has_flag(toast,LV_OBJ_FLAG_HIDDEN));
        nl_panel_shell_notify("again", &lv_font_montserrat_14, 60, 100);
        lv_tick_inc(150); lv_timer_handler();
        assert(lv_obj_has_flag(toast,LV_OBJ_FLAG_HIDDEN));
    } else if(mode=="coalesce") {
        for(int i=0;i<200;++i) nl_ov_request_home_state_refresh();
        assert(state_updates==0);
        lv_timer_handler(); assert(state_updates==1);
        nl_ov_request_home_state_refresh(); lv_timer_handler();
        assert(state_updates==2);
    } else { assert(false); }
    lv_deinit();
}
'''
        (work / "driver.cpp").write_text(driver, encoding="utf-8")
        cls.exe = work / ("panel-ui.exe" if os.name == "nt" else "panel-ui")
        args = ["-O0", "-g", "-DLV_CONF_SKIP", "-DLV_USE_STDLIB_MALLOC=1",
                "-DLV_FONT_MONTSERRAT_20=1", "-I" + str(lvgl), "-I" + str(work),
                "-I" + str(ROOT / "main/nodelio"), "-x", "c"]
        args += [str(p) for p in sorted((lvgl / "src").rglob("*.c"))]
        args += ["-x", "c++", str(work / "driver.cpp"), str(ROOT / "main/nodelio/panel_shell.cc"), "-o", str(cls.exe)]
        # Clang/GCC response files avoid Windows command-length limits.
        (work / "args.rsp").write_text("\n".join('"' + a.replace('\\', '/') + '"' for a in args))
        result = subprocess.run(compiler + ["@" + str(work / "args.rsp")],
                                capture_output=True, text=True, encoding="utf-8", timeout=300)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode("utf-8", "replace"))

    def test_wide_apps_reuse_selection_geometry_and_data(self):
        self.run_case("apps")

    def test_square_apps_keep_bottom_navigation_geometry(self):
        self.run_case("square")

    def test_page_cache_eviction_and_hidden_invalidation(self):
        self.run_case("cache")

    def test_skill_update_preserves_neighbor_controls(self):
        self.run_case("skills")

    def test_countdown_keeps_state_without_mutating_hidden_or_stale_tree(self):
        self.run_case("countdown")

    def test_native_shell_and_reused_notification_timer(self):
        self.run_case("shell")

    def test_network_state_bursts_coalesce(self):
        self.run_case("coalesce")

    def test_tile_state_cache_changes_and_deleted_object_reuse(self):
        self.run_case("visual_state")
