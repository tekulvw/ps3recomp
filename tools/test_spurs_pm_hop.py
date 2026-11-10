#!/usr/bin/env python3
"""Synthetic SPURS policy-module hop chain through the real scheduler.

Default (CI): short chains, SPU-kicked on 1 and 6 lanes and PPU-kicked on 2,
with and without PS3RECOMP_SPURS_SELF_DISPATCH=1, each of which must complete.
--hops N: timed runs; prints per-hop wall time and request-to-start latency.
Manufactured policy modules only; no game content.
"""
import argparse
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
r = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--hops', type=int, default=0, help='timed run with this many hops per case')
parser.add_argument('--lanes', default='1,4,6')
args = parser.parse_args()
units = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy',
         'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
with tempfile.TemporaryDirectory(prefix='spurs-pm-hop-') as tmp:
    out = Path(tmp) / 'bench'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(r / 'include'), '-I' + str(r / 'runtime/spu'), '-I' + str(r / 'runtime/platform'),
                    str(r / 'tests/bench_spurs_pm_hop.c'), str(r / 'libs/spurs/spurs_taskset.c'),
                    *[str(r / 'runtime/spu' / f'{u}.c') for u in units],
                    str(r / 'runtime/platform/win32_compat.c'), str(r / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(out)], check=True)
    if args.hops:
        cases = [(lanes, kick) for kick in ('spu', 'ppu-idle=300') for lanes in args.lanes.split(',')]
        hops = args.hops
    else:
        cases = [('1', 'spu'), ('6', 'spu'), ('2', 'ppu')]
        hops = 200
    for lanes, kick in cases:
        for self_dispatch in ('0', '1'):
            result = subprocess.run([str(out), str(hops), lanes, kick], capture_output=True, text=True, timeout=120,
                                    env=dict(env, PS3RECOMP_SPURS_SELF_DISPATCH=self_dispatch))
            line = [l for l in result.stdout.splitlines() if l.startswith('pm_hop')]
            if result.returncode or not line:
                sys.exit(f'FAIL lanes={lanes} kick={kick} self={self_dispatch}\n{result.stdout}\n{result.stderr[-2000:]}')
            print(f'self_dispatch={self_dispatch} ' + line[0])
print('PASS: SPURS policy-module hop chains completed')
