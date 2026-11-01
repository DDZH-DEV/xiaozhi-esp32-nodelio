"""Execute the production bounded scheduler with a deterministic FreeRTOS adapter."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from test_panel_ui_lifecycle import function

ROOT = Path(__file__).resolve().parents[2]


class PanelJobsTests(unittest.TestCase):
    def test_ownership_capacity_epochs_and_worker_lifetime(self):
        compiler = shlex.split(os.environ.get("CXX", ""))
        if not compiler:
            compiler = [shutil.which("c++") or shutil.which("clang++") or ""]
        if not compiler[0]:
            self.skipTest("requires a host C++ compiler")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "freertos").mkdir()
            (work / "esp_heap_caps.h").write_text(
                "#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n")
            (work / "esp_log.h").write_text("")
            (work / "freertos/FreeRTOS.h").write_text("#define pdPASS 1\n")
            (work / "freertos/task.h").write_text("")
            (work / "freertos/idf_additions.h").write_text('''
#pragma once
int xTaskCreateWithCaps(void (*)(void*), const char*, unsigned, void*, unsigned, void*, unsigned);
void vTaskDeleteWithCaps(void*);
''')
            driver = r'''
#include "panel_jobs.h"
#include <cassert>
#include <vector>
#include <utility>
std::vector<std::pair<void(*)(void*),void*>> workers;
std::vector<int> ran;
int destroyed=0, discarded=0, freed=0;
bool fail=false;
int xTaskCreateWithCaps(void (*fn)(void*), const char*, unsigned bytes, void* arg,
                        unsigned, void*, unsigned caps) {
    assert(bytes==16384 && caps==3);
    if(fail) return 0;
    workers.emplace_back(fn,arg); return 1;
}
void vTaskDeleteWithCaps(void*) { ++destroyed; }
void run(void* p) { ran.push_back(*static_cast<int*>(p)); delete static_cast<int*>(p); ++freed; }
void discard(void* p) { delete static_cast<int*>(p); ++discarded; ++freed; }
void drain() { auto pending=std::move(workers); workers.clear(); for(auto w:pending) w.first(w.second); }
void check_control_replies();
int main() {
    check_control_replies();
    using namespace nodelio;
    auto lane=PanelJobLane::Control;
    fail=true;
    auto* rejected=new int(9);
    assert(!SubmitPanelJob(lane,run,rejected,discard));
    assert(freed==0 && workers.empty()); delete rejected;
    fail=false;
    for(int i=0;i<8;++i) assert(SubmitPanelJob(lane,run,new int(i),discard));
    rejected=new int(99);
    assert(!SubmitPanelJob(lane,run,rejected,discard)); delete rejected;
    assert(workers.size()==1 && freed==0);
    drain(); assert(ran.size()==8 && destroyed==1 && freed==8);
    for(int i=0;i<8;++i) assert(ran[i]==i);
    auto epoch=nodelio_panel_begin_skill_view();
    assert(SubmitPanelJob(lane,run,new int(20),discard,epoch));
    assert(SubmitPanelJob(lane,run,new int(21),discard,epoch,nullptr,true));
    nodelio_panel_begin_skill_view(); drain();
    assert(discarded==1 && ran.back()==21 && ran.size()==9);
    assert(SubmitPanelJob(lane,run,new int(30),discard,0,"photo"));
    assert(SubmitPanelJob(lane,run,new int(31),discard,0,"photo"));
    assert(discarded==2 && workers.size()==1);
    assert(SubmitPanelJob(PanelJobLane::Interactive,run,new int(40),discard));
    assert(SubmitPanelJob(PanelJobLane::Background,run,new int(50),discard));
    assert(workers.size()==3); drain();
    assert(ran[9]==31 && ran[10]==40 && ran[11]==50);
    assert(freed==14 && destroyed==5 && workers.empty());
    // Ring wrap and empty-lane restart do not retain old arguments.
    for(int round=0;round<200;++round) {
        for(int i=0;i<8;++i) assert(SubmitPanelJob(lane,run,new int(i),discard));
        drain();
    }
    assert(freed==1614 && destroyed==205);
    assert(nodelio_panel_job_result_epoch()==0);
}
'''

            home = (ROOT / "main/nodelio/overlay_home_tiles.cc").read_text(encoding="utf-8")
            driver += r"""
#include <cstring>
#include <cstdlib>
#define NL_LAYOUT_STR 72
#define ESP_LOGW(...)
#define ESP_OK 0
using esp_err_t=int;
struct nl_bind_t { int dummy; };
struct nodelio_nvs_config_t {};
struct DisplayLockGuard { explicit DisplayLockGuard(void*) {} operator bool() const { return true; } };
void* nl_ov_disp() { return nullptr; }
void nl_ov_cfg_for_http(nodelio_nvs_config_t*) {}
int http_result=0, rolled_back=0, cleared=0;
int nodelio_panel_command_ex(nodelio_nvs_config_t*,const char*,const char*,const nl_bind_t*,const char*,int*,bool) { return http_result; }
void nl_ov_toast(const char*) {}
void nl_ov_refresh_home_tiles() {}
void cmd_pending_rollback_widget(const char*) { ++rolled_back; }
void cmd_pending_clear_widget(const char*) { ++cleared; }
bool s_cloud_ok=false,s_wifi_ok=true;
void* s_cloud_lbl=nullptr;
void nl_ov_refresh_status_icons(bool,bool,const char*) {}
struct pending_t { bool active; char widget_id[72]; uint32_t sequence; };
pending_t s_cmd_pending[10]={{true,"switch",2}};
"""
            begin = home.index("struct async_cmd_item_t {")
            driver += home[begin:home.index("};", begin) + 2]
            driver += function(home, "static uint32_t cmd_pending_sequence(")
            driver += function(home, "static void async_cmd_worker(")
            driver += r"""
void check_control_replies() {
    auto reply=[](uint32_t sequence,int error) {
        auto* item=static_cast<async_cmd_item_t*>(calloc(1,sizeof(async_cmd_item_t)));
        strcpy(item->widget_id,"switch"); item->rollback_on_fail=true; item->sequence=sequence;
        http_result=error; async_cmd_worker(item);
    };
    reply(1,1); reply(1,0); assert(rolled_back==0 && cleared==0);
    reply(2,1); assert(rolled_back==1 && cleared==0);
    reply(2,0); assert(rolled_back==1 && cleared==1);
    s_cmd_pending[0].active=false;
    reply(2,1); assert(rolled_back==1); // timeout/removed layout: late failure is harmless
}
"""
            (work / "driver.cc").write_text(driver, encoding="utf-8")
            executable = work / ("jobs.exe" if os.name == "nt" else "jobs")
            command = compiler + ["-std=c++17", "-I" + str(work),
                "-I" + str(ROOT / "main/nodelio"), str(work / "driver.cc"),
                str(ROOT / "main/nodelio/panel_jobs.cc"), "-o", str(executable)]
            build = subprocess.run(command, capture_output=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stderr.decode("utf-8", "replace"))
            result = subprocess.run([str(executable)], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
