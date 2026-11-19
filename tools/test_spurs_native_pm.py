#!/usr/bin/env python3
"""Native policy-module seam (PS3RECOMP_SPURS_NATIVE_PM), inert by default.

Builds tests/test_spurs_native_pm.c with the real SPURS initializer,
coordinator, worker, policy runner, resolver, MFC engine and drain, and
requires:

  * switch unset = byte-identical behaviour: with a native module registered
    for the workload's image, with none registered, and with the switch on but
    nothing registered, the runs are lifted, the registry is consulted only in
    the last case, no context ever gets a service table, guest memory and the
    scheduler's bookkeeping are identical, and the policy-run recordings
    (PS3RECOMP_SPURS_PM_RECORD) of the three are event-for-event identical;
  * the identity gate: fingerprint and SHA-256 must both match; a name selects
    one implementation; shadow keeps the lifted path;
  * a native run does the same guest work with the same bookkeeping as the
    lifted one (counter, contention, idle back-off, poll answer);
  * the service window: result in r3, return to r0, r80-r127 untouched, job
    returns by host return and by computed jump, the payload hook claims an
    entry, a run that does not exit reports -2, and a branch to an empty slot
    stops the process (exit 86) with a message.

--mutants: the registry consulted while unset (M11), a service clobbering r80
(M12), the window returning without pc = r0 (M10), the SHA gate skipped, the
service table left installed after a native run; each must fail.
Manufactured modules only; no title code or data.
"""
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture'); raise SystemExit(0)
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import spurs_pm_record as rec  # noqa: E402

SPU = ROOT / 'runtime/spu'
UNITS = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
         'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
# (file relative to the repository, old, new)
MUTANTS = {
    'M11-consulted-while-unset': ('libs/spurs/cellSpurs.c', 'r->native = spurs_native_pm_mode() != SPURS_NATIVE_PM_OFF',
                                  'r->native = 1'),
    'M12-service-clobbers-r80': ('runtime/spu/spu_channels.c', '    if (fn(ctx) == SPURS_NATIVE_SERVICE_RETURN) {',
                                 '    ctx->gpr[80]._u32[0] += 1u;\n    if (fn(ctx) == SPURS_NATIVE_SERVICE_RETURN) {'),
    'M10-window-no-return-pc': ('runtime/spu/spu_channels.c',
                                '        ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;\n        if (ctx->host_depth == 0) g_spu_trampoline_fn',
                                '        if (ctx->host_depth == 0) g_spu_trampoline_fn'),
    'sha-gate-skipped': ('runtime/spu/spurs_native_pm.inc', 'if (memcmp(sha, pm->sha256, 32) != 0) {', 'if (0) {'),
    'services-left-installed': ('runtime/spu/spurs_native_pm.inc', '    c->native_services = NULL;\n    t_npm_pm = NULL;',
                                '    t_npm_pm = NULL;'),
}


def build(tmp: Path, name: str, mutant=None) -> Path:
    shadow = tmp / f'{name}-src'
    shadow.mkdir()
    sources = {u: SPU / u for u in UNITS}
    if mutant:
        rel, old, new = mutant
        src = ROOT / rel
        text = src.read_text()
        assert text.count(old) == 1, f'{name}: mutant anchor not unique in {rel}'
        (shadow / src.name).write_text(text.replace(old, new))
        if src.name.endswith('.inc'):        # included by spurs_policy.c: shadow the includer
            shutil.copy(SPU / 'spurs_policy.c', shadow / 'spurs_policy.c')
            sources['spurs_policy.c'] = shadow / 'spurs_policy.c'
        elif src.name in sources:
            sources[src.name] = shadow / src.name
    out = tmp / name
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(shadow), '-I' + str(ROOT / 'include'), '-I' + str(SPU), '-I' + str(ROOT / 'libs/spurs'),
                    '-I' + str(ROOT / 'runtime/platform'),
                    str(ROOT / 'tests/test_spurs_native_pm.c'), str(ROOT / 'libs/spurs/spurs_taskset.c'),
                    *[str(p) for p in sources.values()],
                    str(ROOT / 'runtime/platform/win32_compat.c'), str(ROOT / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(out)], check=True)
    return out


def run(binary: Path, args, env=None, expect_rc=0):
    r = subprocess.run([str(binary), *args], env=dict(BASE_ENV, **(env or {})), capture_output=True, text=True,
                       timeout=20)
    line = [l for l in r.stdout.splitlines() if l.startswith('result ')]
    if expect_rc is not None and (r.returncode != expect_rc or (expect_rc == 0 and not line)):
        raise AssertionError(f'{args} {env}: rc={r.returncode}\n{r.stdout}\n{r.stderr[-3000:]}')
    fields = dict(f.split('=', 1) for f in line[0].split()[1:]) if line else {}
    return fields, r.stderr, r.returncode


def recording(path: Path):
    """The events of every recorded run, without clocks."""
    out = []
    for f in sorted(path.glob('pmrec-*.bin')):
        for e in rec.read_file(f)[1]:
            out.append({k: v for k, v in e.items() if k != 't_ns'})
    return out


def check(binary: Path, tmp: Path) -> list[str]:
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    lifted = dict(count='6', lifted='6', native='0', services_installed='0', lifted_saw_services='0',
                  curcont='0', active='0', ready='0', poll='3')
    # --- switch unset = byte-identical behaviour --------------------------------
    arms = {}
    for name, env, reg, consults in (('unset+registered', {}, 'reg', '0'), ('unset+none', {}, 'none', '0'),
                                     ('on+none', {'PS3RECOMP_SPURS_NATIVE_PM': '1'}, 'none', '1')):
        d = tmp / f'rec-{name}'
        r, err, _ = run(binary, ['sched', reg], dict(env, PS3RECOMP_SPURS_PM_RECORD=str(d)))
        for k, v in dict(lifted, consults=consults).items():
            expect(r.get(k) == v, f'{name}: {k}={r.get(k)} expected {v}')
        if name.startswith('unset'):
            expect('[spurs-native-pm]' not in err, f'{name}: native-pm output while unset: {err[-400:]}')
        arms[name] = (r, recording(d))
    base_r, base_rec = arms['unset+none']
    expect(len(base_rec) == 6 * 6, f'recording: {len(base_rec)} events, expected 6 runs x 6')
    for name, (r, events) in arms.items():
        expect(r['mem'] == base_r['mem'] and r['idle'] == base_r['idle'],
               f'{name}: memory/bookkeeping differ from unset+none: {r} vs {base_r}')
        expect(events == base_rec, f'{name}: policy-run recording differs from unset+none')
    # and without the recorder
    plain, _, _ = run(binary, ['sched', 'reg'])
    expect(plain['mem'] == base_r['mem'] and plain['consults'] == '0', f'unset without recorder: {plain}')

    # --- identity gate and modes --------------------------------------------------
    r, err, _ = run(binary, ['sched', 'badsha'], {'PS3RECOMP_SPURS_NATIVE_PM': '1'})
    expect(r.get('lifted') == '6' and r.get('native') == '0', f'wrong SHA ran native: {r}')
    expect('SHA-256 differs' in err, 'no SHA refusal line')
    r, err, _ = run(binary, ['sched', 'reg'], {'PS3RECOMP_SPURS_NATIVE_PM': 'shadow'})
    expect(r.get('lifted') == '6' and r.get('native') == '0', f'shadow ran native: {r}')
    r, err, _ = run(binary, ['sched', 'reg'], {'PS3RECOMP_SPURS_NATIVE_PM': 'someother'})
    expect(r.get('lifted') == '6' and r.get('native') == '0' and 'not selected' in err, f'name filter: {r}')
    native = {}
    for value, mode_bit in (('1', 1 << 1), ('check', 1 << 2), ('fixture-native', 1 << 1)):
        r, err, _ = run(binary, ['sched', 'reg'], {'PS3RECOMP_SPURS_NATIVE_PM': value})
        want = dict(count='6', lifted='0', native='6', services_installed='6', consults='1', curcont='0',
                    active='0', ready='0', poll='3', modes=str(mode_bit))
        for k, v in want.items():
            expect(r.get(k) == v, f'native {value}: {k}={r.get(k)} expected {v}')
        expect('runs native fixture-native' in err, f'native {value}: no selection line')
        native[value] = r
    # a native run's guest effect and bookkeeping equal the lifted run's
    expect(native['1'].get('mem') == base_r['mem'] and native['1'].get('idle') == base_r['idle'],
           f'native bookkeeping differs: {native["1"]} vs {base_r}')

    # --- the window, job returns, payload hook ---------------------------------
    r, err, _ = run(binary, ['direct'])
    want = dict(rc0='0', rc1='-2', rc2='0', native='2', lifted='1', services='4', job_ok='4', job_bad='0',
                hook='6', job_c='0', services_installed='2', lifted_saw_services='0', hook_default_null='1', count='3')
    for k, v in want.items():
        expect(r.get(k) == v, f'direct: {k}={r.get(k)} expected {v}')
    # the short dispatch entry (PS3RECOMP_SPU_IB_FAST) never answers a window slot:
    # every service call still reaches its service
    fr, _, _ = run(binary, ['direct'], {'PS3RECOMP_SPU_IB_FAST': '1', 'SPU_DISPATCH_CACHE': '1'})
    expect(fr == r, f'direct with PS3RECOMP_SPU_IB_FAST=1 differs: {fr} vs {r}')
    # the same with the policy-run recorder on: identical results, and the native
    # runs' polls, job entries, service calls/returns and exits are recorded
    d = tmp / 'rec-direct'
    rr, _, _ = run(binary, ['direct'], {'PS3RECOMP_SPURS_PM_RECORD': str(d)})
    expect(rr == r, f'direct with the recorder differs: {rr} vs {r}')
    runs = rec.runs(recording(d))
    kinds = [[e.get('branch') or e.get('service') or e.get('op') or e['kind'] for e in run_] for run_ in runs]
    jobs = ['job_enter', 'service_call', 'service_return', 'service_call', 'service_return', 'job_return',
            'job_enter']
    want = [['run_begin', 'getllar', 'putllc', 'select', *jobs, 'job_return', 'exit', 'run_end'],
            ['run_begin', 'getllar', 'putllc', 'select', *jobs, 'run_end'],      # the run that does not exit
            ['run_begin', 'getllar', 'putllc', 'select', 'exit', 'run_end']]     # the lifted run
    expect(kinds == want, f'recorded native/lifted runs: {kinds}')
    _, err, rc = run(binary, ['badslot'], expect_rc=None)
    expect(rc == 86 and 'no native service there' in err, f'empty slot: rc={rc} {err[-300:]}')
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutants', action='store_true')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='spurs-native-pm-') as t:
        tmp = Path(t)
        fails = check(build(tmp, 'fixture'), tmp)
        for f in fails:
            print('FAIL:', f)
        if fails:
            return 1
        print('PASS: native policy-module seam inert when unset (byte-identical runs and recordings), gated, '
              'window contract held')
        if a.mutants:
            bad = []
            for name, m in MUTANTS.items():
                work = tmp / f'm-{name}'; work.mkdir()
                try:
                    caught = bool(check(build(tmp, f'mutant-{name}', m), work))
                except (AssertionError, subprocess.SubprocessError):
                    caught = True
                print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
                if not caught:
                    bad.append(name)
            if bad:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
