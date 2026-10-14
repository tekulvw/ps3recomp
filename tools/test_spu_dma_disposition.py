#!/usr/bin/env python3
"""Actual core/list DMA outcomes, synthetic memory and host-owned trigger only."""
import os, subprocess, tempfile, re
from pathlib import Path
root=Path(__file__).resolve().parents[1]
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','LBP_'))}
with tempfile.TemporaryDirectory(prefix='dma-disposition-') as tmp:
    binary=Path(tmp)/'fixture'
    flags=['-std=gnu17','-O2','-UNDEBUG','-pthread','-I'+str(root/'include'),'-I'+str(root/'runtime/spu')]
    if os.environ.get('DMA_DISPOSITION_UBSAN'): flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CC','cc'),*flags,str(root/'runtime/spu/tests/test_spu_dma_disposition.c'),'-o',str(binary)],check=True)
    def run(settings={},mode=None):
        p=subprocess.run([str(binary)]+([mode] if mode else []),env=dict(env,**settings),capture_output=True,text=True,check=True,timeout=10)
        return [dict(re.findall(r'(\w+)=([^ ]*)',x)) for x in p.stderr.splitlines() if x.startswith('[dma-disposition]')]
    span={'SPU_DMA_DISPOSITION':'12000-12040'}
    assert run(span,'errno')[0]['copied']=='1'
    assert run({},'errno')==[]
    assert run(dict(span,SPU_DMA_DISPOSITION_TRIGGER=str(Path(tmp)/'missing')),'errno')==[]
    assert run(span,'reserved')[0]['disposition']=='copied-put'
    failed=run(span,'tag-failure')
    assert len(failed)==1 and failed[0]['rc']=='-1' and failed[0]['copied']=='0' and failed[0]['words']=='0'
    zero=run({'SPU_DMA_DISPOSITION':'1000-12100','LBP_SKIP_NULL_DMA':'1'},'zeros')
    assert [r['disposition'] for r in zero]==['zeroed-ea','zeroed-ls']
    assert all(r['copied']=='0' and r['words']=='0' for r in zero)
    assert run({},'disabled')==[]
    rows=run(span)
    assert [r['disposition'] for r in rows]==['copied-put','copied-get','copied-put','malformed-no-copy','rejected','rejected'],rows
    assert [r['rc'] for r in rows]==['0','0','0','0','-1','-1']
    assert all(r['before']==r['after'] and r['words']=='8' and r['copied']=='1' for r in rows[:3])
    assert all(r['words']=='0' and r['copied']=='0' for r in rows[3:])
    assert [int(r['seq']) for r in rows]==list(range(1,7))
    rows=run(dict(span,SPU_DMA_DISPOSITION_WORDS='2'))
    assert all(r['words']=='2' for r in rows[:3])
    rows=run({'SPU_DMA_DISPOSITION':'12011-1201F'})
    assert all(r['sample_offset']=='20' and r['words']=='2' for r in rows[:3])
    assert run({'SPU_DMA_DISPOSITION':'13000-13010'})==[]
    for k,values in {'SPU_DMA_DISPOSITION':['','-1-12040','12040-12000','12000-100000001','12000-12040junk'], 'SPU_DMA_DISPOSITION_WORDS':['0','17','-1','2x'], 'SPU_DMA_DISPOSITION_LIMIT':['0','32769','-1',''], 'SPU_DMA_DISPOSITION_TRIGGER':['','x'*129]}.items():
        for value in values: assert run(dict(span,**{k:value}),'disabled')==[],(k,value)
    rows=run(dict(span,SPU_DMA_DISPOSITION_LIMIT='7'),'threads')
    assert len(rows)==7 and sorted(int(r['seq']) for r in rows)==list(range(1,8))
    assert all(r['disposition']=='copied-get' and r['copied']=='1' for r in rows)
    trigger=Path(tmp)/'arm'
    rows=run(dict(span,SPU_DMA_DISPOSITION_TRIGGER=str(trigger),SPU_DMA_DISPOSITION_LIMIT='6'),'trigger')
    assert len(rows)==6 and rows[0]['seq']=='1',rows
    assert trigger.is_file()
    directory=Path(tmp)/'dir';directory.mkdir()
    assert run(dict(span,SPU_DMA_DISPOSITION_TRIGGER=str(directory)))==[]
    assert run(dict(span,SPU_DMA_DISPOSITION_TRIGGER=str(Path(tmp)/'missing')))==[]
print('PASS: real direct/list copies, malformed rc0 no-copy, validation failures, bounded payload/range, disabled poison, pre-trigger budget, exact concurrent cap')
