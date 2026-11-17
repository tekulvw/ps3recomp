#!/usr/bin/env python3
"""PS3RECOMP_SPU_DISPATCH_FAST: the indirect-branch fast path and the
dispatch-gate line change no observable result.

The fixture (runtime/spu/tests/test_spu_dispatch_fast.c) branches through one
scenario per case the fast path must leave to the full resolver. Its output
must be identical with the switch off and on, with no diagnostics and under
each diagnostic switch. Each fast-path exclusion is then removed in turn
(a mutant); every mutant must change the output, so no exclusion is untested.
The overlay residency counters must keep counting with the switch off, or
with their statistics or shadow check on, and stay idle otherwise.
"""
import os
import subprocess
import sys
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
DIAGNOSTICS = [None, 'SPU_IBHIST', 'SPU_IBCOV', 'SPU_POLLTRACE', 'YDKJ_POLTRACE',
               'SPU_JOBTRACE', 'SPU_JOBEXEC', 'LBP_HLE_JOBDONE', 'YDKJ_CRI_CHAIN']

# Each fast-path exclusion and the source change that removes it.
WRAPPER = 'void spu_indirect_branch(spu_context* ctx)\n{\n    if (spu_dispatch_fast()) {'
MUTANTS = {
    'SPU_IBHIST gate': ('plain = !spu_ibhist_armed() && ', 'plain = '),
    'IB diagnostics gate': ('!spu_ib_diagnostics_armed() &&\n', '\n'),
    'SPU_JOBTRACE gate': ('!getenv("SPU_JOBTRACE") && ', ''),
    'LBP_HLE_JOBDONE gate': ('!getenv("LBP_HLE_JOBDONE") && ', ''),
    'SPU_JOBEXEC gate': (' && !getenv("SPU_JOBEXEC");', ';'),
    'saved interrupt registers': ('if (plain && !ctx->irq_saved && ', 'if (plain && '),
    'taskset syscall address': ('pc != SPURS_TASKSET_PM_SYSCALL_LS &&\n', '\n'),
    'resolver-only image': ('ctx->image_id != SPU_RESOLVER_ONLY_IMAGE_ID &&\n', '1 &&\n'),
    'policy exit-to-kernel': ('(pc == SPURS_PM_EXIT_TO_KERNEL_LS || ', '(0 || '),
    'policy select-workload': (' || pc == SPURS_PM_SELECT_WORKLOAD_LS))', ' || 0))'),
    'task resume at depth zero': ('                    goto full;   /* the task resume',
                                  '                    (void)0;   /* the task resume'),
    'job-return address': ('if (fn && (ctx->policy_mode || pc != SPU_JOB_RETURN_LS)) {',
                           'if (fn) {'),
    'stack-reset mirror': ('        __atomic_store_n(&g_spu_dispatch_gates.stack_reset_count,n+1,__ATOMIC_RELEASE);\n', ''),
    'miss': ('if (fn && (ctx->policy_mode || pc != SPU_JOB_RETURN_LS)) {',
             'if (ctx->policy_mode || pc != SPU_JOB_RETURN_LS) { if (!fn) return;'),
}


def build(tmp, name, channels=None):
    src = spu / 'tests/test_spu_dispatch_fast.c'
    if channels is not None:
        text = src.read_text().replace('#include "../spu_channels.c"', f'#include "{channels}"')
        src = Path(tmp) / f'{name}.c'
        src.write_text(text)
    binary = Path(tmp) / name
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(src), *[str(spu / (u + '.c')) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'),
                    str(root / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True, timeout=300)
    return binary


def run(binary, tmp, **extra):
    env = dict(base_env, **extra)
    r = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=120, cwd=tmp)
    assert r.returncode == 0, (extra, r.returncode, r.stdout, r.stderr[-4000:])
    out = [l for l in r.stdout.splitlines() if not l.startswith('residency')]
    res = [l for l in r.stdout.splitlines() if l.startswith('residency')]
    err = [l for l in r.stderr.splitlines() if not l.startswith('residency')]
    return out, err, res


def compare(binary, tmp):
    """Return the first diagnostic setting whose on/off results differ, or None."""
    for diag in DIAGNOSTICS:
        extra = {diag: '1'} if diag else {}
        off = run(binary, tmp, **extra)[:2]
        on = run(binary, tmp, PS3RECOMP_SPU_DISPATCH_FAST='1', **extra)[:2]
        if off != on:
            return diag or 'none', off, on
    return None


with tempfile.TemporaryDirectory(prefix='spu-dispatch-fast-') as tmp:
    binary = build(tmp, 'fixture')
    diff = compare(binary, tmp)
    assert diff is None, diff
    print('PASS fast path: identical results off/on, no diagnostics and under',
          ', '.join(d for d in DIAGNOSTICS if d))

    want = 'residency checks=1 dispatches=6'
    for extra, expect in (({}, want),
                          ({'PS3RECOMP_SPU_DISPATCH_FAST': '1'}, 'residency checks=0 dispatches=0'),
                          ({'PS3RECOMP_SPU_DISPATCH_FAST': '1', 'SPU_OVERLAY_RESIDENCY_STATS': '1'}, want),
                          ({'PS3RECOMP_SPU_DISPATCH_FAST': '1', 'SPU_OVERLAY_RESIDENCY_SHADOW': '1'}, want)):
        res = run(binary, tmp, **extra)[2]
        assert res == [expect], (extra, res)
    print('PASS residency counters: kept off, and with statistics or shadow on; idle otherwise')

    channels = (spu / 'spu_channels.c').read_text()
    assert channels.count(WRAPPER) == 1
    start = channels.index(WRAPPER)
    for name, (old, new) in MUTANTS.items():
        at = start if channels.count(old, start) == 1 else 0
        assert channels.count(old, at) == 1, (name, old)
        mutant = channels[:at] + channels[at:].replace(old, new, 1)
        path = spu / f'.mutant_dispatch_fast_{os.getpid()}.c'
        try:
            path.write_text(mutant)
            mbinary = build(tmp, 'mutant', channels=str(path))
        finally:
            path.unlink(missing_ok=True)
        diff = compare(mbinary, tmp)
        assert diff is not None, f'mutant not detected: {name}'
        print(f'PASS mutant detected: {name} (under {diff[0]})')
