#!/usr/bin/env python3
"""Real policy request / PPU local-store read / mailbox reply with manufactured bytes only."""
import os, platform, subprocess, tempfile
from pathlib import Path
if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-policy-service-') as tmp:
    out=Path(tmp)/'test'
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload',
           'spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap',
           'spurs_policy_blob_weak','spu_interp']
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
        '-ffunction-sections','-fdata-sections',
        '-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections',
        '-I'+str(r/'include'),'-I'+str(r/'runtime/spu'),'-I'+str(r/'runtime/platform'),
        str(r/'runtime/spu/tests/test_spurs_policy_service.c'),
        str(r/'runtime/spu/spu_lifted_thread.c'),
        str(r/'runtime/syscalls/spu_fallback.c'),
        str(r/'runtime/syscalls/sdk_version_weak.c'),
        *[str(r/'runtime/spu'/f'{u}.c') for u in units],
        str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out)],check=True)
    env=os.environ.copy();env.pop('SPU_CH_BLOCK',None)
    subprocess.run([str(out)],check=True,timeout=15,env=env)
