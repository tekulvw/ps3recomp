#!/usr/bin/env python3
"""Async SPU tasks through the real dispatch, pool off and on: nothing a task
leaves on its host thread (stack-keyed registrations, per-task thread-local
state) reaches the next task, and results agree."""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture (links the SPU runtime with the POSIX toolchain); '
          'tools/test_spu_task_pool.py covers the Windows threads')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
units = ['spu_drain', 'spu_lockstep', 'spu_workload', 'spurs_policy', 'spurs_job',
         'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
env = {k: v for k, v in os.environ.items()
       if not k.startswith(('SPU_', 'SPURS_', 'PS3_SPU_', 'YDKJ_', 'LBP_', 'GH3_', 'PS3RECOMP_SPU_TASK_POOL'))}
with tempfile.TemporaryDirectory(prefix='spu-async-task-pool-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_async_task_pool.c'),
                    *[str(spu / (u + '.c')) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'),
                    str(root / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True, timeout=180)
    results = {}
    for mode in (None, '1'):
        e = dict(env)
        if mode:
            e['PS3RECOMP_SPU_TASK_POOL'] = mode
        label = 'pool on' if mode else 'pool off'
        run = subprocess.run([str(binary)], env=e, cwd=tmp, timeout=120, capture_output=True, text=True)
        print(f'--- {label}', flush=True)
        print(run.stdout.strip(), flush=True)
        if run.returncode:
            print(run.stderr[-4000:], flush=True)
            raise SystemExit(f'{label}: exit {run.returncode}')
        results[label] = next(line for line in run.stdout.splitlines() if line.startswith('RESULT '))
    assert results['pool off'] == results['pool on'], results
    print('PASS async SPU tasks: no state crosses a reused thread; pool off and on agree', flush=True)
