#!/usr/bin/env python3
"""Policy-run recorder (PS3RECOMP_SPURS_PM_RECORD / PS3RECOMP_SPURS_PM_STATS).

Builds runtime/spu/tests/test_spurs_pm_record.c against the real policy runner,
MFC engine, resolver and drain. Its manufactured policy module performs each
kind of recorded event once per run. The script requires:

  * inertness: with no variable set no context gets an observer, no file is
    written, and the run's results and guest memory are what they are with
    the recorder and the statistics on (the recorder only observes);
  * the recording decodes (tools/spurs_pm_record.py) to exactly the events
    the module performed, in order, with their data, outcomes and registers;
  * the workload filter, the per-workload run limit and the statistics lines.

PS3RECOMP_SPURS_PM_RECORD_LSREAD adds the job code's local-store quadword
reads below a limit: the script requires exactly the job's low reads (at and
inside the block its r3 names, and an absolute one), with their addresses as
formed, flags and values, none from the policy or a service or above the
limit, the rest of the recording unchanged, the aggregate table, and the hook
left unarmed (and the read gate closed) unless the switch and the recorder are
both set.

--mutants rebuilds the recorder with deliberate breaks and requires each to
fail: a dropped PUTLLC outcome, an observer armed while every variable is
unset, a lost service return, a job return not inferred when policy code
enters the next job, the hook sites' gate line never opened, a wrong drain
target for the nested call; for the read events, the limit ignored, service
reads reported, the hook armed without its switch, the in-context flag lost,
the address's low bits dropped, and the read gate left closed.
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
UNITS = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c',
         'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
MUTANTS = {
    'putllc-outcome-dropped': ('pmb_u32(&b, cmd); pmb_u32(&b, lsa); pmb_u32(&b, ea); pmb_u32(&b, status);',
                               'pmb_u32(&b, cmd); pmb_u32(&b, lsa); pmb_u32(&b, ea); pmb_u32(&b, 0);'),
    'armed-while-unset': ('    if (!s_pmrec.any) return;\n', ''),
    'service-return-lost': ('            spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_SERVICE_RETURN,',
                            '            if (0) spurs_pm_rec_branch_event(t, ctx, SPURS_PM_REC_SERVICE_RETURN,'),
    'job-return-not-inferred-at-next-entry': ('        if (!t->depth && t->region == SPURS_PM_REGION_JOB) {',
                                              '        if (0) {'),
    'gate-line-never-opened': ('        if (s_pmrec.any) __atomic_store_n(&g_spu_dispatch_gates.pm_extras, 1, __ATOMIC_RELEASE);\n', ''),
    'nested-drain-target': ('spu_drain_call(ctx, link);     /* and the drain',
                            'spu_drain_call(ctx, link + 4); /* and the drain'),
    'regs-armed-without-switch': ('          s_pmrec.regs = s_pmrec.record && rg && *rg && strcmp(rg, "0") != 0;',
                                  '          s_pmrec.regs = s_pmrec.record;'),
    'regs-not-at-entry': ('        spurs_pm_rec_regs_event(t, ctx, 1);\n', ''),
    'halt-hook-not-installed': ('          if (s_pmrec.regs) g_spurs_pm_halt_hook = spurs_pm_record_halt; }', '          }'),
    'lsread-limit-ignored': ('    if (a >= s_pmrec.lsread_limit) return;\n', ''),
    'lsread-service-reported': ('    if (t->region != SPURS_PM_REGION_JOB || t->depth) return;',
                                '    if (t->region == SPURS_PM_REGION_POLICY) return;'),
    'lsread-armed-without-switch': ('            s_pmrec.lsread_limit = host ? lim : 0;',
                                    '            s_pmrec.lsread_limit = host ? (lim ? lim : 0x4000u) : 0;'),
    'lsread-in-ctx-flag-lost': ('        flags |= SPURS_PM_LSREAD_IN_CTX;', '        flags |= 0;'),
    'lsread-low-bits-dropped': ('    pmb_u32(&b, lsa); pmb_u32(&b, 16);', '    pmb_u32(&b, q); pmb_u32(&b, 16);'),
    'lsread-gate-not-opened': ('            __atomic_store_n(&g_spu_ls_rdiag, 1, __ATOMIC_RELEASE);\n', ''),
}
BASE_ENV = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_'))}

# what the manufactured module does, in order (see test_spurs_pm_record.c)
LIST_EA, HDR_EA, REC_EA, PUT_EA, GATHER_EA, UNCOND_EA, SVC_EA, JOB_PUT_EA = (
    0x20000, 0x21000, 0x22000, 0x23000, 0x24000, 0x25000, 0x26000, 0x27000)
PM_SERVICE, PM_CALL_SITE, JOB_ENTRY, JOB_CALL_SITE, LS_CTX = 0x1A00, 0x2000, 0x4100, 0x4200, 0x1400
EXPECTED = [
    ('atomic', dict(op='getllar', ea=LIST_EA, region='policy')),
    ('atomic', dict(op='putllc', ea=LIST_EA, status=0)),
    ('atomic', dict(op='putllc', ea=LIST_EA, status=1)),
    ('command', dict(cmd=0x40, ea=HDR_EA, size=0x10, tag=1)),
    ('transfer', dict(xfer='get', ea=HDR_EA, size=0x10, captured=0x10)),
    ('command', dict(cmd=0x40, ea=REC_EA, size=0x40, tag=1)),
    ('transfer', dict(xfer='get', ea=REC_EA, size=0x40)),
    ('command', dict(cmd=0x20, ea=PUT_EA, size=0x10, tag=5)),
    ('transfer', dict(xfer='put', ea=PUT_EA, size=0x10)),
    ('command', dict(cmd=0x44, size=16, tag=6)),
    ('transfer', dict(xfer='get', ea=GATHER_EA, size=0x20, lsa=0x1300)),
    ('transfer', dict(xfer='get', ea=GATHER_EA + 0x100, size=0x10, lsa=0x1320)),
    ('atomic', dict(op='putlluc', ea=UNCOND_EA)),
    ('kernel', dict(service='select', link=PM_CALL_SITE)),
    ('branch', dict(branch='job_enter', pc=JOB_ENTRY, r0=PM_CALL_SITE + 0x14, r1=0x3FF00, ctx_lsa=LS_CTX,
                    inferred=False)),
    ('branch', dict(branch='service_call', pc=PM_SERVICE, r0=JOB_CALL_SITE + 4, slot=0x44, r4=1, r6=1,
                    depth=0)),
    ('command', dict(cmd=0x40, ea=SVC_EA, size=0x20, tag=3, region='service', depth=1)),
    ('transfer', dict(xfer='get', ea=SVC_EA, size=0x20, region='service', depth=1)),
    ('branch', dict(branch='service_return', pc=JOB_CALL_SITE + 4, depth=0)),
    ('command', dict(cmd=0x20, ea=JOB_PUT_EA, size=0x10, tag=31, region='job')),
    ('transfer', dict(xfer='put', ea=JOB_PUT_EA, size=0x10, region='job')),
    ('branch', dict(branch='job_return', inferred=True, pc=0x4400)),
    ('branch', dict(branch='job_enter', pc=0x4400, r0=PM_CALL_SITE + 0x18, inferred=False)),
    ('branch', dict(branch='job_return', inferred=True)),
    ('kernel', dict(service='exit', region='policy')),
]


def build(tmp: Path, name: str, record_inc: str | None = None) -> Path:
    out = tmp / name
    srcdir = SPU
    if record_inc is not None:
        srcdir = tmp / f'{name}-src'
        srcdir.mkdir()
        shutil.copy(SPU / 'spurs_policy.c', srcdir / 'spurs_policy.c')
        (srcdir / 'spurs_pm_record.inc').write_text(record_inc)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-w',
                    '-I' + str(ROOT / 'include'), '-I' + str(SPU), '-I' + str(ROOT / 'runtime/platform'),
                    str(SPU / 'tests/test_spurs_pm_record.c'), str(srcdir / 'spurs_policy.c'),
                    *[str(SPU / u) for u in UNITS], str(ROOT / 'runtime/diagnostics/job_io_recorder.c'),
                    str(ROOT / 'runtime/platform/win32_compat.c'), '-pthread', '-lm', '-o', str(out)],
                   check=True)
    return out


def run(binary: Path, cwd: Path, env: dict, runs: int = 2):
    r = subprocess.run([str(binary), str(runs)], env=dict(BASE_ENV, **env), capture_output=True, text=True,
                       timeout=60, cwd=cwd)
    line = [l for l in r.stdout.splitlines() if l.startswith('rc=')]
    if r.returncode or not line:
        raise AssertionError(f'fixture failed rc={r.returncode}\n{r.stdout}\n{r.stderr[-3000:]}')
    return dict(f.split('=') for f in line[0].split()), r.stderr


def check_event(got: dict, kind: str, want: dict, where: str):
    assert got['kind'] == kind, f'{where}: kind {got["kind"]} != {kind} ({got})'
    for k, v in want.items():
        g = got.get(k)
        assert g == v, f'{where}: {k}={g!r} != {v!r} ({ {x: y for x, y in got.items() if x not in ("line", "bytes", "extra")} })'


def check(binary: Path, tmp: Path) -> list[str]:
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    off_dir = tmp / 'off'; off_dir.mkdir()
    off, off_err = run(binary, off_dir, {})
    expect(off['observer'] == '0', f'observer armed with every variable unset: {off}')
    expect(off['pmread'] == '0' and off['rdiag'] == '0', f'LS read hook or gate armed while unset: {off}')
    expect(off['putllc_ok'] == '2' and off['putllc_fail'] == '2', f'fixture PUTLLC outcomes: {off}')
    expect(int(off['service_r3'], 16) == 0x5EC0FFEF, f'service result: {off}')
    expect(not list(off_dir.iterdir()), 'files written with the recorder off')
    expect('[spurs-pm-stats]' not in off_err and '[spurs-pm-record]' not in off_err, 'recorder output while off')

    rec_dir = tmp / 'rec'
    on, on_err = run(binary, tmp, {'PS3RECOMP_SPURS_PM_RECORD': str(rec_dir), 'PS3RECOMP_SPURS_PM_RECORD_BYTES': '16'})
    expect(on['observer'] == '1', 'observer not armed while recording')
    expect(on['pmread'] == '0' and on['rdiag'] == '0', f'LS read hook armed without its switch: {on}')
    for k in ('rc', 'putllc_ok', 'putllc_fail', 'service_r3', 'job_saw', 'select_wid', 'lsread_sum', 'mem'):
        expect(on[k] == off[k], f'recording changed {k}: {off[k]} -> {on[k]}')
    files = sorted(rec_dir.glob('pmrec-*.bin'))
    expect(len(files) == 1, f'expected one worker file, got {files}')
    if files:
        try:
            head, events = rec.read_file(files[0])
            expect(head['byte_cap'] == 16, f'byte cap {head}')
            runs = rec.runs(events)
            expect(len(runs) == 2, f'expected 2 runs, got {len(runs)}')
            for i, r in enumerate(runs):
                b, end = r[0], r[-1]
                check_event(b, 'run_begin', dict(run=i + 1, wid=6, lane=2, spurs_ea=0x10000, wkl_data=LIST_EA,
                                                 image_id=77, pm_size=0x2F00, line_valid=1), f'run{i}')
                check_event(end, 'run_end', dict(run=i + 1, result=0, exited=1, polls=1, jobs=2, line_valid=1),
                            f'run{i}')
                expect(b['line'][0x30:0x34] == HDR_EA.to_bytes(4, 'big'), f'run{i}: entry line')
                expect(end['line'][0x40:0x44] == (i + 1).to_bytes(4, 'big'), f'run{i}: exit line marker')
                body = r[1:-1]
                expect(len(body) == len(EXPECTED), f'run{i}: {len(body)} events, expected {len(EXPECTED)}: '
                       + ' '.join(e.get('branch') or e.get('service') or e.get('op') or e.get('xfer') or e['kind']
                                  for e in body))
                for j, (got, (kind, want)) in enumerate(zip(body, EXPECTED)):
                    check_event(got, kind, want, f'run{i} event{j}')
                if len(body) == len(EXPECTED):
                    put = body[2]['line']
                    expect(put[0x40:0x44] == (i + 1).to_bytes(4, 'big'), f'run{i}: attempted PUTLLC line')
                    hdr = body[4]
                    expect(hdr['bytes'] == bytes.fromhex('0040000100022000' '0000000300000000'), 'header bytes')
                    recg = body[6]
                    expect(len(recg['bytes']) == 16, 'byte cap not applied')
                    gather = body[10]
                    expect(gather['bytes'] == bytes(range(0x30, 0x40)), 'gathered bytes')
                    enter = body[14]
                    expect(enter['extra'][0x44:0x48] == PM_SERVICE.to_bytes(4, 'big') and len(enter['extra']) == 128,
                           'job context block')
                    ret = body[18]
                    expect(ret['r3'][0] == 0x5EC0FFEF, f'service return r3 {ret["r3"]}')
                    sel = body[13]
                    expect(sel['r3'][0] == 6 and sel['r3'][1] == 0, f'select answer {sel["r3"]}')
        except (AssertionError, rec.FormatError) as e:
            fails.append(str(e))
    expect('[spurs-pm-record] dir=' in on_err, 'no configuration line')

    # the short dispatch entry (PS3RECOMP_SPU_IB_FAST) yields to the recorder:
    # the same recording with it requested
    def events_of(d):
        return [{k: v for k, v in e.items() if k != 't_ns'}
                for f in sorted(d.glob('pmrec-*.bin')) for e in rec.read_file(f)[1]]
    ibf_dir = tmp / 'rec-ibf'
    ibf, _ = run(binary, tmp, {'PS3RECOMP_SPURS_PM_RECORD': str(ibf_dir), 'PS3RECOMP_SPURS_PM_RECORD_BYTES': '16',
                               'PS3RECOMP_SPU_IB_FAST': '1', 'SPU_DISPATCH_CACHE': '1'})
    expect(ibf['mem'] == off['mem'], 'IB_FAST + recorder changed guest memory')
    expect(files and events_of(ibf_dir) == events_of(rec_dir), 'recording differs with PS3RECOMP_SPU_IB_FAST=1')

    # workload filter and run limit (Windows-style suffix parsing is covered by the
    # C parser's own rules: a non-hex suffix is never taken)
    for spec, want_runs in ((f'{tmp}/f1:7', 0), (f'{tmp}/f2:6,a', 3), (f'{tmp}/f3::2', 2), (f'{tmp}/f4:6:1', 1)):
        _, _ = run(binary, tmp, {'PS3RECOMP_SPURS_PM_RECORD': spec}, runs=3)
        d = Path(spec.split(':')[0])
        n = sum(len(rec.runs(rec.read_file(f)[1])) for f in d.glob('pmrec-*.bin')) if d.exists() else 0
        expect(n == want_runs, f'{spec}: recorded {n} runs, expected {want_runs}')

    fails += check_lsread(binary, tmp, off, rec_dir, events_of)
    fails += check_regs(binary, tmp, off, rec_dir, events_of)

    stats_dir = tmp / 'stats'; stats_dir.mkdir()
    st, st_err = run(binary, stats_dir, {'PS3RECOMP_SPURS_PM_STATS': '1'})
    for k in ('rc', 'putllc_ok', 'putllc_fail', 'service_r3', 'job_saw', 'select_wid', 'mem'):
        expect(st[k] == off[k], f'statistics changed {k}: {off[k]} -> {st[k]}')
    expect(not list(stats_dir.iterdir()), 'statistics wrote files')
    lines = [l for l in st_err.splitlines() if l.startswith('[spurs-pm-stats]')]
    row = [l for l in lines if ' wid=6 ' in l]
    expect(bool(row), f'no statistics row: {lines}')
    if row:
        f = dict(x.split('=', 1) for x in row[-1].split()[1:] if '=' in x)
        for k, v in dict(runs='2', jobs='4', job_returns='4', services='2', polls='2', exits='2', getllar='2', putllc_ok='2',
                         putllc_fail='2', putlluc='2', idle='0', recorded='0').items():
            expect(f.get(k) == v, f'stats {k}={f.get(k)} expected {v}')
    types = [l for l in lines if 'entry-types' in l]
    expect(bool(types) and ' 1:2' in types[-1] and ' 3:2' in types[-1], f'entry types {types}')
    cmds = [l for l in lines if 'commands (' in l]
    expect(bool(cmds) and 'reserve/00000:2' in cmds[-1] and 'use/01010:2' in cmds[-1] and
           'use/01100:2' in cmds[-1] and 'run/00000:2' in cmds[-1] and 'end/00000:2' in cmds[-1]
           and 'nop/00000:6' in cmds[-1], f'command histogram {cmds}')
    slots = [l for l in lines if 'service-slots' in l]
    expect(bool(slots) and '00000000/+0x44:2' in slots[-1], f'service slots {slots}')
    return fails


LS_LOW_READS = [   # (address as formed, flags: 1 inside the r3 block, 2 at r3, value bytes)
    (LS_CTX, 3, (3).to_bytes(4, 'big') + bytes(12)),
    (LS_CTX + 0x14, 1, bytes(16)),
    (0x1C48, 0, b''.join((0xC0FFEE40 + o).to_bytes(4, 'big') for o in range(0, 16, 4))),
]


def check_lsread(binary: Path, tmp: Path, off: dict, base_dir: Path, events_of) -> list[str]:
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    # the switch alone (no recorder): ignored, unarmed
    alone_dir = tmp / 'lsread-alone'; alone_dir.mkdir()
    al, al_err = run(binary, alone_dir, {'PS3RECOMP_SPURS_PM_RECORD_LSREAD': '1'})
    expect(al['pmread'] == '0' and al['rdiag'] == '0' and al['observer'] == '0',
           f'LS read switch alone armed something: {al}')
    expect('PS3RECOMP_SPURS_PM_RECORD_LSREAD ignored' in al_err, 'no notice for the switch alone')
    expect(not list(alone_dir.iterdir()), 'files written with the switch alone')

    d = tmp / 'rec-lsread'
    on, err = run(binary, tmp, {'PS3RECOMP_SPURS_PM_RECORD': str(d), 'PS3RECOMP_SPURS_PM_RECORD_BYTES': '16',
                                'PS3RECOMP_SPURS_PM_RECORD_LSREAD': '1'})
    expect(on['pmread'] == '1' and on['rdiag'] == '1', f'LS read hook not armed: {on}')
    for k in ('rc', 'putllc_ok', 'putllc_fail', 'service_r3', 'job_saw', 'select_wid', 'lsread_sum', 'mem'):
        expect(on[k] == off[k], f'LS read events changed {k}: {off[k]} -> {on[k]}')
    try:
        got = events_of(d)
        reads = [e for e in got if e['kind'] == 'lsread']
        expect([e for e in got if e['kind'] != 'lsread'] == events_of(base_dir),
               'the rest of the recording changed with LS read events on')
        expect(len(reads) == 2 * len(LS_LOW_READS),
               f'{len(reads)} read events, expected {2 * len(LS_LOW_READS)}: '
               + ' '.join(hex(e['lsa']) for e in reads))
        for run_events in rec.runs([e for f in sorted(d.glob('pmrec-*.bin')) for e in rec.read_file(f)[1]]):
            kinds = [e.get('branch') or e['kind'] for e in run_events]
            enter = kinds.index('job_enter') if 'job_enter' in kinds else -1
            body = run_events[enter + 1:enter + 1 + len(LS_LOW_READS)]
            for i, (lsa, flags, value) in enumerate(LS_LOW_READS):
                if i >= len(body):
                    fails.append(f'read {i} missing'); break
                check_event(body[i], 'lsread', dict(lsa=lsa, size=16, flags=flags, entry=JOB_ENTRY,
                                                    ctx_lsa=LS_CTX, value=value), f'lsread {i}')
                expect(body[i]['r3'][0] == LS_CTX, f'lsread {i}: r3 {body[i]["r3"]}')
            expect(enter >= 0 and kinds[enter + 1 + len(LS_LOW_READS)] == 'service_call',
                   f'reads not between the job entry and its service call: {kinds}')
    except (AssertionError, rec.FormatError) as e:
        fails.append(str(e))
    table = sorted(d.glob('pmrec-lsread-*.txt'))
    expect(len(table) == 1, f'expected one aggregate table, got {table}')
    expect('[spurs-pm-lsread] job-region local-store reads below 0x4000 reported' in err, 'no configuration line')
    if table:
        rows = [l for l in table[0].read_text().splitlines() if ' lsq=' in l]
        want = {(f'{lsa & ~15:05X}', flags) for lsa, flags, _ in LS_LOW_READS}
        seen = set()
        for l in rows:
            f = dict(x.split('=', 1) for x in l.split()[1:] if '=' in x)
            fl = (1 if f['in_ctx'] != '0' else 0) | (2 if f['at_ctx'] != '0' else 0)
            seen.add((f['lsq'], fl))
            expect(f['entry'] == f'{JOB_ENTRY:05X}' and f['reads'] == '2' and f['changed'] == '0',
                   f'aggregate row {l}')
        expect(seen == want, f'aggregate rows {sorted(seen)} != {sorted(want)}')
        head = [l for l in table[0].read_text().splitlines() if 'job_reads=' in l]
        expect(bool(head) and ' job_reads=8 below=6 overflow=0' in head[-1], f'aggregate totals {head}')

    # statistics only: the table goes to stderr
    sd = tmp / 'stats-lsread'; sd.mkdir()
    st, st_err = run(binary, sd, {'PS3RECOMP_SPURS_PM_STATS': '1', 'PS3RECOMP_SPURS_PM_RECORD_LSREAD': '4000'})
    expect(st['mem'] == off['mem'] and st['lsread_sum'] == off['lsread_sum'], 'statistics + LS reads changed results')
    expect(not list(sd.iterdir()), 'statistics + LS reads wrote files')
    expect(sum(' lsq=' in l for l in st_err.splitlines()) == len(LS_LOW_READS), 'no aggregate rows on stderr')
    return fails


def check_regs(binary: Path, tmp: Path, off: dict, base_dir: Path, events_of) -> list[str]:
    """PS3RECOMP_SPURS_PM_RECORD_REGS: the register file at each job entry, and at
    a halt the register file and the local store from the halted function."""
    fails = []

    def expect(cond, msg):
        if not cond:
            fails.append(msg)

    d = tmp / 'rec-regs'
    on, _ = run(binary, tmp, {'PS3RECOMP_SPURS_PM_RECORD': str(d), 'PS3RECOMP_SPURS_PM_RECORD_BYTES': '16',
                              'PS3RECOMP_SPURS_PM_RECORD_REGS': '1'})
    for k in ('rc', 'putllc_ok', 'putllc_fail', 'service_r3', 'job_saw', 'select_wid', 'mem'):
        expect(on[k] == off[k], f'register events changed {k}: {off[k]} -> {on[k]}')
    got = events_of(d)
    regs = [e for e in got if e['kind'] == 'regs']
    expect([e for e in got if e['kind'] != 'regs'] == events_of(base_dir), 'the rest of the recording changed with REGS on')
    expect(len(regs) == 4 and all(e['why'] == 'job_enter' for e in regs), f'entry register files: {[e["why"] for e in regs]}')
    if regs:
        e = regs[0]
        expect(e['pc'] == JOB_ENTRY and e['gpr'][3][0] == LS_CTX and e['gpr'][1][0] == 0x3FF00 and e['entry'] == JOB_ENTRY,
               f'entry registers: pc {e["pc"]:#x} r3 {e["gpr"][3]} r1 {e["gpr"][1]}')
    # without the switch: no register events
    expect(not [e for e in events_of(base_dir) if e['kind'] == 'regs'], 'register events without the switch')
    # a halt: its register file and the local store at the halted function
    hd = tmp / 'rec-regs-halt'
    r = subprocess.run([str(binary), '1'], env=dict(BASE_ENV, PS3RECOMP_SPURS_PM_RECORD=str(hd), PS3RECOMP_SPURS_PM_RECORD_REGS='1',
                                                     PMREC_FIXTURE_HALT='1'), capture_output=True, text=True, timeout=60, cwd=tmp)
    halts = [e for e in events_of(hd) if e['kind'] == 'regs' and e['why'] == 'halt']
    expect(len(halts) == 1 and halts[0]['pc'] == 0x4400 and halts[0]['gpr'][77][2] == 0xFEED and len(halts[0]['ls']) == 512,
           f'halt register file: {[(h["pc"], h["gpr"][77], len(h["ls"])) for h in halts]} rc={r.returncode}')
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutants', action='store_true')
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='spurs-pm-record-') as t:
        tmp = Path(t)
        (tmp / 'run').mkdir()
        fails = check(build(tmp, 'fixture'), tmp / 'run')
        for f in fails:
            print('FAIL:', f)
        if fails:
            return 1
        print('PASS: recorder inert when unset, observes without effect, decodes to the performed events')
        if a.mutants:
            source = (SPU / 'spurs_pm_record.inc').read_text()
            bad = []
            for name, (old, new) in MUTANTS.items():
                assert source.count(old) == 1, f'mutant {name}: anchor not unique'
                work = tmp / f'm-{name}'; work.mkdir()
                try:
                    caught = bool(check(build(tmp, f'mutant-{name}', source.replace(old, new)), work))
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
