#!/usr/bin/env python3
"""PS3RECOMP_PPU_STORE_LIVE_SPU: a locked write skips the holder walk while no SPU counts the line.

Builds the real lifted ldarx/stdcx. helpers, ppu_loader.cpp's guest stores and
spu_coherency.c, and an emulated GETLLAR/PUTLLC (the protocol in spu_channels.c,
including its live-tracking calls), then runs the same driver with the switch
off and on:
  - deterministic: a write to a line an SPU reserved once skips the holder walk
    once that SPU's reservation has ended (the lock-line lock is still taken), a
    counted holder still loses its reservation and gets SPU_EVENT_LR, the count
    moves with the next GETLLAR, a context-less interest is never skipped, and a
    PUTLLC whose line a PPU store or store-conditional touched after its GETLLAR
    fails, including a store that writes the same bytes back (ABA);
  - stress: an SPU thread runs GETLLAR/PUTLLC increments of one word of a line,
    pausing between bursts so the line keeps going idle and live again, while one
    PPU thread plain-stores increasing values to a second word and another
    load-reserve/store-conditional-increments a third. Every update must survive:
    the final words equal the SPU commit count, the last store and the PPU commit
    count, and no snapshot ever goes backwards.
Final memory is the same with the switch off and on. PS3RECOMP_SANITIZE=thread
(or address) builds the fixture with that sanitizer.
"""
import os
if os.name == 'nt':
    print('SKIP: POSIX fixture: compiles ppu_loader.cpp with cc/c++, whose Windows path needs MSVC __try/__except')
    raise SystemExit(0)
import os, sys, subprocess, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ppu_lifter as L
from ppu_disasm import Instruction
root = Path(__file__).resolve().parents[1]
driver = r'''
#include "ppu_recomp.h"
#include "spu_coherency.h"
#include <atomic>
#include <initializer_list>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
extern "C" { uint8_t* vm_base=nullptr; extern PPU_THREAD_LOCAL ppu_context* g_active_ctx; extern uint32_t ppu_vm_size; uint32_t ppu_hle_inject_base=0; }
extern "C" void spu_ch_wake(spu_context*) {}
extern "C" int spu_raw_reg_load(uint32_t,uint32_t*) {std::abort();}
extern "C" void spu_raw_reg_store(uint32_t,uint32_t,unsigned) {std::abort();}
extern "C" void cellGcm_ref_on_poll(void) {std::abort();}
extern "C" { uint32_t g_gcm_consumer_put_wake_ea=0; void cellGcm_fifo_kick_notify(void) {std::abort();} }
extern "C" void ppu_resv_register(ppu_context*);
extern "C" unsigned long g_spu_coh_live_skips;
extern "C" unsigned long g_spu_coh_shard_idle_stores;
static unsigned char arena[0x40000];
static bool eq(const ppu_context& c){return (c.cr&0x20000000u)!=0;}
static uint64_t ll(ppu_context& c,uint32_t ea){c.gpr[4]=ea;func_00001000(&c);return c.gpr[3];}
static bool sc(ppu_context& c,uint32_t ea,uint64_t v){c.gpr[4]=ea;c.gpr[5]=v;func_00002000(&c);return eq(c);}
/* Emulated SPU atomics: spu_channels.c's GETLLAR/PUTLLC, live tracking included. */
static void getllar(spu_context* c,uint32_t ea,uint8_t* ls){
  const int fast=spu_coh_reserve_fast(c,ea);
  if(!fast){spu_lockline_lock();spu_coh_reserve(c,ea);}
  spu_coh_live_track(c,ea);
  memcpy(ls,vm_base+ea,128);memcpy(c->resv_line,ls,128);
  if(!fast){c->resv_ea=ea;c->resv_valid=1;spu_lockline_unlock();}
}
static bool putllc(spu_context* c,uint32_t ea,const uint8_t* ls){
  if(!__atomic_load_n(&c->resv_valid,__ATOMIC_ACQUIRE)||c->resv_ea!=ea||memcmp(vm_base+ea,c->resv_line,128)){
    __atomic_store_n(&c->resv_valid,0,__ATOMIC_RELAXED);spu_coh_live_untrack(c);return false;}
  spu_lockline_lock();bool ok=false;
  if(c->resv_valid&&c->resv_ea==ea&&!memcmp(vm_base+ea,c->resv_line,128)){
    memcpy(vm_base+ea,ls,128);c->resv_valid=0;spu_coh_notify_write(ea);ok=true;}
  else c->resv_valid=0;
  spu_coh_live_untrack(c);
  spu_lockline_unlock();return ok;
}
static uint32_t be32(const uint8_t* p){uint32_t v;memcpy(&v,p,4);return __builtin_bswap32(v);}
static void put_be32(uint8_t* p,uint32_t v){v=__builtin_bswap32(v);memcpy(p,&v,4);}
static spu_context g_spu,g_spu2;
static ppu_context g_c,g_x,g_y;
/* 1 when a plain store to ea would take the shard path. */
static int shard_path(uint32_t ea){int s=spu_coh_ppu_store_lock(ea);if(s)spu_coh_ppu_store_unlock(ea);return s;}

int main(int argc,char** argv){
  const bool live=spu_coh_live_spu_enabled()>0;
  const bool shard=spu_coh_shard_idle_enabled()>0;
  const int rounds=argc>1?atoi(argv[1]):200000;
  vm_base=arena;ppu_vm_size=sizeof(arena);
  g_active_ctx=&g_c;
  for(ppu_context* r:{&g_c,&g_x,&g_y})ppu_resv_register(r);
  uint8_t ls[128];
  // A line reserved once walks the holders on every write when off, only while counted when on.
  const uint32_t A=0x10000;
  getllar(&g_spu,A,ls);put_be32(ls,1);assert(putllc(&g_spu,A,ls));
  assert(shard_path(A)==(shard?1:0));                      // idle: the shard path only with SHARD_IDLE
  { unsigned long k0=g_spu_coh_live_skips,s0=g_spu_coh_shard_idle_stores;vm_write32(A+0x40,1);
    assert(g_spu_coh_live_skips-k0==(live&&!shard?1u:0u)&&g_spu_coh_shard_idle_stores-s0==(shard?1u:0u)); }
  // A counted holder is told, on and off.
  getllar(&g_spu2,A,ls);assert(!shard_path(A));             // counted: the lock-line lock
  { unsigned long k0=g_spu_coh_live_skips;__atomic_store_n(&g_spu2.event_status,0,__ATOMIC_RELAXED);
    vm_write32(A+0x44,1);assert(g_spu_coh_live_skips==k0);
    assert(!g_spu2.resv_valid&&(g_spu2.event_status&SPU_EVENT_LR)&&"holder must lose its reservation"); }
  getllar(&g_spu2,A+0x80,ls);                               // the count moves with the next GETLLAR
  { unsigned long k0=g_spu_coh_live_skips,s0=g_spu_coh_shard_idle_stores;vm_write32(A+0x48,1);
    assert(g_spu_coh_live_skips-k0==(live&&!shard?1u:0u)&&g_spu_coh_shard_idle_stores-s0==(shard?1u:0u)); }
  putllc(&g_spu2,A+0x80,ls);
  // A context-less interest (a watch, an HLE line) is never skipped.
  { const uint32_t W=0x11000;spu_lockline_lock();spu_coh_reserve(NULL,W);spu_lockline_unlock();
    unsigned long k0=g_spu_coh_live_skips;vm_write32(W,1);assert(g_spu_coh_live_skips==k0);assert(!shard_path(W)); }
  // A line host code read-modify-writes keeps the lock-line lock from then on.
  { const uint32_t H=0x12000;getllar(&g_spu,H,ls);putllc(&g_spu,H,ls);
    assert(shard_path(H)==(shard?1:0));
    spu_lockline_lock();spu_coh_host_rmw_mark(H+4,4);spu_lockline_unlock();
    assert(!shard_path(H)&&!shard_path(H+0x7C)&&"the whole line is marked");
  }
  // An overlapping PPU store fails the PUTLLC, the same bytes written back included.
  getllar(&g_spu,A,ls);vm_write32(A+0x40,7);put_be32(ls,2);assert(!putllc(&g_spu,A,ls));
  getllar(&g_spu,A,ls);{uint32_t v=vm_read32(A+0x40);vm_write32(A+0x40,v+1);vm_write32(A+0x40,v);}
  put_be32(ls,3);assert(!putllc(&g_spu,A,ls)&&"ABA store must still fail the PUTLLC");
  // ...and so does an overlapping PPU store-conditional.
  getllar(&g_spu,A,ls);ll(g_c,A+0x48);assert(sc(g_c,A+0x48,5));put_be32(ls,4);assert(!putllc(&g_spu,A,ls));
  // A store-conditional fails after an SPU commit to its line, idle line or not.
  ll(g_c,A+0x48);getllar(&g_spu2,A,ls);put_be32(ls+4,9);assert(putllc(&g_spu2,A,ls));assert(!sc(g_c,A+0x48,6));
  // Host read-modify-writes under the lock against PPU stores to the same line: none is lost.
  { const uint32_t C=0x14000;getllar(&g_spu,C,ls);putllc(&g_spu,C,ls);memset(vm_base+C,0,128);
    std::atomic<bool> hdone{false};
    std::thread h([&]{for(int k=0;k<rounds/4;k++){spu_lockline_lock();spu_coh_host_rmw_mark(C,8);
        uint32_t v=be32(vm_base+C);put_be32(vm_base+C,v+1);
        if(spu_coh_is_reserved(C))spu_coh_notify_write(C);spu_lockline_unlock();}hdone=true;});
    g_active_ctx=&g_y;uint32_t k=0;while(!hdone.load()){++k;vm_write32(C+4,k);vm_write32(C+0x40,k);}
    h.join();g_active_ctx=&g_c;
    assert(vm_read32(C)==(uint32_t)(rounds/4)&&"host increments survive");
    assert(vm_read32(C+4)==k&&vm_read32(C+0x40)==k&&"PPU stores survive"); }
  // Stress: SPU increments word 0 in bursts, one PPU thread stores word 2, another ll/sc-increments word 4.
  const uint32_t B=0x20000;
  getllar(&g_spu,B,ls);putllc(&g_spu,B,ls);                // SPU-marked, idle
  memset(vm_base+B,0,128);
  std::atomic<bool> pdone{false};std::atomic<unsigned long> sccommits{0};unsigned long spu_commits=0,back=0;
  std::thread u([&]{uint8_t l[128];uint32_t last=0;unsigned long extra=0;
    while(!pdone.load()||extra<20000){
      if(pdone.load())++extra;
      getllar(&g_spu,B,l);uint32_t seen=be32(l+8);if(seen<last)++back;last=seen;
      put_be32(l,be32(l)+1);
      if(putllc(&g_spu,B,l))++spu_commits;
      if((spu_commits&255)==0)for(volatile int i=0;i<2000;i=i+1){}   // go idle for a while
    }});
  std::thread w([&]{g_active_ctx=&g_x;while(!pdone.load()){
      uint64_t v=ll(g_x,B+16);if(sc(g_x,B+16,v+1))sccommits.fetch_add(1);}});
  g_active_ctx=&g_y;
  for(uint32_t k=1;k<=(uint32_t)rounds;++k)vm_write32(B+8,k);
  pdone=true;u.join();w.join();g_active_ctx=&g_c;
  printf("live=%d shard=%d: %d stores, %lu PUTLLC commits, %lu store-conditional commits, backwards=%lu, shard-path stores=%lu\n",
         live,shard,rounds,spu_commits,sccommits.load(),back,g_spu_coh_shard_idle_stores);
  assert(spu_commits>0&&sccommits>0);assert(back==0);
  assert(vm_read32(B+8)==(uint32_t)rounds);assert(vm_read32(B)==spu_commits);assert(vm_read64(B+16)==sccommits.load());
  spu_coh_unregister(&g_spu);spu_coh_unregister(&g_spu2);
  // Final memory, for comparison across the switch: the deterministic part.
  uint64_t h=1469598103934665603ull;for(int i=0;i<128;i++){h^=vm_base[A+i];h*=1099511628211ull;}
  printf("deterministic line A digest %016llx\n",(unsigned long long)h);
  puts("PASS live SPU reservation tracking");
}
'''
with tempfile.TemporaryDirectory(prefix='ppu-store-live-spu-') as tmp:
    p = Path(tmp); l = L.PPULifter()
    for a, m in [(0x1000, 'ldarx'), (0x2000, 'stdcx.')]:
        ops = [Instruction(a, 0, m, ('r3,0,r4' if 'arx' in m else 'r5,0,r4')), Instruction(a + 4, 0, 'blr', '')]
        l.lift_function(ops, a, a + 8)
    (p/'ppu_recomp.h').write_text(l.emit_header()); (p/'lifted.cpp').write_text(l.emit_source()); (p/'driver.cpp').write_text(driver)
    san = os.environ.get('PS3RECOMP_SANITIZE')
    common = ['-O2', '-g', '-ffunction-sections', '-fdata-sections'] + (['-fsanitize=' + san, '-fno-omit-frame-pointer'] if san else [])
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-I'+str(root/'include'), '-c', str(root/'runtime/spu/spu_coherency.c'), '-o', str(p/'coh.o')], check=True)
    subprocess.run([os.environ.get('CC', 'cc'), *common, '-c', str(root/'runtime/platform/win32_compat.c'), '-o', str(p/'compat.o')], check=True)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-UNDEBUG', *common, '-I'+str(root/'runtime/ppu'), '-I'+str(p), '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'), '-I'+str(root/'runtime/platform'), str(root/'runtime/ppu/ppu_loader.cpp'), str(p/'driver.cpp'), str(p/'lifted.cpp'), str(p/'coh.o'), str(p/'compat.o'), '-pthread', ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'), '-o', str(p/'test')], check=True)
    rounds = os.environ.get('PPU_STORE_LIVE_SPU_ROUNDS', '200000')
    digests = []
    for mode in ('0', '1', 'shard'):
        env = dict(os.environ)
        env.pop('PS3RECOMP_PPU_STORE_LIVE_SPU', None); env.pop('PS3RECOMP_PPU_STORE_SHARD_IDLE', None)
        if mode != '0': env['PS3RECOMP_PPU_STORE_LIVE_SPU'] = '1'
        if mode == 'shard': env['PS3RECOMP_PPU_STORE_SHARD_IDLE'] = '1'
        r = subprocess.run([str(p/'test'), rounds], timeout=600, env=env, capture_output=True, text=True)
        sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
        assert r.returncode == 0, f'live={mode} failed'
        digests.append([x for x in r.stdout.splitlines() if x.startswith('deterministic')])
    assert digests[0] == digests[1] == digests[2], 'final memory differs between the switches'
    print('PASS PPU stores with live SPU reservation tracking and the idle-line shard path, switches off and on')
