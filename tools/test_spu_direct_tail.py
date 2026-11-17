#!/usr/bin/env python3
"""spu_lifter.py --direct-tail: a transfer to another lifted function becomes a
guarded tail call of that function instead of a return to the drain.

The same neutral synthetic program is lifted with and without the flag (and
with --promote-regs) and run through the real runtime: drain, calls,
channels, stop, halt, interrupts, a depth-0 restart and interpreter fallback.
The observable state must match the drain build in every scenario: pc,
status, stop code, steps, host depth, the innermost drain return pc, the
recent-pc ring, the register file and local store.

  - Default output (no flag) is byte-identical to a lift without the option
    code path, and has no guarded transfers.
  - A loop of 2M transfers between two functions completes with every
    transfer admitted as a tail call (no host-stack growth) and the same
    state as the drain build.
  - Every guard trip sends the transfers back to the drain, still with the
    same state: an enabled pending event, the reservation-lost event armed,
    policy-module flow tracing, a stack-reset entry, SPU_TAILRET,
    SPU_PCHIST, step diagnostics, PS3RECOMP_SPU_DIRECT_TAIL=0.
  - With interrupts enabled and no event pending the transfers ARE tail
    calls; an event raised by another thread mid-loop is still delivered.
  - A tail transfer after a nested native call, into code that enters the
    interpreter directly, stops the interpreter at the owning drain's return
    pc (call_ret_pc), as the drain build does; a callee entered from a call
    site that does not keep call_ret (an image lifted without the flag)
    takes the drain.
  - The call-site bookkeeping costs a call-heavy loop with no transfers
    almost nothing (printed as BENCH).
Bug controls (also run by default): a guard that ignores pending events, a
note that skips the step count, a note that does not set the drain return
pc, and a guard that trusts call_ret from another depth must be detected.
"""
import os, pathlib, re, struct, subprocess, sys, tempfile, json, shutil
if os.name == 'nt':
    print('SKIP: POSIX fixture: built with clang against the pthread MFC fixture')
    raise SystemExit(0)

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import test_spu_promote_regs as P   # encoder, base program, image builder

SPU = ROOT / 'runtime/spu'
BASE = P.BASE
CC = os.environ.get('CC', 'clang')

def ceq(rt, ra, rb): return P.rr(0x3C0, rt, ra, rb)
def ilhu(rt, v): return P.ri16(0x082, rt, v)
def iohl(rt, v): return P.ri16(0x0C1, rt, v)

def program():
    code, starts = P.program()
    def at(off, *words):
        for i, w in enumerate(words):
            code[off + 4 * i] = w
    def rel(op, rt, frm, to): return P.ri16(op, rt, (to - frm) >> 2)
    br = lambda frm, to: rel(0x064, 0, frm, to)
    brsl = lambda frm, to: rel(0x066, 0, frm, to)
    brnz = lambda r, frm, to: rel(0x042, r, frm, to)
    # F: 1M laps of a two-function loop: 2M cross-function transfers.
    at(0x7E0, P.il(40, 0), ilhu(41, 0x000F), iohl(41, 0x4240), br(0x7EC, 0x800))
    at(0x800, P.ai(40, 40, 1), ceq(42, 40, 41), brnz(42, 0x808, 0x880), br(0x80C, 0x840))
    at(0x840, br(0x840, 0x800))
    at(0x880, P.stop(0x2010))
    # G: interrupts enabled by the entry transfer, then a two-function loop
    # until the handler's mark appears (or a cap: stop 0x2013).
    at(0x8C0, P.il(40, 0), ilhu(41, 0x02FA), iohl(41, 0xF080), P.ila(2, BASE + 0x900), P.bi(2, e=True))
    at(0x900, P.ai(40, 40, 1), P.ila(45, 0x30100), P.lqd(46, 45, 0), P.il(48, 999), ceq(47, 46, 48),
           brnz(47, 0x914, 0x980), ceq(49, 40, 41), brnz(49, 0x91C, 0x9A0), br(0x920, 0x960))
    at(0x960, br(0x960, 0x900))
    at(0x980, P.stop(0x2012))
    at(0x9A0, P.stop(0x2013))
    # H: G calls F; F makes a nested native call (H returns natively) and
    # then transfers to K; K calls unlifted code directly (bisl -> resolver
    # miss -> interpreter), which runs until the interpreter's stop point --
    # the return pc of the drain that owns K's transfer, 0xA08 -- and then
    # continues through G's code to END. The stop point decides the path.
    fa = lambda rt, ra, rb: P.rr(0x2C4, rt, ra, rb)    # not in the microstep set
    at(0xA00, P.il(40, 1), brsl(0xA04, 0xA40), P.ai(40, 40, 10), br(0xA0C, 0xA20))
    at(0xA20, P.stop(0x2011))
    at(0xA40, brsl(0xA40, 0xA80), br(0xA44, 0xAC0))
    at(0xA80, P.bi(0))
    at(0xAC0, P.ila(43, BASE + 0xB40), P.bisl(0, 43), P.stop(0x2014))
    at(0xB40, fa(50, 50, 50), P.ai(44, 44, 1), br(0xB48, 0xA08))
    # I: call-heavy loop without transfers (the call-site cost of --direct-tail).
    at(0xC00, P.il(40, 0), ilhu(41, 0x0098), iohl(41, 0x9680),
           brsl(0xC0C, 0xC40), P.ai(40, 40, 1), ceq(42, 40, 41), rel(0x040, 42, 0xC18, 0xC0C),
           P.stop(0x2015))
    at(0xC40, P.bi(0))
    starts = sorted(set(starts) | {0x7E0, 0x800, 0x840, 0x880, 0x8C0, 0x900, 0x960, 0x980, 0x9A0,
                                   0xA00, 0xA20, 0xA40, 0xA80, 0xAC0, 0xC00, 0xC40})
    # 0xB40 is deliberately not a function: the interpreter runs it.
    return code, starts

def lift(tmp, image, starts, name, extra=()):
    ends = starts[1:] + [len(image)]
    bounds = [{'start': hex(BASE + s), 'end': hex(BASE + e)} for s, e in zip(starts, ends)]
    (tmp / 'bounds.json').write_text(json.dumps(bounds))
    out = tmp / name
    cmd = [sys.executable, str(ROOT / 'tools/spu_lifter.py'), str(tmp / 'program.bin'), '--base', hex(BASE),
           '--functions', str(tmp / 'bounds.json'), '--symbol-prefix', 'dt_test_', '--output', str(out), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    for f in (out / 'spu_recomp.c', out / 'spu_recomp.h'):
        f.write_text(f.read_text().replace('../../runtime/spu/', ''))
    return out

def build(tmp, src_dir, exe, drain_src=None, first_inc=None):
    common = [str(SPU / c) for c in P.COMMON]
    if drain_src:
        common = [drain_src if c.endswith('/spu_drain.c') else c for c in common]
    flags = (['-I' + str(first_inc)] if first_inc else []) + ['-std=gnu17', '-O2', '-UNDEBUG', '-I' + str(ROOT / 'include'), '-I' + str(SPU),
             '-I' + str(ROOT / 'runtime/platform'), '-I' + str(tmp),
             '-DDIRECT_TAIL_REGISTER=dt_test_spu_recomp_register', '-DSPU_DIRECT_TAIL_TEST_COUNT']
    if os.environ.get('UBSAN') == '1':
        flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    r = subprocess.run([CC, *flags, str(SPU / 'tests/test_spu_direct_tail.c'), *common,
                        str(ROOT / 'runtime/platform/win32_compat.c'), str(src_dir / 'spu_recomp.c'),
                        '-pthread', '-lm', '-o', str(exe)], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr

MUTANT_TIMEOUT = None

def run(exe, entry, opts='none', env_extra=None, stack_kb=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_', 'PS3RECOMP_SPU_DIRECT_TAIL'))}
    env['SPU_INTERP_UNLIFTED'] = '1'
    env.update(env_extra or {})
    cmd = [str(exe), hex(BASE + entry), opts]
    if stack_kb:
        cmd = ['/bin/sh', '-c', 'ulimit -s %d && exec "$@"' % stack_kb, 'sh', *cmd]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True,
                       timeout=MUTANT_TIMEOUT if MUTANT_TIMEOUT and 'mutant' in str(exe) else 120)
    m = re.search(r'RESULT (.*) tail_on=(\d+) tails=(\d+) failed=(\d+)', r.stdout)
    assert m and r.returncode == 0, (exe.name, hex(entry), opts, r.stdout[-2000:], r.stderr[-2000:])
    return m.group(1), int(m.group(2)), int(m.group(3))

# entry, opts, env, expected tail calls: 'some' / 'none' / None (either)
SCENARIOS = [
    (0x000, 'none', {}, None), (0x300, 'none', {}, None), (0x400, 'none', {}, None),
    (0x600, 'none', {}, None), (0x700, 'irq', {}, None), (0xA00, 'none', {}, None),
    (0x7E0, 'none', {}, 'all'),
    # Pending but interrupts disabled: the drain would not deliver, so tails.
    (0x7E0, 'irq', {}, 'all'),
    # Interrupts enabled with an event already pending: delivered at once.
    (0x8C0, 'irq', {}, None),
    (0x7E0, 'lr', {}, 'none'), (0x7E0, 'pmflow', {}, 'none'),
    (0x7E0, 'none', {'SPU_TAILRET': '1'}, 'none'), (0x7E0, 'none', {'SPU_PCHIST': '1'}, 'none'),
    (0x7E0, 'none', {'PS3RECOMP_SPU_DIRECT_TAIL': '0'}, 'none'),
    (0x000, 'none', {'SPU_JOBDRAIN': '1'}, 'none'),
    (0xA00, 'reset', {}, 'none'),
    # F entered from a call site that does not keep call_ret (an image lifted
    # without the flag): its transfer to K must take the drain.
    (0xA40, 'unflagged', {}, 'none'),
    (0xC00, 'none', {}, None),
]

def check_all(tmp, plain, variant, label):
    for entry, opts, env, expect in SCENARIOS:
        want, _, _ = run(plain, entry, opts, env)
        got, on, tails = run(variant, entry, opts, env)
        assert got == want, (label, hex(entry), opts, env, want, got)
        if expect == 'all':
            assert on == 1 and tails >= 2_000_000, (label, hex(entry), on, tails)
        elif expect == 'none':
            assert tails == 0, (label, hex(entry), opts, env, tails)
        print('PASS %s 0x%03X %s %s tails=%d' % (label, entry, opts, env or '', tails), flush=True)
    # Interrupts enabled, no event pending: the loop runs as tail calls, and an
    # event raised by another thread is delivered (handler mark) mid-loop.
    for exe, name in ((plain, 'drain'), (variant, label)):
        got, on, tails = run(exe, 0x8C0, 'raiser')
        assert 'stop=2012 ' in got and 'mark=999 ' in got, (name, got)
        if exe is variant:
            assert tails > 0, (name, 'no tail calls with interrupts enabled', tails)
        print('PASS %s raiser: delivered mid-loop, tails=%d' % (name, tails), flush=True)
    # The 2M-transfer loop runs in a 256 KB stack: tail calls do not nest.
    got, _, tails = run(variant, 0x7E0, 'none', stack_kb=256)
    assert tails >= 2_000_000, tails
    print('PASS %s 2M tail calls in a 256 KB stack' % label, flush=True)

def bench(plain, variant, label):
    """The call-site cost of --direct-tail: 10M calls, no transfers taken."""
    def best(exe):
        times = []
        for _ in range(5):
            env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'PS3RECOMP_SPU_DIRECT_TAIL'))}
            r = subprocess.run([str(exe), hex(BASE + 0xC00), 'bench'], env=env, capture_output=True, text=True, timeout=120)
            times.append(int(re.search(r'BENCH ns=(\d+)', r.stdout).group(1)))
        return min(times) / 10_000_000
    p, v = best(plain), best(variant)
    print('BENCH %s call loop: drain lift %.2f ns/call, %s %.2f ns/call (%+.2f)' % (label, p, label, v, v - p), flush=True)

def main():
    code, starts = program()
    image = P.build_image(code)
    if not shutil.which(CC):
        print('SKIP: %s not found (direct tails need clang musttail)' % CC); return
    with tempfile.TemporaryDirectory(prefix='spu-direct-tail-') as td:
        tmp = pathlib.Path(td)
        (tmp / 'program.bin').write_bytes(image)
        vector = struct.pack('>I', P.ri16(0x064, 0, (BASE + 0x780) >> 2))
        (tmp / 'direct_tail_program.h').write_text(
            '#define DIRECT_TAIL_BASE 0x%Xu\n' % BASE +
            'static const unsigned char direct_tail_program[]={' + ','.join(str(x) for x in image) + '};\n' +
            'static const unsigned char direct_tail_vector[]={' + ','.join(str(x) for x in vector) + '};\n')
        plain = lift(tmp, image, starts, 'plain')
        direct = lift(tmp, image, starts, 'direct', ['--direct-tail'])
        dpromo = lift(tmp, image, starts, 'dpromo', ['--direct-tail', '--promote-regs'])
        psrc, dsrc = (plain / 'spu_recomp.c').read_text(), (direct / 'spu_recomp.c').read_text()
        assert 'SPU_DIRECT_TAIL' not in psrc and 'spu_direct_tail_image' not in psrc
        n_plain = len(re.findall(r'g_spu_trampoline_fn = dt_test_spu_func_', psrc))
        assert dsrc.count('SPU_DIRECT_TAIL_OK(ctx)') == n_plain > 0, (dsrc.count('SPU_DIRECT_TAIL_OK(ctx)'), n_plain)
        assert 'spu_direct_tail_image();' in dsrc
        r = subprocess.run([sys.executable, str(ROOT / 'tools/spu_lifter.py'), str(tmp / 'program.bin'),
                            '--base', hex(BASE), '--functions', str(tmp / 'bounds.json'), '--output',
                            str(tmp / 'rejected'), '--direct-tail', '--connected-regions',
                            '--verified-code-image', '7', '--verified-code-spans'], capture_output=True, text=True)
        assert r.returncode and 'connected-regions' in (r.stdout + r.stderr), 'connected-regions combination accepted'
        print('PASS lifter output: %d guarded transfers, default unchanged, --connected-regions refused' % n_plain)
        build(tmp, plain, tmp / 'plain.exe')
        build(tmp, direct, tmp / 'direct.exe')
        build(tmp, dpromo, tmp / 'dpromo.exe')
        check_all(tmp, tmp / 'plain.exe', tmp / 'direct.exe', 'direct')
        check_all(tmp, tmp / 'plain.exe', tmp / 'dpromo.exe', 'direct+promote')
        pp = lift(tmp, image, starts, 'ppromo', ['--promote-regs'])
        build(tmp, pp, tmp / 'ppromo.exe')
        bench(tmp / 'plain.exe', tmp / 'direct.exe', 'direct')
        bench(tmp / 'ppromo.exe', tmp / 'dpromo.exe', 'direct+promote')
        # Bug controls.
        global MUTANT_TIMEOUT
        MUTANT_TIMEOUT = 20
        hdr = (SPU / 'spu_context.h').read_text()
        drain = (SPU / 'spu_drain.c').read_text()
        def mutant_header(old, new):
            assert hdr.count(old) == 1, old
            d = tmp / 'mutinc'
            if d.exists(): shutil.rmtree(d)
            d.mkdir()
            (d / 'spu_context.h').write_text(hdr.replace(old, new))
            return d
        controls = [
            ('guard ignores pending events', 'header',
             '!((ctx)->int_enable && ((ctx)->event_status & (ctx)->event_mask)) &&', '',
             lambda exe: run(exe, 0x8C0, 'raiser')[0], lambda out: 'stop=2012 ' not in out),
            ('note skips the step count', 'header', '    ctx->steps++;\n    g_spu_pch', '    g_spu_pch',
             lambda exe: run(exe, 0x7E0)[0], lambda out: out != run(tmp / 'plain.exe', 0x7E0)[0]),
            ('note does not set the drain return pc', 'header',
             '    if (ctx->host_depth) ctx->drain_ret_pc = ctx->call_ret_pc;\n', '',
             lambda exe: run(exe, 0xA00)[0], lambda out: out != run(tmp / 'plain.exe', 0xA00)[0]),
            ('guard trusts call_ret from another depth', 'header',
             '(!(ctx)->host_depth || (ctx)->call_ret_depth == (ctx)->host_depth);', '1;',
             lambda exe: run(exe, 0xA40, 'unflagged')[0],
             lambda out: out != run(tmp / 'plain.exe', 0xA40, 'unflagged')[0]),
        ]
        for name, where, old, new, probe, bad in controls:
            exe = tmp / ('mutant_%d.exe' % controls.index((name, where, old, new, probe, bad)))
            if where == 'header':
                inc = mutant_header(old, new)
                build(tmp, direct, exe, first_inc=inc)
            else:
                assert drain.count(old) == 1, old
                mdrain = tmp / 'mutant_drain.c'
                mdrain.write_text(drain.replace(old, new).replace('#include "', '#include "%s/' % SPU))
                build(tmp, direct, exe, str(mdrain))
            try:
                out = probe(exe)
                detected = bad(out)
            except (AssertionError, subprocess.TimeoutExpired):
                detected = True   # a failed run or a livelock (the interpreter-overrun hang)
            assert detected, 'bug control not detected: ' + name
            print('PASS rejected bug control:', name, flush=True)
    print('PASS direct tail calls match the drain in every scenario')

if __name__ == '__main__':
    main()
