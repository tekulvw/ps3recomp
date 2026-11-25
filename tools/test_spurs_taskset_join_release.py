#!/usr/bin/env python3
"""Shutdown + Join of a legacy taskset releases its workload slot (synthetic guest bytes)."""
import os, platform, subprocess, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: taskset fixture requires the POSIX toolchain'); raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spurs-join-release-') as tmp:
    tmp = Path(tmp)
    sources = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency',
               'spu_workload', 'spurs_policy', 'spurs_job', 'spu_tsp_weak',
               'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp', '../diagnostics/job_io_recorder']
    binary = tmp / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
        '-ffunction-sections', '-fdata-sections',
        '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
        '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'),
        '-I'+str(root/'runtime/platform'), str(root/'tests/test_spurs_taskset_join_release.c'),
        str(root/'libs/spurs/spurs_taskset.c'), str(root/'libs/spurs/spurs_pm.c'),
        *[str(root/'runtime/spu'/f'{s}.c') for s in sources],
        str(root/'runtime/platform/win32_compat.c'), '-pthread', '-lm',
        '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    subprocess.run([str(binary)], check=True, timeout=30, env=env, cwd=tmp)
