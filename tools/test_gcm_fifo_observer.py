#!/usr/bin/env python3
"""Run the real FIFO drain with observation off, on, malformed, and capped."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: fixture requires POSIX mmap/linker support')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-observer-') as tmp:
    out=Path(tmp)/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections','-I',str(ROOT/'include'),
        str(ROOT/'libs/video/tests/test_gcm_fifo_observer.c'),
        str(ROOT/'libs/video/tests/gcm_deferred_fixture_stubs.c'),
        '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
        '-pthread','-lm','-o',str(out)],check=True,timeout=60)
    clean={k:v for k,v in os.environ.items() if not k.startswith('GCM_FIFO_OBSERVE')}
    def run(settings,mode=None):
        result=subprocess.run([str(out)]+([mode] if mode else []),env=dict(clean,**settings),capture_output=True,text=True,check=True,timeout=10)
        return [x for x in result.stderr.splitlines() if x.startswith('[fifo-observe]')]
    boundary=run({"GCM_FIFO_OBSERVE":"0x0-0x40"},"zero-boundary")
    assert len(boundary)==2 and all('kind=zeros' in x and 'zero_words=4' in x for x in boundary),boundary
    assert 'io=00000000' in boundary[0] and 'end=000000010' in boundary[0] and 'last_put=00000010' in boundary[0],boundary
    assert 'io=00000010' in boundary[1] and 'end=000000020' in boundary[1] and 'first_put=00000020' in boundary[1],boundary
    assert len(run({"GCM_FIFO_OBSERVE":"0x0-0x40","GCM_FIFO_OBSERVE_LIMIT":"32768"},"zeros"))==3
    assert run({"GCM_FIFO_OBSERVE":"0x0-0x40","GCM_FIFO_OBSERVE_LIMIT":"32769"},"zeros")==[]
    zeros=run({"GCM_FIFO_OBSERVE":"0x0-0x40"},"zeros")
    assert len(zeros)==3 and 'kind=zeros' in zeros[0] and 'io=00000000' in zeros[0] and 'end=000000020' in zeros[0] and 'zero_words=8' in zeros[0],zeros
    assert run({},"zeros")==[]
    assert len(run({"GCM_FIFO_OBSERVE":"0x0-0x40","GCM_FIFO_OBSERVE_LIMIT":"2"},"zeros"))==2
    classified=run({"GCM_FIFO_OBSERVE":"0x18-0x40"},"classification")
    assert len(classified)==1 and "kind=flip" in classified[0],classified
    assert run({})==[]
    for bad in ('','garbage','0x40-0x18','0x18-0x40junk','0x100000000-0x100000004'):
        assert run({'GCM_FIFO_OBSERVE':bad})==[]
    for option in ('GCM_FIFO_OBSERVE_LIMIT','GCM_FIFO_OBSERVE_WORDS'):
        for bad in ('0','-1','garbage','4294967296'):
            assert run({'GCM_FIFO_OBSERVE':'0x18-0x40',option:bad})==[]
    clipped=run({'GCM_FIFO_OBSERVE':'0x18-0x1C'})
    assert len(clipped)==1 and 'sampled=1' in clipped[0] and 'payload=AA000005' in clipped[0],clipped
    lines=run({'GCM_FIFO_OBSERVE':'0x18-0x40','GCM_FIFO_OBSERVE_WORDS':'2'})
    assert any('io=00000000 header=40240300' in x and 'end=000000028' in x and 'sample_io=00000018' in x and 'AA000005,AA000006' in x and 'truncated=1' in x for x in lines),lines
    assert sum('kind=park' in x for x in lines)==1,lines
    assert any('decision=jump target=00000038' in x for x in lines),lines
    assert any('event=report argument=01000430' in x for x in lines),lines
    assert len(run({'GCM_FIFO_OBSERVE':'0x18-0x40','GCM_FIFO_OBSERVE_LIMIT':'2'}))==2
    print('PASS: actual drain overlap, payload truncation, opt-in validation, park suppression, branch/report events, and cap')
