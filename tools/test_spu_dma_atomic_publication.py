#!/usr/bin/env python3
"""Real atomic helper observation; manufactured bytes and no guest assets."""
import os, subprocess, tempfile
from pathlib import Path
if os.name=='nt':
    print('SKIP: atomic helper fixture requires POSIX threads');raise SystemExit(0)
root=Path(__file__).resolve().parents[1];spu=root/'runtime/spu'
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','LBP_','GH3_','YDKJ_'))}
common=['spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c']
with tempfile.TemporaryDirectory(prefix='dma-atomic-publication-') as tmp:
    binary=Path(tmp)/'fixture'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-pthread','-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),str(spu/'tests/test_spu_dma_atomic_publication.c')]+[str(spu/c) for c in common]+[str(root/'runtime/platform/win32_compat.c'),'-lm','-o',str(binary)],check=True)
    def run(settings,mode=None):
        p=subprocess.run([str(binary)]+([mode] if mode else []),env=dict(env,**settings),capture_output=True,text=True,check=True,timeout=10)
        return [s for s in p.stderr.splitlines() if s.startswith('[dma-publication]')]
    settings={'SPU_DMA_PUBLICATION_MATCH_RANGE':'20000-20080','SPU_DMA_PUBLICATION_MATCH_WORD':'12345678','SPU_DMA_PUBLICATION_ATOMIC':'1','SPU_DMA_PUBLICATION_HEADER_WORDS':'8'}
    lines=run(settings)
    assert len(lines)==4,('expected two actual atomic reads and both PUT outcomes',lines)
    assert ['outcome='+x in lines[i] for i,x in enumerate(['getllar-complete','putllc-success','getllar-complete','putllc-failure'])]==[True]*4,lines
    assert all('atomic=1' in s and 'match_ea=00020004' in s and 'header_words=8' in s for s in lines),lines
    assert 'header=00700001,12345678' in lines[0] and 'header=01000001,12345678' in lines[1] and 'header=02000001,12345678' in lines[3],lines
    assert run({})==[]
    assert run({},"poison")==[]
    assert run(dict(settings,SPU_DMA_PUBLICATION_ATOMIC="0"),"poison")==[]
    assert run(settings,"bounds")==[]
    assert run(dict(settings,SPU_DMA_PUBLICATION_ATOMIC='0'))==[]
    for value in ('true','2','-1',''):
        assert run(dict(settings,SPU_DMA_PUBLICATION_ATOMIC=value))==[]
    partial=run(dict(settings,SPU_DMA_PUBLICATION_MATCH_RANGE='20004-20008'))
    assert len(partial)==4 and all('words=1' in s and 'payload=12345678' in s for s in partial),partial
    assert run(dict(settings,SPU_DMA_PUBLICATION_MATCH_RANGE='20005-20007'))==[]
    assert len(run(dict(settings,SPU_DMA_PUBLICATION_LIMIT='2')))==2
    assert len(run(dict(settings,SPU_DMA_PUBLICATION_LIMIT='3'),'threads'))==3
    assert all('lsa=3FF80' in s for s in run(settings,'end'))
    impl=Path(tmp)/'impl.c';impl.write_text('#include "spu_dma_publication_impl.h"\nvoid SetLastError(DWORD x){(void)x;}\n')
    obj=Path(tmp)/'impl.o'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-pthread','-I'+str(spu),'-c',str(impl),'-o',str(obj)],check=True)
    cpp=Path(tmp)/'probe.cpp';cpp.write_text('#include "spu_dma_publication.h"\nint main(){ static spu_context c;static unsigned char ls[SPU_LS_SIZE];c.ls=ls;spu_dma_publication_observe(&c,0,0x20000,128,0x20);for(int i=0;i<8;i++)spu_dma_publication_observe_atomic(&c,0,0x20000,0xD0,SPU_DMA_ATOMIC_GETLLAR_COMPLETE);}\n')
    probe=Path(tmp)/'probe'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-pthread','-I'+str(spu),str(cpp),str(obj),'-o',str(probe)],check=True)
    linked=subprocess.run([str(probe)],env=dict(env,SPU_DMA_PUBLICATION='20000-20080',SPU_DMA_PUBLICATION_ATOMIC='1',SPU_DMA_PUBLICATION_LIMIT='2'),capture_output=True,text=True,check=True)
    assert sum(s.startswith('[dma-publication]') for s in linked.stderr.splitlines())==2,linked.stderr
    assert 'atomic=0' in linked.stderr and 'atomic=1' in linked.stderr,linked.stderr
print('PASS: real GETLLAR / successful and failed PUTLLC outcomes, unchanged reservation semantics, opt-in, bounds and shared cap')
