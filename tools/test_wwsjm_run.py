#!/usr/bin/env python3
"""The native WWS job manager's run loop (runtime/spu/wwsjm/wwsjm_run.inc).

Builds tests/test_wwsjm_run.c with the real SPURS initializer, coordinator,
five workers, policy runner, resolver, MFC engine and drain, and requires:

  * switch unset: the lifted path; the registry is never consulted and no job
    runs natively (the implementation is inert);
  * PS3RECOMP_SPURS_NATIVE_PM=1 and =check: every job of a manufactured list
    runs exactly once across five lanes. The list has two header arrays (one
    with a capacity that is not a power of two, crossing the slot wrap), a
    no-op, a gate opened by dependency decrements and their ready-count wake, a
    report gate and an array switch. Each job sees the registers, context
    words, parameter data, module bytes, gathered inputs, get-buffer
    descriptors and DMA tags the specification gives it. The PPU's completion
    predicate never reports a job whose output is missing, and the final line
    has every lane free and N == S on both arrays.

--mutants: run-loop breaks that the fixture must catch (reservation loss
ignored, a retire that drops the held job, decrements or wakes skipped, the
userdata pointer at the command portion, modules never reloaded, the tag
counter kept across jobs, finish claiming nothing, the switch's array
ignored).
Manufactured modules and lists only; no title code or data.
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
SPU = ROOT / 'runtime/spu'
UNITS = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
         'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}
RUN = 'runtime/spu/wwsjm/wwsjm_run.inc'
# (file relative to the repository, old, new)
MUTANTS = {
    'M4-reservation-loss-ignored': (RUN, '        if (!wj_putllc(r, WJ_LINE_LS, r->list_ea, now)) continue;   /* lost: re-read */\n',
                                    '        (void)wj_putllc(r, WJ_LINE_LS, r->list_ea, now);\n'),
    'retire-drops-held-job': (RUN, '    wwsjm_held held = { r->next.valid, r->next.array, r->next.index };',
                              '    wwsjm_held held = { 0, r->next.array, r->next.index };'),
    'decrement-skipped': (RUN, '    for (unsigned d = 0; d < r->plan.dep_count; d++) {', '    for (unsigned d = 0; d < 0; d++) {'),
    'wake-skipped': (RUN, '        wj_ready_store(r, target, value);\n        WJ_COUNT(wakes, 1);',
                     '        (void)target; (void)value;\n        WJ_COUNT(wakes, 1);'),
    'M8-userdata-at-portion': (RUN, '    a.userdata_ls = WJ_PORTION_LS + r->plan.userdata_offset;', '    a.userdata_ls = WJ_PORTION_LS;'),
    'module-never-reloaded': (RUN, '            if (l->skip) { WJ_COUNT(load_skips, 1); continue; }',
                              '            if (l->skip || r->res.n) { WJ_COUNT(load_skips, 1); continue; }'),
    'tags-kept-across-jobs': (RUN, '    r->tags = 0;\n    memset(r->set_tag, 0, sizeof r->set_tag);\n    r->finishes = 0;',
                              '    r->finishes = 0;'),
    'finish-claims-nothing': (RUN, '        else if (wj_claim(r, &r->next, 1)) wwsjm_finish_result(&r->next.h, result);',
                              '        else if (0) wwsjm_finish_result(&r->next.h, result);'),
    'context-array-dropped': (RUN, '    a.array = job->array;', '    a.array = 0;'),
    # spec 13.4: the fixed low local store
    'low-ls-not-planted': (RUN, '    wj_plant_low_ls(r);\n    r->tags = 0;', '    r->tags = 0;'),
    'vector-not-planted': (RUN, '    for (unsigned i = 0; i < 4; i++) wwsjm_put32(ls + 4 * i,',
                           '    for (unsigned i = 0; i < 0; i++) wwsjm_put32(ls + 4 * i,'),
    'vector-to-job-return': (RUN, 'wj_bra(SPURS_NATIVE_PM_SLOT_LS(WJ_SVC_IRQ_LANDING))',
                             'wj_bra(SPURS_NATIVE_PM_JOB_RETURN_LS)'),
    'vector-not-bra': (RUN, 'return 0x30000000u | ((target >> 2)', 'return 0x32000000u | ((target >> 2)'),
    'low-zeros-skipped': (RUN, '    memset(ls + 0x10, 0, WJ_JOB_SCRATCH_LS - 0x10);\n', ''),
    'irq-landing-quiet': (RUN, '    [WJ_SVC_IRQ_LANDING] = wj_svc_irq_landing,\n', '    [WJ_SVC_IRQ_LANDING] = wj_svc_release,\n'),
    'kctx-lane-dropped': (RUN, '    wwsjm_put32(ls + WJ_KCTX_LS + 0xC8, r->lane);\n', ''),
    'kctx-instance-dropped': (RUN, '    wwsjm_put32(ls + WJ_KCTX_LS + 0xC4, r->spurs_ea);\n', ''),
    'kctx-select-dropped': (RUN, '    wwsjm_put32(ls + WJ_KCTX_LS + 0xE4, SPURS_PM_SELECT_WORKLOAD_LS);\n', ''),
    'context-moved': (RUN, '    WJ_CTX_LS = 0x1330, ', '    WJ_CTX_LS = 0x1200, '),
    'command-copy-moved': (RUN, '    WJ_PORTION_LS = 0xC00, ', '    WJ_PORTION_LS = 0x1200, '),
    'context-in-job-scratch': (RUN, '    WJ_CTX_LS = 0x1330, ', '    WJ_CTX_LS = 0x80, '),
    # spec v0.5 (5.1, 13.4, 14): interrupt state at entry, the whole kernel context
    'int-enable-not-set': (RUN, '    c->event_mask = 0;\n    c->int_enable = 1;', '    c->event_mask = 0;'),
    'event-mask-not-cleared': (RUN, '    c->event_mask = 0;\n    c->int_enable = 1;', '    c->int_enable = 1;'),
    'service-sets-int-enable': (RUN, '    if (++r->finishes > 1 && r->check)',
                                '    c->int_enable = 1;\n    if (++r->finishes > 1 && r->check)'),
    'kctx-low-not-zeroed': (RUN, '    memset(ls + WJ_KCTX_LS, 0, 0xC0);\n', ''),
    'kctx-workload-addr-dropped': (RUN, '    wwsjm_put32(ls + WJ_KCTX_LS + 0xD4, 0xA00);\n', ''),
    'kctx-unique-id-dropped': (RUN, '    wwsjm_put32(ls + WJ_KCTX_LS + 0xD8, r->wid);\n', ''),
    # spec v0.7 (16, 6.1, 6.5): verify after ready := 0, the in-run flag
    'verify-skipped': (RUN, '            if (wj_verify(&r)) continue;\n            goto verified;', '            goto verified;'),
    'flag-poll-write-dropped': (RUN, '    if (status == 4) { wj_ready_store(r, r->wid, WWSJM_READY_ON_FLAG); WJ_COUNT(flag_polls, 1); }',
                                '    if (status == 4) { WJ_COUNT(flag_polls, 1); }'),
    # spec v0.6 (5.1, 15): r1 word 1, the stack bytes the ABI stack check tests
    # spec v0.8 (17): the gather list copy, zero-size elements, tags per set
    'list-copy-at-start': (RUN, '        const uint32_t list_ls = l->ls + l->limit - l->bytes;', '        const uint32_t list_ls = l->ls;'),
    'zero-size-element-advances': ('runtime/spu/wwsjm/wwsjm_core.c', '        if (!word)\n            continue;\n', ''),
    'window-top-3C000': (RUN, '    WJ_LS_HIGH = 0x3E000, ', '    WJ_LS_HIGH = 0x3C000, '),
    'tag-per-call': (RUN, '        if (!r->set_tag[set]) {', '        if (1) {'),
    'stack-bytes-zero': (RUN, 'spurs_native_pm_enter_job(c, entry, WJ_STACK, WJ_STACK_BYTES, WJ_CTX_LS, 0)',
                         'spurs_native_pm_enter_job(c, entry, WJ_STACK, 0, WJ_CTX_LS, 0)'),
    'stack-bytes-too-few': (RUN, 'spurs_native_pm_enter_job(c, entry, WJ_STACK, WJ_STACK_BYTES, WJ_CTX_LS, 0)',
                            'spurs_native_pm_enter_job(c, entry, WJ_STACK, 0x80, WJ_CTX_LS, 0)'),
    'stack-reserved-overlap': (RUN, '    WJ_CTX_LS = 0x1330, ', '    WJ_CTX_LS = 0x3F000, '),
}


# Breaks of the native run the shadow comparison must report as mismatches.
SHADOW_SEES = ('flag-poll-write-dropped', 'M4-reservation-loss-ignored', 'retire-drops-held-job', 'decrement-skipped', 'wake-skipped',
               'M8-userdata-at-portion', 'module-never-reloaded', 'tags-kept-across-jobs', 'finish-claims-nothing',
               'context-array-dropped')


def build(tmp: Path, name: str, mutant=None) -> Path:
    shadow = tmp / f'{name}-src'
    shadow.mkdir()
    sources = {u: SPU / u for u in UNITS}
    incs = []
    if mutant:
        rel, old, new = mutant
        src = ROOT / rel
        text = src.read_text()
        assert text.count(old) == 1, f'{name}: mutant anchor not unique in {rel}'
        # the run loop is an include of spurs_policy.c: shadow the include chain
        (shadow / 'wwsjm').mkdir()
        for f in ('wwsjm_core.c', 'wwsjm_core.h', 'wwsjm_run.inc', 'wwsjm_shadow.inc'):
            shutil.copy(SPU / 'wwsjm' / f, shadow / 'wwsjm' / f)
        (shadow / 'wwsjm' / src.name).write_text(text.replace(old, new))
        shutil.copy(SPU / 'spurs_native_pm.inc', shadow / 'spurs_native_pm.inc')
        shutil.copy(SPU / 'spurs_policy.c', shadow / 'spurs_policy.c')
        sources['spurs_policy.c'] = shadow / 'spurs_policy.c'
    out = tmp / name
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w', '-DWWSJM_TEST_HOOKS',
                    '-ffunction-sections', '-fdata-sections',
                    '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections',
                    '-I' + str(shadow), '-I' + str(ROOT / 'include'), '-I' + str(SPU), '-I' + str(ROOT / 'libs/spurs'),
                    '-I' + str(ROOT / 'runtime/platform'),
                    str(ROOT / 'tests/test_wwsjm_run.c'), str(ROOT / 'libs/spurs/spurs_taskset.c'),
                    *[str(p) for p in sources.values()],
                    str(ROOT / 'runtime/platform/win32_compat.c'), str(ROOT / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(out)], check=True)
    return out


def run(binary: Path, args, env=None):
    r = subprocess.run([str(binary), *args], env=dict(BASE_ENV, **(env or {})), capture_output=True, text=True,
                       timeout=60)
    line = [l for l in r.stdout.splitlines() if l.startswith('result ')]
    if r.returncode != 0 or not line:
        raise AssertionError(f'{args} {env}: rc={r.returncode}\n{r.stdout}\n{r.stderr[-3000:]}')
    return dict(f.split('=', 1) for f in line[0].split()[1:]), r.stderr


def check(binary: Path) -> list[str]:
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    r, err = run(binary, ['unset'])
    expect(r['ran'] == '0' and r['consults'] == '0' and int(r['lifted']) > 0 and '[wwsjm]' not in err
           and '[spurs-native-pm]' not in err, f'unset: not inert: {r} {err[-300:]}')
    r, err = run(binary, ['native'], {'PS3RECOMP_SPURS_NATIVE_PM': '1'})
    expect('wid=3 runs native wwsjm-fixture' in err, f'native: no selection line: {err[-400:]}')
    for mode in ('1', 'check'):
        r, err = run(binary, ['native'], {'PS3RECOMP_SPURS_NATIVE_PM': mode})
        want = dict(done='1', jobs='73', ran='73', dup='0', missing='0', bad_out='0', regs='0', ctx='0', ud='0',
                    module='0', gather='0', desc='0', tag='0', finish='0', violations='0', lifted='0', current='1',
                    n0='43', s0='43', n1='77', s1='77', lanes_busy='0', gate='0', switches='FFFE', lowls='0', irq='0', stack='0', listcopy='0', race='12', stalls='0', lost_kicks='0')
        for k, v in want.items():
            expect(r.get(k) == v, f'native {mode}: {k}={r.get(k)} expected {v}')
        expect(int(r.get('finish_jobs', 0)) > 0, f'native {mode}: finish never claimed a next job: {r}')
        stats = [l for l in err.splitlines() if l.startswith('[wwsjm] runs=')]
        expect(bool(stats), f'native {mode}: no [wwsjm] counters')
        if stats:
            s = dict(f.split('=', 1) for f in stats[-1].split()[1:])
            expect(s.get('jobs') == '73' and s.get('decrements') == '8' and s.get('wakes') == '1' and
                   int(s.get('resident_skips', 0)) > 0 and s.get('retires') == '73' and
                   int(s.get('putllc_retries', 0)) > 0,
                   f'native {mode}: counters {s}')
            # the in-run flag poll is followed by ready := 8 (6.1, U19); the race
            # entries' kicks are never lost (spec 16, R12: stalls=0 above)
            expect(s.get('flag_polls') == '1', f'native {mode}: in-run flag poll: {s}')
        # every one of the 8 in-window publications whose kick the lane then
        # overwrote with ready := 0 was followed by its verify (R12)
        expect(int(r.get('verified_kicks', 0)) >= 1, f'native {mode}: no kick was overwritten and verified: {r}')
        expect('FATAL' not in err, f'native {mode}: {err[-600:]}')
    # the interrupt vector's target halts loudly
    ri = subprocess.run([str(binary), 'native-irq'], env=dict(BASE_ENV, PS3RECOMP_SPURS_NATIVE_PM='1'),
                        capture_output=True, text=True, timeout=60)
    expect(ri.returncode != 0 and 'FATAL SPU interrupt vector entered' in ri.stderr,
           f'interrupt vector target did not halt loudly: rc={ri.returncode} {ri.stderr[-400:]}')
    # the shadow comparison, fed the native runs' own events: every run identical,
    # also with the short dispatch entry requested (it must yield to the comparison:
    # the recorder that feeds it sees every branch)
    for mode, extra in (('1', {}), ('check', {}),
                        ('1', {'PS3RECOMP_SPU_IB_FAST': '1', 'SPU_DISPATCH_CACHE': '1'})):
        r, err = run(binary, ['native'], dict(extra, PS3RECOMP_SPURS_NATIVE_PM=mode, PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW='1'))
        if extra:
            expect('PS3RECOMP_SPU_IB_FAST=1 ignored: a native policy-module shadow comparison is on' in err,
                   f'self-shadow with IB_FAST: the short entry did not yield: {err[-400:]}')
        expect(r.get('done') == '1' and r.get('dup') == '0', f'self-shadow {mode}: {r}')
        sh = shadow_summary(err)
        expect(sh is not None and sh['mismatches'] == 0 and sh['unverifiable'] == 0 and sh['runs'] > 0 and
               sh['identical'] == sh['runs'] and all(sh['ok'].get(c, 0) > 0 for c in SHADOW_CLASSES),
               f'self-shadow {mode}: {sh} {err[-800:]}')
    return fails


SHADOW_CLASSES = ('claim', 'retire', 'stop', 'decrement', 'wake', 'ready', 'plan', 'load', 'load_skip', 'gather',
                  'context', 'userdata', 'getbuf', 'release', 'finish')


def shadow_summary(err: str):
    lines = [l for l in err.splitlines() if l.startswith('[wwsjm-shadow] runs=')]
    if not lines:
        return None
    l = lines[-1]
    head = dict(f.split('=', 1) for f in l.split(' ok{')[0].split()[1:])
    ok = dict(f.split('=', 1) for f in l.split(' ok{')[1].split('}')[0].split())
    tail = dict(f.split('=', 1) for f in l.rsplit('}', 1)[1].split())
    return dict(runs=int(head['runs']), identical=int(head['identical']), ok={k: int(v) for k, v in ok.items()},
                mismatches=int(tail['mismatches']), unverifiable=int(tail['unverifiable']))


def shadow_detects(binary: Path) -> bool:
    """A broken native run, fed to the shadow comparison, must show mismatches."""
    try:
        r, err = run(binary, ['native'], {'PS3RECOMP_SPURS_NATIVE_PM': '1', 'PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW': '1'})
    except AssertionError as e:
        err = str(e)
    sh = shadow_summary(err)
    return sh is not None and sh['mismatches'] > 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutants', action='store_true')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='wwsjm-run-') as t:
        tmp = Path(t)
        fails = check(build(tmp, 'fixture'))
        for f in fails:
            print('FAIL:', f)
        if fails:
            return 1
        print('PASS: native WWS job manager run loop: inert when unset; 73 jobs on two workloads, gates, a wake, a switch, lost reservations, '
              'non-power-of-two wrap, concurrent publication and late headers, an in-run flag, variable reserves; contract held on five '
              'lanes (1 and check)')
        if a.mutants:
            bad = []
            for name, m in MUTANTS.items():
                try:
                    binary = build(tmp, f'mutant-{name}', m)
                except AssertionError as e:          # a stale anchor is a broken test, not a catch
                    print(f'[FAIL] mutant {name}: {e}')
                    bad.append(name)
                    continue
                except subprocess.SubprocessError:   # refused at compile time (a static assertion)
                    print(f'[PASS] mutant {name} caught (does not compile)')
                    continue
                try:
                    caught = bool(check(binary))
                except (AssertionError, subprocess.SubprocessError):
                    caught = True
                print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
                if not caught:
                    bad.append(name)
                # the shadow comparison must see the same break on its own
                if name in SHADOW_SEES:
                    seen = shadow_detects(tmp / f'mutant-{name}')
                    print(f"[{'PASS' if seen else 'FAIL'}] mutant {name} {'seen' if seen else 'NOT seen'} by the shadow comparison")
                    if not seen:
                        bad.append(name + ' (shadow)')
            if bad:
                return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
