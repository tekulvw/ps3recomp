#!/usr/bin/env python3
"""Lifted-style call brackets through the real drain and indirect dispatch.

Default (CI): 200000 calls of each bracket must all reach the leaf and return.
--calls N --runs R: timed runs (ns per call). No lifted code, no game content.
Requires Clang (override with CLANG): spu_dispatch_mt.c uses its musttail syntax.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
units = ['spu_dispatch_mt.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
         'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c',
         'spu_interp.c', '../diagnostics/job_io_recorder.c']
parser = argparse.ArgumentParser()
parser.add_argument('--calls', type=int, default=200000)
parser.add_argument('--runs', type=int, default=1)
args = parser.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
env.update(SPU_MFC_LOOKUP_CACHE='1', SPU_DISPATCH_CACHE='1')   # MAG's runtime defaults
with tempfile.TemporaryDirectory(prefix='spu-call-dispatch-') as tmp:
    binary = Path(tmp) / 'bench'
    subprocess.run([os.environ.get('CLANG', 'clang'), '-std=gnu17', '-O2', '-UNDEBUG', '-w', '-DSPU_USE_DISPATCH_MT',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/bench_spu_call_dispatch.c'), *[str(spu / u) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(binary)], check=True)
    for _ in range(args.runs):
        r = subprocess.run([str(binary), str(args.calls)], env=env, capture_output=True, text=True, timeout=300)
        if r.returncode:
            sys.exit(f'FAIL\n{r.stdout}\n{r.stderr[-2000:]}')
        print(r.stdout.strip())
print('PASS: lifted-style call brackets returned through the drain')
