#!/usr/bin/env python3
"""The split FIFO walker (PS3RECOMP_RSX_SPLIT_WALKER, libs/video/gcm_walker_split.inc).

libs/video/tests/test_gcm_split_walker.c is built once and run twice: the
single-thread form (`off`) and the split (`on`). Their logs -- every engine
call in order, the guest memory a walk leaves, a 30k-packet random stream's
call hash -- must be the same, and the `on` form checks the split's own
rules: every effect on the record thread, a fast guest writer that can never
beat a draw to its label/ref/flip/get, ring reuse, lease yielding, drains
before an unmap and at terminate, resync and checked REF.

When origin/main is available the `off` form is also built against
origin/main's cellGcmSys.c (the walker before the seam) and its log must be
identical: the refactor changed nothing with the switch off.

--mutants builds each listed mutation and requires the fixture to fail it.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
video = root / 'libs/video'
if os.name == 'nt':
    print('SKIP: the split walker fixture uses POSIX mmap and pthreads')
    raise SystemExit(0)

CC = os.environ.get('CC', 'cc')


def build(tmp, label, include_first=None, defines=(), extra=()):
    binary = Path(tmp) / ('fixture-' + label)
    cmd = [CC, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
           '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function']
    cmd += [f'-D{d}' for d in defines]
    cmd += list(extra)
    if include_first:
        cmd += [f'-I{include_first}']
    cmd += ['-I' + str(root / 'include'), '-I' + str(video),
            str(video / 'tests/test_gcm_split_walker.c'),
            '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
            str(video / 'tests/gcm_deferred_fixture_stubs.c'), str(video / 'rsx_fifo_busy.c'),
            '-pthread', '-lm', '-o', str(binary)]
    subprocess.run(cmd, check=True, timeout=300)
    return binary


def run(binary, form, timeout=240, scale=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3RECOMP_', 'GCM_', 'MAG_', 'RSX_'))}
    if scale: env['GCM_SPLIT_TEST_SCALE'] = str(scale)
    r = subprocess.run([str(binary), form], capture_output=True, text=True, timeout=timeout, env=env)
    return r.returncode, r.stdout, r.stderr


def log_lines(out):
    # The on form presents at the flip itself; the off form leaves it to the host.
    return [l for l in out.splitlines() if l.startswith('L ') and not l.startswith('L present-at-flip')]


def check(binary, label, scale=None):
    rc_off, off, _ = run(binary, 'off', scale=scale)
    rc_on, on, err_on = run(binary, 'on', scale=scale)
    ok = rc_off == 0 and rc_on == 0 and log_lines(off) == log_lines(on) \
        and '[rsx split walker] on:' in err_on and 'engine calls' in on
    return ok, off, on, err_on


MUTANTS = {
    # every effect on the decoder (the seam's split path removed): wrong thread, labels early
    'methods-at-decode': ('gcm_walker_split.inc', [
        ('    const u32 type = w >> 29, method = w & 0x1FFCu, subch = (w >> 13) & 7;\n    if (s_split != 1)\n        return gcm_fx_methods',
         '    const u32 type = w >> 29, method = w & 0x1FFCu, subch = (w >> 13) & 7;\n    if (1)\n        return gcm_fx_methods')]),
    # the semaphore packet (label release) applied by the decoder: the label beats the draw
    'label-at-decode': ('gcm_walker_split.inc', [
        ('    const u32 type = w >> 29, method = w & 0x1FFCu;\n    if (s_split != 1) {\n        *blocked = gcm_fx_sema',
         '    const u32 type = w >> 29, method = w & 0x1FFCu;\n    if (1) {\n        *blocked = gcm_fx_sema')]),
    # flips-reached bumped by the decoder: the flip completes before its draws are recorded
    'flip-at-decode': ('gcm_walker_split.inc', [
        ('    if (s_split != 1) {\n        /* Direct: the host presents from the receipt, as before. */',
         '    if (1) {\n        /* Direct: the host presents from the receipt, as before. */')]),
    # the NV0039 copy made by the decoder: guest memory written before earlier draws are recorded
    'nv0039-at-decode': ('gcm_walker_split.inc', [
        ('    if (s_split != 1) { nv0039_copy(s_nv0039.in_dma, s_nv0039.out_dma, v, io); return 0; }',
         '    if (1) { nv0039_copy(s_nv0039.in_dma, s_nv0039.out_dma, v, io); return 0; }')]),
    # `get` published by the decoder: get == put before the draw is recorded
    'get-at-decode': ('gcm_walker_split.inc', [
        ('    if (s_split != 1) { vm_write32(GCM_CONTROL_GUEST_ADDR + 4, value); return; }',
         '    if (1) { vm_write32(GCM_CONTROL_GUEST_ADDR + 4, value); return; }')]),
    # the draw fence ignored: `get` runs the lead past an unrecorded draw
    'no-draw-fence': ('gcm_walker_split.inc', [
        ('        if (head < s_rq.getq[h % 64u].draw_fence || pos > head + s_rq.get_lead) {',
         '        if (pos > head + s_rq.get_lead) {')]),
    # the record thread reads the guest ring instead of its copy
    'ring-read': ('gcm_walker_split.inc', [
        ('        return gcm_fx_methods(type, subch, method, count, io, r + 4,',
         '        return gcm_fx_methods(type, subch, method, count, io, NULL,')]),
    # the lease never yielded: a guest report wait takes the whole queue
    'no-yield': ('gcm_walker_split.inc', [
        ('            if (atomic_load_explicit(&s_lease_waiters, memory_order_acquire) > 0) gcm_split_lease_yield();\n',
         '')]),
    # the checked-REF bit not carried: the record thread takes no receipt
    'checked-dropped': ('gcm_walker_split.inc', [
        ('    r[0] = GCM_RQ_HDR(GCM_RQ_METHODS, (checked ? GCM_RQ_F_CHECKED : 0) | (plain_walk ? GCM_RQ_F_PLAIN : 0) |',
         '    r[0] = GCM_RQ_HDR(GCM_RQ_METHODS, (0) | (plain_walk ? GCM_RQ_F_PLAIN : 0) |')]),
    # the pass end not queued: the ref is never published
    'no-pass-end': ('gcm_walker_split.inc', [
        ('    r[0] = GCM_RQ_HDR(GCM_RQ_PASS_END, gcm_checked_ref_mode() ? GCM_RQ_F_CHECKED : 0, 1); r[1] = (u32)issue;\n    gcm_rq_commit(2);',
         '    r[0] = GCM_RQ_HDR(GCM_RQ_PAD, 0, 1); r[1] = (u32)issue;\n    gcm_rq_commit(2);')]),
    # an IO unmap without draining: the record thread meets an unmapped page
    'no-drain-unmap': ('cellGcmSys.c', [
        ('void cellGcm_syscall_iounmap(u32 io, u32 size)\n{\n    gcm_split_drain();    /* what the decoder reads changes: the record thread catches up first */\n',
         'void cellGcm_syscall_iounmap(u32 io, u32 size)\n{\n')]),
    # terminate without stopping the record thread
    'no-stop-terminate': ('cellGcmSys.c', [
        ('void cellGcmTerminate(void)\n{\n    gcm_split_stop();     /* the record thread ends before the renderer state does */\n',
         'void cellGcmTerminate(void)\n{\n')]),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mutants', action='store_true')
    ap.add_argument('--sanitize', choices=['thread', 'address'], help='also build and run the fixture under a sanitizer')
    args = ap.parse_args()
    failed = 0
    with tempfile.TemporaryDirectory(prefix='gcm-split-walker-') as tmp:
        binary = build(tmp, 'walker')
        ok, off, on, err_on = check(binary, 'walker')
        print(off.splitlines()[-1] if off else '(no off output)')
        print(on.splitlines()[-1] if on else '(no on output)')
        if not ok:
            failed += 1
            print('FAIL: the split form differs from the single-thread form or failed its own checks')
            for l in on.splitlines():
                if l.startswith('FAIL'): print('   ', l)
            a, b = log_lines(off), log_lines(on)
            for i, (x, y) in enumerate(zip(a, b)):
                if x != y: print(f'    first log difference at line {i}:\n      off: {x}\n      on:  {y}'); break
            if len(a) != len(b): print(f'    log lengths {len(a)} vs {len(b)}')
            print(err_on[-2000:])
        else:
            print(f'PASS split walker: {len(log_lines(on))} log lines equal between the forms')

        # The off form against origin/main's walker: the refactor is inert.
        ref = subprocess.run(['git', '-C', str(root), 'show', 'origin/main:libs/video/cellGcmSys.c'],
                             capture_output=True, text=True)
        if ref.returncode == 0 and 'gcm_walker_split.inc' not in ref.stdout:
            rdir = Path(tmp) / 'reference'; rdir.mkdir()
            (rdir / 'cellGcmSys.c').write_text(ref.stdout)
            rbin = build(tmp, 'reference', rdir, defines=['GCM_SPLIT_REFERENCE_BUILD'])
            rc, rout, _ = run(rbin, 'off')
            if rc != 0 or log_lines(rout) != log_lines(off):
                failed += 1
                print('FAIL: the off form differs from the walker before the seam (origin/main)')
                a, b = log_lines(rout), log_lines(off)
                for i, (x, y) in enumerate(zip(a, b)):
                    if x != y: print(f'    first difference at line {i}:\n      main: {x}\n      off:  {y}'); break
            else:
                print(f'PASS off form == origin/main walker ({len(log_lines(rout))} log lines)')
        else:
            print('SKIP off-form comparison with origin/main (not available here or already carries the seam)')

        if args.sanitize:
            flag = f'-fsanitize={args.sanitize}'
            try:
                sbin = build(tmp, 'san-' + args.sanitize, extra=[flag, '-g', '-O1'])
            except subprocess.CalledProcessError:
                # A toolchain without the sanitizer runtime (gcc without libtsan):
                # nothing to check, not a failure of the walker.
                sbin = None
                print(f'SKIP -fsanitize={args.sanitize}: the sanitizer build failed on this toolchain')
            if sbin:
                ok, _, on, err_on = check(sbin, 'san', scale=20)   # a fifth of the workload under instrumentation
                bad = 'WARNING: ThreadSanitizer' in err_on or 'ERROR: AddressSanitizer' in err_on
                if not ok or bad:
                    failed += 1
                    # The first reports carry the stacks; the tail only counts them.
                    first = err_on.find('WARNING:'); first = 0 if first < 0 else max(0, first - 200)
                    print(f'FAIL under -fsanitize={args.sanitize}:\n' + err_on[first:first + 12000] + '\n...\n' + err_on[-600:])
                else:
                    print(f'PASS under -fsanitize={args.sanitize}')

        if args.mutants:
            sources = {'gcm_walker_split.inc': (video / 'gcm_walker_split.inc').read_text(),
                       'cellGcmSys.c': (video / 'cellGcmSys.c').read_text()}
            for name, (which, edits) in MUTANTS.items():
                mutated = sources[which]
                for old, new in edits:
                    assert mutated.count(old) == 1, (name, old)
                    mutated = mutated.replace(old, new)
                mdir = Path(tmp) / f'mutant-{name}'; mdir.mkdir()
                # The walker's quoted include finds the .inc beside it: copy both.
                (mdir / 'cellGcmSys.c').write_text(mutated if which == 'cellGcmSys.c' else sources['cellGcmSys.c'])
                (mdir / 'gcm_walker_split.inc').write_text(mutated if which == 'gcm_walker_split.inc' else sources['gcm_walker_split.inc'])
                try:
                    mbin = build(tmp, f'mutant-{name}', mdir)
                    ok, _, on, err_on = check(mbin, name)
                except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
                    ok = False
                if ok:
                    failed += 1
                    print(f'FAIL: mutant {name} not caught')
                else:
                    print(f'[PASS] mutant {name} caught')
    print('[gcm_split_walker] ' + ('FAIL' if failed else 'PASS'))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
