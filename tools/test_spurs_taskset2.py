#!/usr/bin/env python3
"""Taskset2 guest layout, registered worker execution and generated firmware NID."""
import os, platform, subprocess, sys, tempfile
from pathlib import Path
from gen_hle_nids import scan

root = Path(__file__).resolve().parents[1]
handlers = dict(scan(root / 'libs/spurs/cellSpurs.c'))
assert handlers.get(0x4A6465E3) == 'cellSpursCreateTaskset2', 'Taskset2 HLE is absent'
assert handlers.get(0xC2ACDF43) == '_cellSpursTasksetAttribute2Initialize'
if os.name == 'nt':
    print('SKIP: worker fixture requires the POSIX toolchain'); raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-taskset2-') as tmp:
    tmp = Path(tmp)
    nids = tmp / 'nids.cpp'
    subprocess.run([sys.executable, str(root/'tools/gen_hle_nids.py'),
                    'cellSpurs', '--out', str(nids)], check=True)
    assert 'ps3_hle_register(0x4A6465E3u, "cellSpursCreateTaskset2",' in nids.read_text()
    sources = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency',
               'spu_workload', 'spurs_policy', 'spurs_job', 'spu_tsp_weak',
               'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
        '-ffunction-sections', '-fdata-sections',
        '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'),
        '-I'+str(root/'runtime/platform'), str(root/'tests/test_spurs_taskset2.c'),
        str(root/'libs/spurs/spurs_taskset.c'), str(root/'libs/spurs/spurs_pm.c'),
        *[str(root/'runtime/spu'/f'{s}.c') for s in sources],
        str(root/'runtime/platform/win32_compat.c'), '-pthread', '-lm',
        '-o', str(binary)], check=True)
    env = {k:v for k,v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    subprocess.run([str(binary)], check=True, timeout=15, env=env, cwd=tmp)
