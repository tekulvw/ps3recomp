#!/usr/bin/env python3
"""The micro-interpreter's decoded-trace cache changes nothing it computes.

Builds runtime/spu/tests/test_spu_smc_trace.c against the real spu_channels.c
and runs the same random stubs with PS3RECOMP_SPU_SMC_TRACE=0 (the ordinary
microstep only) and with the cache on: every per-run digest of registers,
local store, pc, interrupt enable and trampoline must match. --cases N --bench
also times one 121-instruction save stub both ways. Synthetic data only.
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
units = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c', 'spurs_job.c',
         'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
parser = argparse.ArgumentParser()
parser.add_argument('--cases', type=int, default=800)
parser.add_argument('--bench', action='store_true', help='also time a 121-instruction save stub both ways')
args = parser.parse_args()
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
with tempfile.TemporaryDirectory(prefix='spu-smc-trace-') as tmp:
    binary = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_smc_trace.c'), *[str(spu / u) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(binary)], check=True)
    outs = {}
    for mode in ('0', '1'):
        r = subprocess.run([str(binary), str(args.cases)], env=dict(env, PS3RECOMP_SPU_SMC_TRACE=mode),
                           capture_output=True, text=True, timeout=600)
        if r.returncode:
            sys.exit(f'FAIL mode={mode}\n{r.stdout}\n{r.stderr[-2000:]}')
        outs[mode] = r.stdout
    if outs['0'] != outs['1']:
        sys.exit('FAIL: trace cache diverged from the ordinary microstep\n--- off\n' + outs['0'] + '--- on\n' + outs['1'])
    print(outs['1'].strip().splitlines()[-1])
    if args.bench:
        for mode in ('0', '1'):
            r = subprocess.run([str(binary), 'bench'], env=dict(env, PS3RECOMP_SPU_SMC_TRACE=mode),
                               capture_output=True, text=True, timeout=600)
            print(r.stdout.strip())
print(f'PASS: {args.cases} random stubs x 3 runs identical with and without the trace cache')
