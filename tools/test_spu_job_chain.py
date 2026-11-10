#!/usr/bin/env python3
"""Synthetic SPU job chain through the real channel, MFC and lock-line paths.

Default: a short run that checks every job's output and the completion
counters (CI). --jobs N: a timed run; prints nanoseconds per channel write,
tag wait, small DMA, GETLLAR/PUTLLC, job, job with completion, a memcpy floor
for the same bytes and one hop of a two-thread dependent chain. The runtime's
defaults that MAG sets (SPU_MFC_LOOKUP_CACHE=1, SPU_DISPATCH_CACHE=1) are
applied unless --runtime-defaults off. No game content.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture (pthreads, clock_gettime)')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
units = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
         'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c',
         '../diagnostics/job_io_recorder.c']
parser = argparse.ArgumentParser()
parser.add_argument('--jobs', type=int, default=2000)
parser.add_argument('--runs', type=int, default=1)
parser.add_argument('--runtime-defaults', choices=('mag', 'off'), default='mag')
args = parser.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
if args.runtime_defaults == 'mag':
    env.update(SPU_MFC_LOOKUP_CACHE='1', SPU_DISPATCH_CACHE='1')
with tempfile.TemporaryDirectory(prefix='spu-job-chain-') as tmp:
    binary = Path(tmp) / 'bench'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/bench_spu_job_chain.c'), *[str(spu / u) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(binary)],
                   check=True)
    for _ in range(args.runs):
        result = subprocess.run([str(binary), str(args.jobs)], env=env, capture_output=True, text=True, timeout=600)
        if result.returncode:
            sys.exit(f'FAIL (exit {result.returncode})\n{result.stdout}\n{result.stderr[-2000:]}')
        print(result.stdout.strip())
print(f'PASS: synthetic job chain, {args.jobs} jobs, outputs and completion counters checked')
