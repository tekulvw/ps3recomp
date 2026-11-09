#!/usr/bin/env python3
"""spu_lifter.py --promote-regs keeps SPU registers in C locals between sync
points. Lift one neutral synthetic program with and without it and run both
(and, where it applies, the interpreter) through the real runtime: drain,
channels, stop, halt, an interrupt taken and returned through its frame, and a
depth-0 restart (longjmp) into interpreted code. Every architectural result --
register file, local store, pc, status, stop code -- must be identical.

Bug controls (--mutants, also run by default): a promoted build with one store
before a call removed, or one reload after a call removed, must be detected.
"""
import os, pathlib, re, struct, subprocess, sys, tempfile, json
if os.name == 'nt':
    print('SKIP: POSIX fixture: built with cc against the pthread MFC fixture; MinGW has no UBSan runtime')
    raise SystemExit(0)

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPU = ROOT / 'runtime/spu'
COMMON = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
          'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c',
          '../diagnostics/job_io_recorder.c']
BASE = 0x10000

# --- a minimal SPU encoder ---------------------------------------------------
def rr(op, rt=0, ra=0, rb=0): return (op << 21) | (rb << 14) | (ra << 7) | rt
def rrr(op, rt, ra, rb, rc): return (op << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc
def ri10(op, rt, ra, i): return (op << 24) | ((i & 0x3FF) << 14) | (ra << 7) | rt
def ri16(op, rt, i): return (op << 23) | ((i & 0xFFFF) << 7) | rt
def ri18(op, rt, i): return (op << 25) | ((i & 0x3FFFF) << 7) | rt

def il(rt, v): return ri16(0x081, rt, v)
def ila(rt, v): return ri18(0x21, rt, v)
def a(rt, ra, rb): return rr(0x0C0, rt, ra, rb)
def ai(rt, ra, v): return ri10(0x1C, rt, ra, v)
def ceqi(rt, ra, v): return ri10(0x7C, rt, ra, v)
def lqd(rt, ra, disp): return ri10(0x34, rt, ra, disp >> 4)
def stqd(rt, ra, disp): return ri10(0x24, rt, ra, disp >> 4)
def shufb(rt, ra, rb, rc): return rrr(0xB, rt, ra, rb, rc)
def heqi(ra, v): return ri10(0x7F, 0, ra, v)
def stop(code): return code & 0x3FFF
def wrch(ch, rt): return rr(0x10D, rt, ch)
def rdch(rt, ch): return rr(0x00D, rt, ch)
def rchcnt(rt, ch): return rr(0x00F, rt, ch)
def bi(ra, e=False): return rr(0x1A8, 0, ra) | (0x40000 if e else 0)
def bisl(rt, ra): return rr(0x1A9, rt, ra)
def biz(rt, ra): return rr(0x128, rt, ra)
def irete(): return rr(0x1AA) | 0x40000

def program():
    """{offset: word}, function starts, scenario entries (offsets from BASE)."""
    code = {}
    def at(off, *words):
        for i, w in enumerate(words):
            code[off + 4 * i] = w
    def rel(op, rt, frm, to): return ri16(op, rt, (to - frm) >> 2)
    br = lambda frm, to: rel(0x064, 0, frm, to)
    brsl = lambda frm, to: rel(0x066, 0, frm, to)
    brz = lambda r, frm, to: rel(0x040, r, frm, to)
    brnz = lambda r, frm, to: rel(0x042, r, frm, to)
    # A: loop over LS, call, channels, tail transfer, conditional transfers.
    at(0x000, il(10, 5), il(11, 0), ila(12, 0x30000))
    at(0x00C, a(11, 11, 10), lqd(13, 12, 0), shufb(13, 13, 11, 13), stqd(13, 12, 0),
           ai(12, 12, 16), ai(10, 10, -1), brnz(10, 0x024, 0x00C))
    at(0x028, il(3, 7), brsl(0x02C, 0x200), a(6, 3, 4), il(9, 0x55),
           wrch(22, 9), rdch(8, 12), rchcnt(14, 28), a(15, 8, 14), ai(16, 15, 3), br(0x04C, 0x100))
    at(0x100, a(17, 16, 6), ceqi(18, 17, 0x1FF), brnz(18, 0x108, 0x180), brz(18, 0x10C, 0x1C0), stop(0x2000))
    at(0x180, il(19, 1), stop(0x2001))
    at(0x1C0, ai(19, 17, 1), stqd(19, 12, 0x40), stop(0x2002))
    at(0x200, ai(3, 3, 100), il(4, 9), il(5, 11), bi(0))
    # B: a taken halt assertion with a dirty register.
    at(0x300, il(20, 0), ai(21, 20, 77), heqi(20, 0), stop(0x2003))
    # C: indirect call, a callee that changes the caller's registers and
    # returns conditionally, then a computed transfer.
    at(0x400, ila(22, BASE + 0x500), il(23, 5), bisl(0, 22), a(24, 23, 3), ila(25, BASE + 0x480), bi(25))
    at(0x480, ai(26, 24, 1), stop(0x2004))
    at(0x500, il(3, 40), ai(23, 23, 2), il(27, 0), biz(27, 0), bi(0))
    # D: the callee returns to a pc that is no lifted entry: the drain falls
    # back to a depth-0 restart (longjmp) and the interpreter runs on.
    at(0x600, il(28, 3), ai(29, 28, 4), ila(9, BASE + 0x618), brsl(0x60C, 0x680),
           ai(29, 29, 1000), stop(0x2005), a(30, 29, 28), stop(0x2006))
    at(0x680, stqd(9, 1, -16), lqd(0, 1, -16), bi(0))
    # E: interrupts enabled by a transfer; the handler clobbers a register,
    # acknowledges the event and returns; the register file is restored.
    at(0x700, il(31, 21), ila(2, BASE + 0x720), bi(2, e=True))
    at(0x720, a(34, 31, 31), stop(0x2007))
    at(0x780, il(31, 999), ila(32, 0x30100), stqd(31, 32, 0), il(33, 2), wrch(2, 33), irete())
    starts = [0x000, 0x100, 0x180, 0x1C0, 0x200, 0x300, 0x400, 0x480, 0x500, 0x600, 0x680, 0x700, 0x720, 0x780]
    return code, starts

def build_image(code):
    end = max(code) + 4
    words = [code.get(o, 0x40200000) for o in range(0, end, 4)]   # nop padding
    return struct.pack('>%dI' % len(words), *words)

def lift(tmp, image, starts, name, promote, extra=()):
    bounds = []
    ends = starts[1:] + [len(image)]
    for s, e in zip(starts, ends):
        bounds.append({'start': hex(BASE + s), 'end': hex(BASE + e)})
    (tmp / 'bounds.json').write_text(json.dumps(bounds))
    out = tmp / name
    cmd = [sys.executable, str(ROOT / 'tools/spu_lifter.py'), str(tmp / 'program.bin'), '--base', hex(BASE),
           '--functions', str(tmp / 'bounds.json'), '--symbol-prefix', 'promote_test_', '--output', str(out)]
    if promote:
        cmd.append('--promote-regs')
    cmd += list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    for f in (out / 'spu_recomp.c', out / 'spu_recomp.h'):
        f.write_text(f.read_text().replace('../../runtime/spu/', ''))
    return out

def build(tmp, src_dir, exe, extra=()):
    flags = ['-std=gnu17', '-O2', '-UNDEBUG', '-I' + str(ROOT / 'include'), '-I' + str(SPU),
             '-I' + str(ROOT / 'runtime/platform'), '-I' + str(tmp), '-DPROMOTE_REGISTER=promote_test_spu_recomp_register',
             *extra]
    if os.environ.get('UBSAN') == '1':
        flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    r = subprocess.run([os.environ.get('CC', 'cc'), *flags, str(SPU / 'tests/test_spu_promote_regs.c'),
                        *[str(SPU / c) for c in COMMON], str(ROOT / 'runtime/platform/win32_compat.c'),
                        str(src_dir / 'spu_recomp.c'), '-pthread', '-lm', '-o', str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr

SCENARIOS = [  # (entry offset, irq, also compare against the interpreter)
    (0x000, 0, True), (0x300, 0, True), (0x400, 0, True), (0x600, 0, False), (0x700, 1, False)]

def run(exe, entry, mode, irq):
    env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    env['SPU_INTERP_UNLIFTED'] = '1'
    r = subprocess.run([str(exe), hex(BASE + entry), mode, str(irq)], env=env, capture_output=True,
                       text=True, timeout=20)
    m = re.search(r'RESULT (.*) failed=(\d+)', r.stdout)
    assert m and r.returncode == 0, (exe.name, hex(entry), r.stdout, r.stderr[-2000:])
    return m.group(1)

def main():
    code, starts = program()
    image = build_image(code)
    with tempfile.TemporaryDirectory(prefix='spu-promote-') as td:
        tmp = pathlib.Path(td)
        (tmp / 'program.bin').write_bytes(image)
        vector = struct.pack('>I', ri16(0x064, 0, (BASE + 0x780) >> 2))   # br handler, relative to LS 0
        (tmp / 'promote_program.h').write_text(
            '#define PROMOTE_BASE 0x%Xu\n' % BASE +
            'static const unsigned char promote_program[]={' + ','.join(str(x) for x in image) + '};\n' +
            'static const unsigned char promote_vector[]={' + ','.join(str(x) for x in vector) + '};\n')
        plain = lift(tmp, image, starts, 'plain', False)
        promoted = lift(tmp, image, starts, 'promoted', True)
        src = (promoted / 'spu_recomp.c').read_text()
        assert 'spu_pr_v rg' in src and 'spu_pr_lq' in src, 'promotion not applied'
        build(tmp, plain, tmp / 'plain.exe')
        build(tmp, promoted, tmp / 'promoted.exe')
        build(tmp, promoted, tmp / 'promoted_diag.exe', ['-DSPU_LS_DIAG=1'])
        # A register cap mixes promoted and unpromoted functions in one image.
        capped = lift(tmp, image, starts, 'capped', True, ['--promote-max-regs', '4'])
        csrc = (capped / 'spu_recomp.c').read_text()
        nfun = len(re.findall(r'^void promote_test_spu_func_\w+\(spu_context\* ctx\) \{', csrc, re.M))
        npro = csrc.count('uint8_t* ls = ctx->ls;')
        assert 0 < npro < nfun, (npro, nfun)
        build(tmp, capped, tmp / 'capped.exe')
        outcomes = {}
        for entry, irq, oracle in SCENARIOS:
            want = run(tmp / 'plain.exe', entry, 'lift', irq)
            for exe in ('promoted.exe', 'promoted_diag.exe', 'capped.exe'):
                got = run(tmp / exe, entry, 'lift', irq)
                assert got == want, (exe, hex(entry), want, got)
            if oracle:
                # The interpreter reports stops and halts in its own way (the pc
                # of the stop itself, its own halt path); the architectural
                # data -- register file and local store -- must agree.
                ref = run(tmp / 'plain.exe', entry, 'interp', irq)
                strip = lambda o: re.search(r'regs=\w+ ls=\w+', o).group(0)
                assert strip(ref) == strip(want), ('interpreter', hex(entry), ref, want)
            outcomes[entry] = want
            print('PASS scenario 0x%03X%s: %s' % (entry, ' (irq)' if irq else '', want), flush=True)
        stops = {e: re.search(r'stop=(\w+)', o).group(1) for e, o in outcomes.items()}
        assert stops == {0x000: '2002', 0x300: '0', 0x400: '2004', 0x600: '2006', 0x700: '2007'}, stops
        # D ran its tail through the restart (r30 = 7 + 3); E took the
        # interrupt (the handler's store) and returned with r31 restored.
        assert 'r30=10 ' in outcomes[0x600], outcomes[0x600]
        assert 'r34=42 mark=999 ' in outcomes[0x700], outcomes[0x700]
        # Bug controls: drop one store before a call / one reload after it.
        call = re.search(r'(ctx->gpr\[(\d+)\] = SPU_PR_U\(rg\2\); )(?=[^\n]*promote_test_spu_func_00010200\(ctx\))', src)
        # r3 is the callee's result, read right after the call returns.
        reload = re.search(r'promote_test_spu_func_00010200\(ctx\);[^\n]*?spu_img_restore\(ctx, _si\); [^\n]*?(rg3 = SPU_PR_V\(ctx->gpr\[3\]\); )', src)
        assert call and reload, 'bug-control anchors'
        for name, bad in (('store', src.replace(call.group(1), '', 1)),
                          ('reload', src[:reload.start(1)] + src[reload.end(1):])):
            mutant = tmp / ('mutant_' + name)
            mutant.mkdir()
            (mutant / 'spu_recomp.c').write_text(bad)
            (mutant / 'spu_recomp.h').write_text((promoted / 'spu_recomp.h').read_text())
            build(tmp, mutant, tmp / (name + '.exe'))
            detected = any(run(tmp / (name + '.exe'), e, 'lift', irq) != outcomes[e] for e, irq, _ in SCENARIOS)
            assert detected, 'bug control not detected: missing ' + name
            print('PASS rejected bug control: missing', name, flush=True)
    print('PASS promoted lift matches the plain lift and the interpreter')

if __name__ == '__main__':
    main()
