#!/usr/bin/env python3
"""Exercise real mapper/indirect-entry/DMA diagnostic paths using synthetic data."""
import os, subprocess, tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: standalone provenance fixtures require the POSIX toolchain')
    raise SystemExit(0)
loader = (root / 'runtime/ppu/ppu_loader.cpp').read_text()
start = loader.index('    case 330: {')
end = loader.index('    case 988:', start)
mapper = loader[start:end]
start = loader.index('static void ps3_indirect_call_impl(ppu_context* ctx)\n{')
start = loader.index('{', start) + 1
end = loader.index('    /* BCTRL_RING', start)
entry = loader[start:end]
header = root / 'runtime/output_provenance.h'
helper = ''
if 'static void ppu_output_provenance' in loader:
    a=loader.index('static void ppu_output_provenance');b=loader.index('static void ps3_indirect_call_impl(ppu_context* ctx)\n{',a);helper=loader[a:b]
include = '#include "output_provenance.h"' if header.exists() else ''
ppu = r"""
#include "ppu_context.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
// HEADER
static thread_local ppu_context* g_active_ctx;
static uint32_t written;
static void vm_write32(uint64_t a,uint32_t v){assert(a==0x100);written=v;}
static void vm_commit(uint32_t,uint32_t){}
extern "C" uint32_t ps3_mmapper_block_size(uint32_t){return 0x100000;}
static void mapper(ppu_context* ctx){switch(ctx->gpr[11]){// MAPPER
}}
static void entry(ppu_context* ctx){// ENTRY
}
int main(int argc,char**argv){ppu_context c{};auto before=c;
if(!strcmp(argv[1],"mapper")){
c.gpr[11]=330;c.gpr[3]=0xF000000;c.gpr[5]=0x100000;c.gpr[6]=0x100;c.lr=0x12345678;mapper(&c);assert(!c.gpr[3]&&written==0x11000000);
c.gpr[3]=0x100000;mapper(&c);assert(written==0x20000000&&!c.gpr[3]);
c.gpr[11]=337;c.gpr[3]=0x20000000;c.gpr[4]=0x1000;mapper(&c);assert(written==0x20000000&&!c.gpr[3]);
}else{c.ctr=0xABCD;c.gpr[6]=0x10080;c.gpr[8]=0x4000;c.gpr[4]=0x345000;c.gpr[5]=0x234;c.lr=0x5678;c.thread_id=7;before=c;
c.ctr=0xABCE;entry(&c);c.ctr=0xABCD;c.gpr[6]=0x11000;entry(&c);c.gpr[6]=0x10080;
c.gpr[8]=0;entry(&c);c.gpr[8]=0xFFFFFFFFFFFFFFFFull;entry(&c);c.gpr[8]=0x4000;
if(!strcmp(argv[1],"concurrent")){std::thread workers[8];for(auto& w:workers)w=std::thread([c]()mutable{for(int i=0;i<100;i++)entry(&c);});for(auto& w:workers)w.join();}
else for(int i=0;i<100;i++)entry(&c);assert(!memcmp(&before,&c,sizeof c));
}puts("PASS unchanged mapper returns and PPU context");}
""".replace('// HEADER',include+'\n'+helper).replace('// MAPPER',mapper).replace('// ENTRY',entry)
spu = r"""
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static unsigned char memory[1u<<20];
unsigned char* vm_base=memory;
unsigned int ppu_vm_size=sizeof memory;
#include "spu_channels.c"
int main(void){spu_context* c=calloc(1,sizeof *c);assert(c);spu_context_init(c,9);c->image_id=100;c->spu_id=9;c->pc=0x408C;c->gpr[0]._u32[0]=0x3B4C;
memset(c->ls+0x22800,0xA6,0x4000);
assert(mfc_do_transfer(c,0x22800,0x14080,0x4000,0x20)==0);
assert(mfc_do_transfer(c,0x22801,0x10080,0x4000,0x20)==0);assert(memory[0x10080]==0);
for(int i=0;i<100;i++){assert(mfc_do_transfer(c,0x22800,0x10080,0x4000,0x20)==0);assert(memory[0x10080]==0xA6&&memory[0x1407F]==0xA6);}
assert(mfc_do_transfer(c,0x22800,0x14080,0x4000,0x20)==0);puts("PASS actual DMA copy unchanged");return 0;}
"""
spu += 'void ppu_resv_break_store'+(root/'runtime/spu/tests/test_spu_lockline_peer.c').read_text().split('void ppu_resv_break_store',1)[1]
common=['spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c']
with tempfile.TemporaryDirectory(prefix='output-provenance-') as td:
    td=Path(td);(td/'ppu.cpp').write_text(ppu);(td/'spu.c').write_text(spu)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-c',str(root/'runtime/platform/win32_compat.c'),'-o',str(td/'compat.o')],check=True)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG','-I'+str(root/'runtime/ppu'),'-I'+str(root/'runtime'),str(td/'ppu.cpp'),str(td/'compat.o'),'-pthread','-o',str(td/'ppu')],check=True)
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),str(td/'spu.c'),*[str(root/'runtime/spu'/n) for n in common],str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(td/'spu')],check=True)
    clean={k:v for k,v in os.environ.items() if not k.startswith(('PS3_OUTPUT_', 'SPU_', 'SPURS_', 'LBP_', 'YDKJ_'))}
    def run(binary,args,options):
        return subprocess.run([str(td/binary),*args],env=dict(clean,**options),capture_output=True,text=True,check=True,timeout=15)
    off=run('ppu',['entry'],{});assert '[output-provenance]' not in off.stderr
    invalid=run('ppu',['entry'],{'PS3_OUTPUT_PROVENANCE':'10000-10000','PS3_OUTPUT_PROVENANCE_TARGET':'ABCD'});assert '[output-provenance]' not in invalid.stderr
    m=run('ppu',['mapper'],{'PS3_OUTPUT_PROVENANCE':'20000000-20008000'})
    assert 'mapper=330' in m.stderr and 'mapper=337' in m.stderr, 'missing inline mapper overlap receipts: '+m.stderr
    assert 'va=0x11000000' not in m.stderr
    assert 'va=0x20000000' in m.stderr and 'lr=0x12345678' in m.stderr
    p=run('ppu',['entry'],{'PS3_OUTPUT_PROVENANCE':'10000-11000','PS3_OUTPUT_PROVENANCE_TARGET':'ABCD'})
    lines=[l for l in p.stderr.splitlines() if '[output-provenance]' in l]
    assert len(lines)==64, 'expected exactly64 bounded PPU records: '+str(len(lines))
    assert all('target=0x0000ABCD' in l and 'out=0x00010080' in l and 'size=16384' in l and 'lr=0x00005678' in l for l in lines)
    concurrent=run('ppu',['concurrent'],{'PS3_OUTPUT_PROVENANCE':'10000-11000','PS3_OUTPUT_PROVENANCE_TARGET':'ABCD'})
    assert sum('[output-provenance]' in line for line in concurrent.stderr.splitlines())==64, concurrent.stderr
    s=run('spu',[],{'PS3_OUTPUT_PROVENANCE':'10000-11000'})
    lines=[l for l in s.stderr.splitlines() if '[output-provenance]' in l]
    assert len(lines)==64, 'expected exactly64 bounded DMA records: '+str(len(lines))
    assert all('r0=0x00003B4C' in l and 'img=100' in l and 'spu=9' in l and 'ea=0x00010080' in l for l in lines)
    off=run('spu',[],{});assert '[output-provenance]' not in off.stderr
    print('PASS mapper overlap receipts, disabled/invalid modes, target/range filters,64-record caps, real unchanged DMA')
