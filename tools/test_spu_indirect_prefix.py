#!/usr/bin/env python3
"""Synthetic full indirect dispatch with diagnostics off and individually on."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX full-resolver fixture')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
spu=root/'runtime/spu'
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy',
       'spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','PS3_','PS3RECOMP_','YDKJ_'))}
with tempfile.TemporaryDirectory(prefix='spu-indirect-prefix-') as tmp:
    binary=Path(tmp)/'fixture'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-w',
                    '-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),
                    str(spu/'tests/test_spu_indirect_prefix.c'),
                    *[str(spu/(u+'.c')) for u in units],
                    str(root/'runtime/platform/win32_compat.c'),
                    str(root/'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread','-lm','-o',str(binary)],check=True,timeout=60)
    markers={'SPU_IBCOV':'[ibcov3]','SPU_POLLTRACE':'[ibranch]','YDKJ_POLTRACE':'[POLTRACE]'}
    for switch in (None,*markers):
        result=subprocess.run([str(binary)],env=dict(env,**({switch:'1'} if switch else {})),
                              capture_output=True,text=True,timeout=10)
        assert result.returncode==0,result.stdout+result.stderr
        for key,marker in markers.items():
            assert (marker in result.stderr)==(key==switch),(switch,marker,result.stderr)
        print('PASS indirect entry diagnostics:',switch or 'off')
