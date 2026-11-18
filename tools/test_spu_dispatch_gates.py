#!/usr/bin/env python3
"""The dispatch gate line and the overlay residency counters.

The residency counters are kept only while SPU_OVERLAY_RESIDENCY_STATS or
SPU_OVERLAY_RESIDENCY_SHADOW is on, and the statistics still report them; the
gate line's copies read the same as their sources and the stack-reset count
follows registration (runtime/spu/tests/test_spu_dispatch_gates.c).
"""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
units = ['spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy',
         'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
base_env = {k: v for k, v in os.environ.items()
            if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_', 'YDKJ_', 'LBP_'))}

with tempfile.TemporaryDirectory(prefix='spu-dispatch-gates-') as tmp:
    binary = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_dispatch_gates.c'), *[str(spu / (u + '.c')) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'),
                    str(root / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True, timeout=300)

    def run(**extra):
        r = subprocess.run([str(binary)], env=dict(base_env, **extra), capture_output=True,
                           text=True, timeout=60, cwd=tmp)
        assert r.returncode == 0, (extra, r.stdout, r.stderr[-4000:])
        return r.stdout.splitlines(), r.stderr

    for extra, dcache, scope in (({}, 0, 0), ({'SPU_DISPATCH_CACHE': '1'}, 1, 0),
                                 ({'SPU_DISPATCH_CACHE': 'shadow'}, 2, 0),
                                 ({'SPU_DISPATCH_CACHE': '1', 'SPU_REGION_FRAMED': '1'}, 1, 1)):
        out, _ = run(**extra)
        assert out[0] == out[1] == f'gates dcache={dcache} scope={scope}', (extra, out)
        assert out[2].endswith(f'scope={2 if scope else 1}'), (extra, out)
        assert out[3] == 'stack reset needed before=0 after=1 count=1', (extra, out)
    print('PASS gate line: copies match their sources; the stack-reset count follows registration')

    counted = 'residency checks=1 dispatches=6 refused=0'
    out, err = run()
    assert out[-1] == 'residency checks=0 dispatches=0 refused=0', out
    assert '[spu overlay residency]' not in err, err
    out, err = run(SPU_OVERLAY_RESIDENCY_STATS='1')
    assert out[-1] == counted, out
    assert ('[spu overlay residency] loads=0 checks=1 rejects=0 dispatches=6 refused=0'
            ' shadow_checks=0 shadow_mismatches=0') in err, err
    out, err = run(SPU_OVERLAY_RESIDENCY_SHADOW='1')
    assert out[-1] == counted, out
    print('PASS residency counters: idle by default; counted and reported with statistics or shadow on')
