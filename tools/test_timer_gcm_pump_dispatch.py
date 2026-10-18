#!/usr/bin/env python3
"""Neutral production-dispatch regression for the PR353 timer GCM pump."""
import os,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import ppu_lifter
ROOT=Path(__file__).resolve().parents[1]
DRIVER=r'''
#include "ppu_recomp.h"
#include "runtime/diagnostics/timer_gcm_pump.h"
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "runtime/platform/win32_compat.h"
extern "C" {
DWORD GetFileAttributesA(const char*){return 0xffffffffu;}
void SetLastError(DWORD){}
uint32_t ps3_mmapper_block_size(uint32_t){std::abort();}
void ps3_msf(const char*,...){}
uint8_t *vm_base=nullptr;uint32_t ppu_hle_inject_base=0;
uint32_t g_gcm_consumer_put_wake_ea=0;
void cellGcm_ref_on_poll(){std::abort();}
void cellGcm_fifo_kick_notify(){std::abort();}
int spu_raw_reg_load(uint32_t,uint32_t*){std::abort();}
void spu_raw_reg_store(uint32_t,uint32_t,unsigned){std::abort();}
int spu_coh_is_reserved(uint32_t){return 0;}
void spu_coh_notify_write(uint32_t){std::abort();}
void spu_lockline_lock(){std::abort();}void spu_lockline_unlock(){std::abort();}
void ppu_prof_stamp(void*,unsigned){}uint32_t ppu_prof_resolve_host(void*){return 0;}
int64_t sys_mmapper_allocate_address(ppu_context*){std::abort();}
int64_t sys_mmapper_search_map_address(uint32_t,uint32_t,uint32_t*){std::abort();}
}
extern const func_entry function_table[]={{0,nullptr,nullptr}};
extern const uint64_t function_table_count=0;
static unsigned original_dispatches,pump_bodies;static uint64_t last_number,last_argument;
extern "C" int lv2_try_syscall(ppu_context*c){++original_dispatches;last_number=c->gpr[11];last_argument=c->gpr[3];c->gpr[3]=0x76543210;return 1;}
#ifndef WITHOUT_GCM
extern "C" void ppu_gcm_pump(){static bool active;if(active)return;active=true;++pump_bodies;ppu_context nested{};nested.gpr[11]=141;nested.gpr[3]=7;lv2_syscall(&nested);assert(nested.gpr[3]==0x76543210);active=false;}
#endif
int main(){PpuTimerGcmPumpSnapshot s{};ppu_timer_gcm_pump_get_snapshot(&s);assert(s.bytes==56&&s.enabled);ppu_timer_gcm_pump_set_enabled(0);ppu_context c{};c.thread_id=1;c.gpr[11]=141;c.gpr[3]=10;c.gpr[2]=0x123456;c.lr=0xabcdef;lv2_syscall(&c);assert(original_dispatches==1&&last_number==141&&last_argument==10&&c.gpr[3]==0x76543210&&c.gpr[2]==0x123456&&c.lr==0xabcdef&&!pump_bodies);
ppu_timer_gcm_pump_set_enabled(1);for(uint64_t n:{141ull,142ull,999ull}){c.gpr[11]=n;c.gpr[3]=13;lv2_syscall(&c);assert(last_number==n&&last_argument==13&&c.gpr[3]==0x76543210&&c.gpr[11]==n);}
ppu_timer_gcm_pump_get_snapshot(&s);assert(s.enabled&&s.eligible_142==1&&s.setter_transitions==2);
#ifndef WITHOUT_GCM
assert(pump_bodies==2&&s.pump_invocations==4&&!s.missing_pump&&s.eligible_141==4&&original_dispatches==6);
#else
assert(!pump_bodies&&!s.pump_invocations&&s.missing_pump==2&&s.eligible_141==2&&original_dispatches==4);
#endif
ppu_timer_gcm_pump_set_enabled(0);c.gpr[11]=142;c.gpr[3]=19;lv2_syscall(&c);assert(last_argument==19);ppu_timer_gcm_pump_get_snapshot(&s);assert(!s.enabled&&s.disabled_eligible==2&&s.setter_transitions==3);puts("PASS real production dispatcher141/142 gate, originalarguments/dispatch, guardednestedpump or noGCM link");}

'''
with tempfile.TemporaryDirectory(prefix="timer-gcm-dispatch-")as td:
 p=Path(td);(p/"ppu_recomp.h").write_text(ppu_lifter.PPULifter().emit_header());(p/"check.cpp").write_text(DRIVER)
 for absent in(False,True):
  out=p/("without-gcm"if absent else"with-gcm")
  flags=["-std=c++17","-O1","-ffunction-sections","-fdata-sections"]+(["-DWITHOUT_GCM"]if absent else[])
  linker=["-Wl,-dead_strip"]if sys.platform=="darwin"else["-Wl,--gc-sections"]
  # Darwin executables require naming an optional undefined import explicitly;
  # the weak_import reference remains null when the provider is not linked.
  if absent and sys.platform=="darwin":linker +=["-Wl,-U,_ppu_gcm_pump"]
  subprocess.run([os.environ.get("CXX","c++"),*flags,"-I"+str(p),"-I"+str(ROOT/"runtime/ppu"),"-I"+str(ROOT/"include"),"-I"+str(ROOT),str(ROOT/"runtime/ppu/ppu_loader.cpp"),str(p/"check.cpp"),*linker,"-pthread","-o",str(out)],check=True,timeout=60)
  subprocess.run([str(out)],check=True,timeout=10)
