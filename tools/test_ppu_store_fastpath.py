#!/usr/bin/env python3
"""Stress the PPU store coherence paths with and without PS3RECOMP_PPU_STORE_FASTPATH.

Builds the real lifted ldarx/stdcx. helpers, ppu_loader.cpp's guest stores and
spu_coherency.c, then runs the same driver with the switch off and on:
  - deterministic reservation outcomes (same-line store, other thread's
    reservation, ABA, the epoch left alone only when the switch is on);
  - a PPU stress: one thread load-reserves a doubleword, reads a second word of
    the line and store-conditionals (that word, sequence); another thread
    plain-stores increasing values to the second word. Any commit seen after a
    store must carry a value at least as new as that store, or a store landed
    inside a reservation without breaking it;
  - an SPU stress: an emulated GETLLAR/PUTLLC loop (the protocol in
    spu_channels.c) increments one word of a line that starts PPU-only while a
    PPU thread plain-stores increasing values to another word. A PUTLLC that
    commits over a store reverts it: the check is the final value and that no
    snapshot ever goes backwards.
PPU_RESV_OFF=1 (plain value CAS) must make the PPU stress report violations,
which shows the check can see the failure it guards against.
"""
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
static unsigned char arena[0x40000];
static bool eq(const ppu_context& c){return (c.cr&0x20000000u)!=0;}
static uint64_t ll(ppu_context& c,uint32_t ea){c.gpr[4]=ea;func_00001000(&c);return c.gpr[3];}
static bool sc(ppu_context& c,uint32_t ea,uint64_t v){c.gpr[4]=ea;c.gpr[5]=v;func_00002000(&c);return eq(c);}
static uint64_t epoch(uint32_t ea){spu_lockline_lock();uint64_t e=spu_coh_line_epoch(ea);spu_lockline_unlock();return e;}

/* Emulated SPU atomics, the protocol of spu_channels.c's GETLLAR/PUTLLC. */
static void getllar(spu_context* c,uint32_t ea,uint8_t* ls){
  const int fast=spu_coh_reserve_fast(c,ea);
  if(!fast){spu_lockline_lock();spu_coh_reserve(c,ea);}
  memcpy(ls,vm_base+ea,128);memcpy(c->resv_line,ls,128);
  if(!fast){c->resv_ea=ea;c->resv_valid=1;spu_lockline_unlock();}
}
static bool putllc(spu_context* c,uint32_t ea,const uint8_t* ls){
  if(!__atomic_load_n(&c->resv_valid,__ATOMIC_ACQUIRE)||c->resv_ea!=ea||memcmp(vm_base+ea,c->resv_line,128)){
    __atomic_store_n(&c->resv_valid,0,__ATOMIC_RELAXED);return false;}
  spu_lockline_lock();bool ok=false;
  if(c->resv_valid&&c->resv_ea==ea&&!memcmp(vm_base+ea,c->resv_line,128)){
    memcpy(vm_base+ea,ls,128);c->resv_valid=0;spu_coh_notify_write(ea);ok=true;}
  else c->resv_valid=0;
  spu_lockline_unlock();return ok;
}
static uint32_t be32(const uint8_t* p){uint32_t v;memcpy(&v,p,4);return __builtin_bswap32(v);}
static void put_be32(uint8_t* p,uint32_t v){v=__builtin_bswap32(v);memcpy(p,&v,4);}
static spu_context g_spu;
/* Registered PPU contexts must outlive the process: the registry keeps them. */
static ppu_context g_c,g_o,g_x,g_y;
extern "C" void ppu_resv_register(ppu_context*);

int main(int argc,char** argv){
  const bool fast=getenv("PS3RECOMP_PPU_STORE_FASTPATH")&&!strcmp(getenv("PS3RECOMP_PPU_STORE_FASTPATH"),"1");
  const bool resv_off=getenv("PPU_RESV_OFF")!=nullptr;
  const int rounds=argc>1?atoi(argv[1]):200000;
  vm_base=arena;ppu_vm_size=sizeof(arena);
  ppu_context& c=g_c;g_active_ctx=&c;
  for(ppu_context* r:{&g_c,&g_o,&g_x,&g_y})ppu_resv_register(r);
  if(!resv_off){
  // A plain store into a reserved line breaks the reservation.
  vm_write64(0x10008,20);ll(c,0x10008);vm_write32(0x10040,1);assert(!sc(c,0x10008,21));assert(vm_read64(0x10008)==20);
  // Same-line ABA still loses the reservation.
  ll(c,0x10008);vm_write32(0x10040,2);vm_write32(0x10040,1);assert(!sc(c,0x10008,21));
  // With no reservation live a store leaves the epoch alone only when the switch is on.
  ll(c,0x10008);assert(sc(c,0x10008,21));
  { uint64_t e0=epoch(0x10000);vm_write32(0x10044,5);uint64_t e1=epoch(0x10000);
    assert(e1==(fast?e0:e0+1)&&"fast path skips only reservation-free lines");
    ll(c,0x10008);assert(sc(c,0x10008,22));assert(vm_read64(0x10008)==22); }
  // Another thread's live reservation is broken by this thread's store; an
  // abandoned load-reserve keeps the slow path until that thread replaces it.
  { std::atomic<int> step{0};
    std::thread t([&]{g_active_ctx=&g_o;ll(g_o,0x10008);step=1;while(step!=2){}
      step=sc(g_o,0x10008,99)?3:4;while(step!=5){}
      ll(g_o,0x10008);step=6;while(step!=7){}
      ll(g_o,0x10108);step=8;});
    while(step!=1){} uint64_t e0=epoch(0x10000);vm_write32(0x1004c,7);assert(epoch(0x10000)==e0+1);
    step=2;while(step<3){} assert(step==4&&"store must break another thread's reservation");
    assert(vm_read64(0x10008)==22);
    step=5;while(step!=6){}
    uint64_t e1=epoch(0x10000);vm_write32(0x1004c,8);vm_write32(0x1004c,9);
    assert(epoch(0x10000)==e1+2&&"an abandoned load-reserve keeps the slow path");
    step=7;while(step!=8){}
    uint64_t e2=epoch(0x10000);vm_write32(0x1004c,10);
    assert(epoch(0x10000)==(fast?e2:e2+1));
    t.join(); }
  }
  // PPU stress: commits after a store must carry a value at least that new.
  const uint32_t W0=0x20000,W1=0x20040;   // one line
  vm_write64(W0,0);vm_write64(W1,0);ll(c,W0);sc(c,W0,0);   // mark the line before racing
  std::atomic<bool> stop{false};std::atomic<unsigned long> commits{0};
  std::thread s([&]{ppu_context& x=g_x;g_active_ctx=&x;uint32_t seq=0;
    while(!stop.load(std::memory_order_relaxed)){
      ll(x,W0);uint64_t a=vm_read64(W1);
      for(volatile int i=0;i<(int)(seq&63);i=i+1){}
      if(sc(x,W0,(a<<32)|(uint32_t)(seq+1))){++seq;commits.fetch_add(1,std::memory_order_relaxed);}
    }});
  unsigned long violations=0;
  { g_active_ctx=&g_y;
    for(uint64_t k=1;k<=(uint64_t)rounds;++k){
      vm_write64(W1,k);uint64_t r1=vm_read64(W0);
      for(volatile int i=0;i<(int)(k&127);i=i+1){}
      uint64_t r2=vm_read64(W0);
      if((uint32_t)r2!=(uint32_t)r1&&(r2>>32)<k)++violations;
    }
    g_active_ctx=&c; }
  stop=true;s.join();
  printf("ppu stress fast=%d resv_off=%d: %d stores, %lu commits, %lu violations\n",fast,resv_off,rounds,commits.load(),violations);
  if(resv_off){ return violations?0:3; }
  assert(commits>0);assert(violations==0);
  // SPU stress: a whole-line PUTLLC must never revert a PPU store.
  const uint32_t L2=0x30000;
  ll(c,L2);sc(c,L2,0);                                   // PPU-only to start
  std::atomic<bool> pdone{false};unsigned long spu_commits=0,back=0;uint32_t final_seen=0;
  std::thread u([&]{uint8_t ls[128];uint32_t last=0;unsigned long extra=0;
    while(!pdone.load()||extra<20000){
      if(pdone.load())++extra;
      getllar(&g_spu,L2,ls);uint32_t seen=be32(ls+8);if(seen<last)++back;last=seen;
      put_be32(ls,be32(ls)+1);
      for(volatile int i=0;i<(int)(spu_commits&31);i=i+1){}
      if(putllc(&g_spu,L2,ls))++spu_commits;
    }
    final_seen=last;});
  for(uint32_t k=1;k<=(uint32_t)rounds;++k)vm_write32(L2+8,k);
  pdone=true;u.join();
  printf("spu stress fast=%d: %d stores, %lu PUTLLC commits, final=%u, backwards=%lu\n",fast,rounds,spu_commits,vm_read32(L2+8),back);
  assert(spu_commits>0);assert(back==0);assert(vm_read32(L2+8)==(uint32_t)rounds);assert(vm_read32(L2)==spu_commits);
  spu_coh_unregister(&g_spu);
  puts("PASS PPU store coherence under load");
}
'''
with tempfile.TemporaryDirectory(prefix='ppu-store-fastpath-') as tmp:
    p = Path(tmp); l = L.PPULifter()
    for a, m in [(0x1000, 'ldarx'), (0x2000, 'stdcx.')]:
        ops = [Instruction(a, 0, m, ('r3,0,r4' if 'arx' in m else 'r5,0,r4')), Instruction(a + 4, 0, 'blr', '')]
        l.lift_function(ops, a, a + 8)
    (p/'ppu_recomp.h').write_text(l.emit_header()); (p/'lifted.cpp').write_text(l.emit_source()); (p/'driver.cpp').write_text(driver)
    common = ['-O2', '-ffunction-sections', '-fdata-sections']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-I'+str(root/'include'), '-c', str(root/'runtime/spu/spu_coherency.c'), '-o', str(p/'coh.o')], check=True)
    subprocess.run([os.environ.get('CC', 'cc'), *common, '-c', str(root/'runtime/platform/win32_compat.c'), '-o', str(p/'compat.o')], check=True)
    extra = os.environ.get('PPU_TEST_EXTRA_DEFINES', '').split()
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-UNDEBUG', *extra, '-I'+str(root/'runtime/ppu'), *common, '-I'+str(p), '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'), '-I'+str(root/'runtime/platform'), str(root/'runtime/ppu/ppu_loader.cpp'), str(p/'driver.cpp'), str(p/'lifted.cpp'), str(p/'coh.o'), str(p/'compat.o'), '-pthread', ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'), '-o', str(p/'test')], check=True)
    rounds = os.environ.get('PPU_STORE_FASTPATH_ROUNDS', '200000')
    for mode in ('0', '1'):
        env = dict(os.environ); env.pop('PPU_RESV_OFF', None)
        if mode == '1': env['PS3RECOMP_PPU_STORE_FASTPATH'] = '1'
        else: env.pop('PS3RECOMP_PPU_STORE_FASTPATH', None)
        subprocess.run([str(p/'test'), rounds], check=True, timeout=120, env=env)
    env = dict(os.environ); env['PPU_RESV_OFF'] = '1'; env['PS3RECOMP_PPU_STORE_FASTPATH'] = '1'
    r = subprocess.run([str(p/'test'), rounds], timeout=120, env=env)
    assert r.returncode == 0, 'the PPU stress must detect a store-conditional that ignores plain stores'
    print('PASS PPU store fast path: reservations, PPU stress and SPU stress, switch off and on')
