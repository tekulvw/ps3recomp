#!/usr/bin/env python3
"""Execute real lifted reserve instructions and runtime coherence publication."""
import os,sys,subprocess,tempfile,shutil
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
import ppu_lifter as L
from ppu_disasm import Instruction
root=Path(__file__).resolve().parents[1]
driver=r'''
#include "ppu_recomp.h"
#include "spu_coherency.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
extern "C" { uint8_t* vm_base=nullptr; extern PPU_THREAD_LOCAL ppu_context* g_active_ctx; extern uint32_t ppu_vm_size; uint32_t ppu_hle_inject_base=0; }
extern "C" void spu_ch_wake(spu_context*) {std::abort();}
extern "C" int spu_raw_reg_load(uint32_t,uint32_t*) {std::abort();}
extern "C" void spu_raw_reg_store(uint32_t,uint32_t,unsigned) {std::abort();}
extern "C" void cellGcm_ref_on_poll(void) {std::abort();}
// COFF retains loader sections whose imports ELF/Mach-O can discard. Provide
// their exact runtime interfaces, and fail if this reservation-only fixture
// accidentally enters profiling, syscall dispatch, or guest-thread startup.
extern "C" uint32_t spu_registry_size(void) {std::abort();}
extern "C" int spu_registry_entry(uint32_t,void**,uint32_t*) {std::abort();}
extern "C" uint32_t ps3_hle_count(void) {std::abort();}
extern "C" void ps3_msf(const char*,...) {std::abort();}
extern "C" void ppu_prof_stamp(void*,unsigned) {std::abort();}
extern "C" uint32_t ps3_mmapper_block_size(uint32_t) {std::abort();}
extern "C" int lv2_try_syscall(ppu_context*) {std::abort();}
extern "C" uint64_t ppu_thread_register_main_priority(int32_t) {std::abort();}
extern "C" { void (*g_ppu_thread_entry_trampoline)(ppu_context*)=nullptr; }
static unsigned char arena[0x20000];
static void publish(uint32_t ea,uint32_t value){spu_lockline_lock();uint32_t raw=__builtin_bswap32(value);memcpy(vm_base+ea,&raw,4);spu_coh_notify_write(ea);spu_lockline_unlock();}
int main(){vm_base=arena;ppu_vm_size=sizeof(arena);ppu_context c{};g_active_ctx=&c;c.gpr[4]=0x10008;c.gpr[5]=21;vm_write64(0x10008,20);
spu_lockline_lock();spu_coh_reserve(nullptr,0x800);spu_lockline_unlock();
func_00001000(&c);publish(0x10000,27);func_00002000(&c);
assert(!(c.cr&0x20000000u) && "same-line SPU write must invalidate PPU tail reservation");assert(vm_read64(0x10008)==20);
func_00001000(&c);func_00002000(&c);assert(c.cr&0x20000000u);assert(vm_read64(0x10008)==21);
func_00001000(&c);publish(0x10080,99);func_00002000(&c);assert(c.cr&0x20000000u);
func_00001000(&c);publish(0x10004,7);publish(0x10004,0);func_00002000(&c);assert(!(c.cr&0x20000000u) && "same-line ABA must lose reservation");
c.gpr[4]=0x10008;func_00003000(&c);publish(0x1007c,7);func_00004000(&c);assert(!(c.cr&0x20000000u));
// Every SC consumes its reservation, including mismatches and guest address zero.
c.gpr[4]=0x10108;c.gpr[5]=31;vm_write64(0x10108,30);func_00001000(&c);
c.gpr[4]=0x10110;func_00002000(&c);assert(!(c.cr&0x20000000u));
c.gpr[4]=0x10108;func_00002000(&c);assert(!(c.cr&0x20000000u) && "mismatched SC must consume reservation");
c.gpr[4]=0;c.gpr[5]=1;vm_write64(0,0);func_00001000(&c);func_00002000(&c);assert(c.cr&0x20000000u);
memset(vm_base,0,8);c.gpr[5]=2;func_00002000(&c);assert(!(c.cr&0x20000000u) && "second SC at address zero needs a new LL");
// A publication on a separate host worker must invalidate the producer.
c.gpr[4]=0x10208;c.gpr[5]=41;vm_write64(0x10208,40);func_00001000(&c);
std::thread worker([]{spu_lockline_lock();uint32_t raw=__builtin_bswap32(27u);memcpy(vm_base+0x10200,&raw,4);spu_coh_notify_write_except(0x10200,nullptr);spu_lockline_unlock();});worker.join();
func_00002000(&c);assert(!(c.cr&0x20000000u));
func_00001000(&c);assert(!ppu_store_conditional64(&c,0x10210,41));
assert(!ppu_store_conditional64(&c,0x10208,41));
func_00001000(&c);ppu_context scratch=c;assert(!ppu_store_conditional64(&scratch,0x10208,41));
assert(!ppu_store_conditional64(&c,0x10208,41));
// A virgin line and a legacy context must not manufacture a new token.
vm_write64(0x10308,50);c.reserve_addr=0x10308;c.reserve_value=50;
assert(!ppu_store_conditional64(&c,0x10308,51));
// Legacy entry points retain their existing ABI when an old LL follows a new pair.
c.reserve_addr=0x10208;c.reserve_value=40;assert(ppu_stdcx64(0x10208,40,41));
func_00001000(&c);assert(ppu_store_conditional64(&c,0x10208,42));
assert(!ppu_store_conditional64(&c,0x10208,43));
puts("PASS real lifted word/doubleword instructions: same-line publication, retry, unrelated line and ABA");}
'''
with tempfile.TemporaryDirectory(prefix='ppu-spu-lines-') as tmp:
 p=Path(tmp);l=L.PPULifter()
 for a,m in [(0x1000,'ldarx'),(0x2000,'stdcx.'),(0x3000,'lwarx'),(0x4000,'stwcx.')]:
  ops=[Instruction(a,0,m,('r3,0,r4' if 'arx' in m else 'r5,0,r4')),Instruction(a+4,0,'blr','')];l.lift_function(ops,a,a+8)
 (p/'ppu_recomp.h').write_text(l.emit_header());(p/'lifted.cpp').write_text(l.emit_source());(p/'driver.cpp').write_text(driver)
 common=['-O1','-ffunction-sections','-fdata-sections']
 # The Windows loader contains SEH; use the MSVC-compatible clang toolchain
 # available in CI rather than an unrelated MinGW c++ from PATH.
 cc=os.environ.get('CC') or shutil.which('clang') or 'cc'
 cxx=os.environ.get('CXX') or shutil.which('clang++') or 'c++'
 linker='-Wl,/OPT:REF' if sys.platform=='win32' else ('-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections')
 subprocess.run([cc,'-std=gnu17',*common,'-c',str(root/'runtime/spu/spu_coherency.c'),'-o',str(p/'coh.o')],check=True)
 subprocess.run([cc,*common,'-c',str(root/'runtime/platform/win32_compat.c'),'-o',str(p/'compat.o')],check=True)
 subprocess.run([cxx,'-std=c++17','-UNDEBUG',*common,'-I'+str(p),'-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-I'+str(root/'runtime/platform'),str(root/'runtime/ppu/ppu_loader.cpp'),str(p/'driver.cpp'),str(p/'lifted.cpp'),str(p/'coh.o'),str(p/'compat.o'),'-pthread',linker,'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=10)
