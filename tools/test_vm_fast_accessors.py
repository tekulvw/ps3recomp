#!/usr/bin/env python3
"""PS3RECOMP_VM_FAST_ACCESSORS: the out-of-line guest accessors with and without the fast path.

Builds ppu_loader.cpp's real vm_read*/vm_write* and spu_coherency.c twice (option off and on)
and runs one driver against both:
  - big-endian values for every width, aligned and unaligned, must match a byte-level reference;
  - side-effect addresses keep their slow path: the raw SPU problem-state window (load and store
    hooks), the GCM ref-poll word, the GCM PUT wake word, the null page (store dropped);
  - a store to a line an SPU has reserved still raises SPU_EVENT_LR and ends the reservation,
    and a store to an unrelated line does not;
  - read and store diagnostics in the environment turn the fast path off (PPU_FORCE_READ_ADDR
    still forces its value, PPU_WWATCH is accepted);
and prints the same transcript in both builds. The reservation stress tests run the fast
accessors through tools/test_ppu_store_fastpath.py (PPU_TEST_EXTRA_DEFINES).
"""
import os
if os.name == 'nt':
    print('SKIP: POSIX fixture: compiles ppu_loader.cpp with cc/c++, whose Windows path needs MSVC __try/__except')
    raise SystemExit(0)
import os, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ppu_lifter as L
from ppu_disasm import Instruction
root = Path(__file__).resolve().parents[1]
driver = r'''
#include "ppu_recomp.h"
#include "spu_coherency.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" { uint8_t* vm_base=nullptr; extern uint32_t ppu_vm_size; uint32_t ppu_hle_inject_base=0x03000000;
 uint32_t g_gcm_consumer_put_wake_ea=0; extern int vm_inline_ok; }
static int raw_loads, raw_stores, ref_polls, kicks, wakes;
extern "C" void spu_ch_wake(spu_context*) { ++wakes; }
extern "C" int spu_raw_reg_load(uint32_t, uint32_t* out) { ++raw_loads; *out = 0x5EED5EEDu; return 1; }
extern "C" void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { ++raw_stores; }
extern "C" void cellGcm_ref_on_poll(void) { ++ref_polls; }
extern "C" void cellGcm_fifo_kick_notify(void) { ++kicks; }
static uint64_t be(const uint8_t* p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
static spu_context g_spu;
int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "values";
  vm_base = (uint8_t*)calloc(1, 0xE0000000u + 0x80000u); ppu_vm_size = 0;   // whole window, like the runtime
  if (!vm_base) { puts("SKIP no memory"); return 0; }
  if (!strcmp(mode, "values")) {
    for (uint32_t off = 0; off < 9; ++off) {
      const uint32_t a = 0x00100000u + off;
      vm_write8(a, 0xA1); assert(vm_base[a] == 0xA1 && vm_read8(a) == 0xA1);
      vm_write16(a, 0xB2C3); assert(be(vm_base + a, 2) == 0xB2C3 && vm_read16(a) == 0xB2C3);
      vm_write32(a, 0xD4E5F607u); assert(be(vm_base + a, 4) == 0xD4E5F607u && vm_read32(a) == 0xD4E5F607u);
      vm_write64(a, 0x0123456789ABCDEFull); assert(be(vm_base + a, 8) == 0x0123456789ABCDEFull && vm_read64(a) == 0x0123456789ABCDEFull);
    }
    puts("values: big-endian loads and stores, every width, offsets 0-8");
    // Raw SPU problem state: loads and stores keep their hooks; local store stays plain memory.
    const uint32_t reg = 0xE0000000u + 0x40000u + 0x4004u, ls = 0xE0000000u + 0x100u;
    assert(vm_read32(reg) == 0x5EED5EEDu && raw_loads == 1);
    vm_write32(reg, 7); assert(raw_stores == 1);
    vm_write32(ls, 0x11223344u); assert(vm_read32(ls) == 0x11223344u && raw_loads == 1 && raw_stores == 1);
    // GCM ref poll and PUT wake word.
    (void)vm_read32(0x03002008u); assert(ref_polls == 1);
    g_gcm_consumer_put_wake_ea = 0x00200040u; vm_write32(0x00200040u, 1); assert(kicks == 1);
    vm_write32(0x00200044u, 1); assert(kicks == 1); g_gcm_consumer_put_wake_ea = 0;
    // Null page: the store is dropped (PS3_NULL_WRITE default), the read reports and returns memory.
    vm_write32(0x100u, 0xFFFFFFFFu); assert(be(vm_base + 0x100, 4) == 0);
    puts("special: raw SPU registers, GCM ref poll, PUT wake, null page keep their slow paths");
    // An SPU reservation on a line: a PPU store to it raises LR; one to the next line does not.
    // The SPU is parked on its events and looks at them after each store, as the guest does:
    // per-line version counters (PS3RECOMP_COH_VERSIONS=1) deliver the loss then, and wake
    // only a parked SPU; the default scheme raised it at the store.
    const uint32_t line = 0x00300000u;
    const spu_coh_ops* v = g_spu_coh_ops;
    static uint8_t snapshot[128]; void* parked[SPU_COH_WAITER_BYTES / sizeof(void*)];
    auto reserve = [&] {
      if (v) { v->getllar(&g_spu, line, vm_base + line, snapshot); v->wait_begin(&g_spu, parked, [](void*) { ++wakes; }); return; }
      spu_lockline_lock(); spu_coh_reserve(&g_spu, line); g_spu.resv_ea = line; g_spu.resv_valid = 1; spu_lockline_unlock(); };
    auto look = [&] { if (v) v->settle(&g_spu); };
    auto unpark = [&] { if (v) v->wait_end(parked); };
    reserve();
    vm_write32(line + 0x80, 1); look(); assert(g_spu.resv_valid == 1 && !(g_spu.event_status & SPU_EVENT_LR) && wakes == 0);
    vm_write64(line + 0x78, 2); look(); unpark(); assert(g_spu.resv_valid == 0 && (g_spu.event_status & SPU_EVENT_LR) && wakes == 1);
    g_spu.event_status = 0; reserve();
    vm_write8(line + 5, 3); look(); unpark(); assert(g_spu.resv_valid == 0 && (g_spu.event_status & SPU_EVENT_LR) && wakes == 2);
    spu_coh_unregister(&g_spu);
    puts("coherency: stores to a reserved line raise LR, a store to the next line does not");
  } else if (!strcmp(mode, "force-read")) {
    vm_write32(0x00100100u, 1); assert(vm_read32(0x00100100u) == 0xCAFEu);
    puts("force-read: PPU_FORCE_READ_ADDR still forces its value");
  } else if (!strcmp(mode, "wwatch")) {
    vm_write32(0x00100200u, 9); assert(vm_read32(0x00100200u) == 9);
    puts("wwatch: stores still land with PPU_WWATCH armed");
  }
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='vm-fast-') as tmp:
    p = Path(tmp); l = L.PPULifter()
    l.lift_function([Instruction(0x1000, 0, 'blr', '')], 0x1000, 0x1004)
    (p/'ppu_recomp.h').write_text(l.emit_header()); (p/'lifted.cpp').write_text(l.emit_source()); (p/'driver.cpp').write_text(driver)
    common = ['-O2', '-ffunction-sections', '-fdata-sections']
    inc = ['-I'+str(root/'runtime/ppu'), '-I'+str(p), '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'), '-I'+str(root/'runtime/platform')]
    transcripts = {}
    for variant, defines in (('off', []), ('on', ['-DPS3RECOMP_VM_FAST_ACCESSORS=1'])):
        b = p / variant; b.mkdir()
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-I'+str(root/'include'), '-c', str(root/'runtime/spu/spu_coherency.c'), '-o', str(b/'coh.o')], check=True)
        subprocess.run([os.environ.get('CC', 'cc'), *common, '-c', str(root/'runtime/platform/win32_compat.c'), '-o', str(b/'compat.o')], check=True)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-UNDEBUG', *defines, *common, *inc, str(root/'runtime/ppu/ppu_loader.cpp'),
                        str(p/'driver.cpp'), str(p/'lifted.cpp'), str(b/'coh.o'), str(b/'compat.o'), '-pthread',
                        ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'), '-o', str(b/'test')], check=True)
        out = []
        for mode, env in (('values', {}), ('force-read', {'PPU_FORCE_READ_ADDR': '00100100', 'PPU_FORCE_READ_VAL': 'CAFE'}),
                          ('wwatch', {'PPU_WWATCH': '00100200'})):
            e = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'SPU_', 'YDKJ_'))}; e.update(env)
            r = subprocess.run([str(b/'test'), mode], capture_output=True, text=True, timeout=60, env=e)
            if r.returncode: print(r.stdout, r.stderr[-3000:]); raise SystemExit(f'{variant}:{mode} failed')
            banner = [x for x in r.stderr.splitlines() if x.startswith('[vm] fast accessors')]
            if variant == 'on':
                want = {'values': 'reads on, stores on', 'force-read': 'reads off', 'wwatch': 'stores off'}[mode]
                assert banner and want in banner[0], (mode, banner)
            else:
                assert not banner, banner
            out.append(r.stdout.strip())
            print(variant, r.stdout.strip().replace('\n', ' | '), '|', banner[0] if banner else '(no fast path)')
        transcripts[variant] = out
    assert transcripts['off'] == transcripts['on'], 'fast accessors changed observable results'
    env = dict(os.environ); env['PPU_TEST_EXTRA_DEFINES'] = '-DPS3RECOMP_VM_FAST_ACCESSORS=1'
    subprocess.run([sys.executable, str(root/'tools/test_ppu_store_fastpath.py')], check=True, env=env)
    print('PASS fast guest accessors: values, side-effect addresses, coherency, diagnostics, reservation stress')
