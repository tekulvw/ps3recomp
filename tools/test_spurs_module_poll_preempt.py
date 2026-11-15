#!/usr/bin/env python3
"""cellSpursModulePoll preemption (PS3RECOMP_SPURS_MODULE_POLL_PREEMPT=1).

Every SPU holds an idling policy module that polls the kernel; another
workload is then requested through the instance's workload flag. Without the
switch it waits for an idler to exit on its own (the fixture's 300 ms); with
it, one idler is told to switch, exits, and the flagged workload runs within
milliseconds, exactly once. A flagged workload that the running one outranks
(the runner is requested and has the higher priority) gets no switch.
Runs with and without PS3RECOMP_COH_VERSIONS=1. Manufactured modules only.
"""
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
r = Path(__file__).resolve().parents[1]
units = ['spu_channels', 'spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy',
         'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
base = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
with tempfile.TemporaryDirectory(prefix='spurs-poll-preempt-') as tmp:
    out = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(r / 'include'), '-I' + str(r / 'runtime/spu'), '-I' + str(r / 'runtime/platform'),
                    str(r / 'tests/test_spurs_module_poll_preempt.c'), str(r / 'libs/spurs/spurs_taskset.c'),
                    *[str(r / 'runtime/spu' / f'{u}.c') for u in units],
                    str(r / 'runtime/platform/win32_compat.c'), str(r / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(out)], check=True)

    def run(lanes, mode, preempt, cohv):
        env = dict(base, PS3RECOMP_SPURS_MODULE_POLL_PREEMPT=preempt, PS3RECOMP_COH_VERSIONS=cohv)
        res = subprocess.run([str(out), str(lanes), mode], env=env, capture_output=True, text=True, timeout=60)
        line = [l for l in res.stdout.splitlines() if l.startswith('lanes=')]
        if res.returncode or not line:
            sys.exit(f'FAIL lanes={lanes} mode={mode} preempt={preempt} cohv={cohv}\n{res.stdout}\n{res.stderr[-2000:]}')
        print(f'preempt={preempt} cohv={cohv} {line[0]}')
        return {k: float(v) for k, v in (f.split('=') for f in line[0].split()[2:])}

    for cohv in ('0', '1'):
        for lanes in (1, 5):
            off = run(lanes, 'flag', '0', cohv)
            assert off['b_latency_us'] >= 200000 and off['switch_answers'] == 0, off
            on = run(lanes, 'flag', '1', cohv)
            assert 0 <= on['b_latency_us'] < 50000, on
            assert on['switch_answers'] == 1 and on['b_runs'] == 1, on   # one idler, once: no herd, no ping-pong
            low = run(lanes, 'lowprio', '1', cohv)
            assert low['switch_answers'] == 0, low                     # outranked: no switch
print('PASS: module poll preemption switches one idler to a flagged workload, respects priority')
