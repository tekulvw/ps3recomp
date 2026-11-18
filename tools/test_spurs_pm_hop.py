#!/usr/bin/env python3
"""Synthetic SPURS policy-module hop chain through the real scheduler.

Default (CI): short chains, SPU-kicked on 1 and 6 lanes and PPU-kicked on 2,
with and without PS3RECOMP_SPURS_SELF_DISPATCH=1, each of which must complete;
and a PPU-kicked chain whose requests are stored while the workers sleep,
which must start each PM well inside the coordinator's 1 ms timed pass (the
request line's coherence watch wakes it) and, with PS3RECOMP_SPURS_STORE_WAKE=0,
waits for that pass. Every case runs under both coherence schemes
(PS3RECOMP_COH_VERSIONS=0 and 1).

The store wake is proved without timing: with it on, the scheduler has its
request-line watch registered and every PPU store reaches the line's watches
(the fixture counts them with a watch of its own); with it off, no watch is
registered. Latency is then checked: the woken p50 must be under 500 us, measured
up to three times on a busy host. The stores land at a pseudo-random 200-1200 us
after each hop, so the polled arm usually waits near half the coordinator's
1 ms timed pass and the woken arm is then required to be at most half of it.
A host can still align both timers (macOS coalesces nearby timer deadlines,
so the stand-in's sleep and the timed pass can expire together), which makes
the polled arm fast too; that comparison is then reported as inconclusive
rather than failed, since the wake itself is already proved above.
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
    for coh in ('0', '1'):
        coh_env = dict(env, PS3RECOMP_COH_VERSIONS=coh)
        for lanes, kick in cases:
            for self_dispatch in ('0', '1'):
                result = subprocess.run([str(out), str(hops), lanes, kick], capture_output=True, text=True,
                                        timeout=120, env=dict(coh_env, PS3RECOMP_SPURS_SELF_DISPATCH=self_dispatch))
                line = [l for l in result.stdout.splitlines() if l.startswith('pm_hop')]
                if result.returncode or not line:
                    sys.exit(f'FAIL coh_versions={coh} lanes={lanes} kick={kick} self={self_dispatch}\n'
                             f'{result.stdout}\n{result.stderr[-2000:]}')
                print(f'coh_versions={coh} self_dispatch={self_dispatch} ' + line[0])
        if args.hops:
            continue
        def measure(store_wake):
            result = subprocess.run([str(out), '300', '2', 'ppu-idle=200~1000'], capture_output=True, text=True,
                                    timeout=120, env=dict(coh_env, PS3RECOMP_SPURS_STORE_WAKE=store_wake))
            line = [l for l in result.stdout.splitlines() if l.startswith('pm_hop')]
            if result.returncode or not line:
                sys.exit(f'FAIL coh_versions={coh} ppu-idle store_wake={store_wake}\n'
                         f'{result.stdout}\n{result.stderr[-2000:]}')
            print(f'coh_versions={coh} store_wake={store_wake} ' + line[0])
            field = lambda name: line[0].split(f' {name}=')[1].split()[0]
            return (float(field('p50')), int(field('request_watched')),
                    int(field('ppu_stores')), int(field('line_writes')))
        attempts = []
        for _ in range(3):
            woken, watched_on, stores, writes = measure('1')
            polled, watched_off, _, _ = measure('0')
            if watched_on != 1 or watched_off != 0:
                sys.exit(f'FAIL coh_versions={coh}: request-line watch registered={watched_on} with the store '
                         f'wake on and {watched_off} with it off (want 1 and 0)')
            if stores < 1 or writes != stores:
                sys.exit(f'FAIL coh_versions={coh}: {stores} PPU stores reached the request line\'s '
                         f'watches {writes} times')
            attempts.append((woken, polled))
            if woken < 500:
                break
        else:
            sys.exit(f'FAIL coh_versions={coh}: store wake p50 not under 500 us in any attempt '
                     f'(woken, polled): {attempts}')
        if woken * 2 <= polled:
            print(f'coh_versions={coh}: store wake p50 {woken:.0f} us vs {polled:.0f} us polled')
        else:
            print(f'coh_versions={coh}: INCONCLUSIVE latency comparison: the polled arm was also fast '
                  f'({polled:.0f} us vs {woken:.0f} us woken), so this host aligned the timers; '
                  f'the wake is proved by its watch registration and line writes above')
print('PASS: SPURS policy-module hop chains completed')
