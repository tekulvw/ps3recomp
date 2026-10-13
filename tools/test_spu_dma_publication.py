#!/usr/bin/env python3
"""Outgoing DMA observation through real direct/list PUTs, synthetic bytes only."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture requires POSIX threads'); raise SystemExit(0)
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_'))}
with tempfile.TemporaryDirectory(prefix='dma-publication-') as tmp:
    binary=Path(tmp)/'fixture'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-pthread',
        '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),
        str(root/'runtime/spu/tests/test_spu_dma_publication.c'),'-o',str(binary)],check=True)
    def run(settings, mode=None):
        p=subprocess.run([str(binary)]+([mode] if mode else []), env=dict(env,**settings),capture_output=True,text=True,check=True,timeout=10)
        return [x for x in p.stderr.splitlines() if x.startswith('[dma-publication]')]
    raw={'SPU_DMA_PUBLICATION':'12010-12050','SPU_DMA_PUBLICATION_WORDS':'2'}
    assert run({})==[]
    assert run({},"disabled")==[]
    lines=run(raw)
    assert len(lines)==2, ('expected direct/list source publication reports',lines)
    assert all('kind=span' in x and 'sample_ea=00012010' in x and 'words=2' in x and 'AA000004,AA000005' in x and 'truncated=1' in x for x in lines),lines
    match={'SPU_DMA_PUBLICATION_MATCH_RANGE':'12000-12080','SPU_DMA_PUBLICATION_MATCH_WORD':'12345678'}
    lines=run(match)
    assert len(lines)==2 and all('kind=match' in x and 'match_ea=00012020' in x and '12345678' in x for x in lines),lines
    assert len(run(dict(raw,**match,SPU_DMA_PUBLICATION_LIMIT='3'),'threads'))==3
    for bad in ('','garbage','12080-12000','12000-12080junk','100000000-100000004','-1-20','\n12000-12080'):
        assert run({'SPU_DMA_PUBLICATION':bad})==[]
    for option in ('SPU_DMA_PUBLICATION_WORDS','SPU_DMA_PUBLICATION_LIMIT'):
        for bad in ('0','-1','garbage','4294967296'):
            assert run(dict(raw,**{option:bad}))==[]
    assert run(dict(match,SPU_DMA_PUBLICATION_MATCH_WORD='12345678junk'))==[]
    assert run(dict(match,SPU_DMA_PUBLICATION_MATCH_RANGE='12000-12020'))==[]
    full=run({'SPU_DMA_PUBLICATION_MATCH_RANGE':'12000-16000','SPU_DMA_PUBLICATION_MATCH_WORD':'12345678'},'full')
    assert len(full)==1 and 'match_ea=00015FFC' in full[0] and '12345678' in full[0],full
    assert run(dict(raw,SPU_DMA_PUBLICATION_LIMIT='32768'))
    # C++ callers share the C implementation and its process-global cap.
    impl=Path(tmp)/'impl.c'
    impl.write_text('#include "spu_dma_publication_impl.h"\nvoid SetLastError(DWORD e) {(void)e;}\n')
    obj=Path(tmp)/'impl.o'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-pthread','-I'+str(root/'runtime/spu'),'-c',str(impl),'-o',str(obj)],check=True)
    cpp=Path(tmp)/'probe.cpp'
    cpp.write_text('#include "spu_dma_publication.h"\nint main(){static spu_context s; static unsigned char ls[SPU_LS_SIZE]; s.ls=ls; for(int i=0;i<4;i++) spu_dma_publication_observe(&s,0,0x12000,16,0x20); }\n')
    probe=Path(tmp)/'probe'
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-pthread','-I'+str(root/'runtime/spu'),str(cpp),str(obj),'-o',str(probe)],check=True)
    linked=subprocess.run([str(probe)],env=dict(env,SPU_DMA_PUBLICATION='12000-12010',SPU_DMA_PUBLICATION_LIMIT='2'),capture_output=True,text=True,check=True)
    assert sum(x.startswith('[dma-publication]') for x in linked.stderr.splitlines())==2,linked.stderr
    assert run(dict(raw,SPU_DMA_PUBLICATION_WORDS='17'))==[]
    assert run(dict(raw,SPU_DMA_PUBLICATION_LIMIT='32769'))==[]
    assert run({'SPU_DMA_PUBLICATION':'12011-12013'})==[]
    assert run({'SPU_DMA_PUBLICATION':'1207C-12080'})[0].endswith('payload=AA00001F')
print('PASS: actual direct/list PUT bytes unchanged, raw intersection, descriptor match, strict opt-in bounds and concurrent global cap')
