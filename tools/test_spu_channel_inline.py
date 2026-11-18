#!/usr/bin/env python3
"""spu_lifter.py --inline-channels (runtime/spu/spu_channel_inline.h).

One synthetic program issues every channel operation the option inlines (the
six MFC parameter latches, the tag mask, the tag-status, tag-mask and
atomic-status reads) and the ones it must leave as calls (MFC_Cmd GET/PUT,
tag update, GETLLAR/PUTLLC, list-stall status, event mask, mailbox, machine
status, rchcnt). It is lifted four ways, {plain, --promote-regs} x {without,
with --inline-channels}, and run through the real channel entries, MFC engine
and drain under each switch state:

1. Without the option the output is byte-identical to the lifter on main.
2. Every lift leaves the same register file, local store, guest memory, MFC
   latches, statuses and mailbox, with the switch unset, 0 and 1.
3. With the switch set and a channel diagnostic armed (SPU_CHHIST, SPU_DMA_WATCH)
   the inline form yields (reported as ignored) and the diagnostic sees the same
   events: the per-channel write and read histograms of the inline lift equal
   the plain lift's, latch writes included.
4. The promoted inline form has no register store or reload in its inline arm
   and keeps the store in its call arm.

Bug controls (always run): the guard dropped (the diagnostic then misses the
latch writes), a latch stored into the wrong field (in the generated code and
in the shared header), the tag mask dropped, a status read from the wrong
field, and the call arm's register store removed (the lifter refuses it).
"""
import os, pathlib, re, shutil, struct, subprocess, sys, tempfile, json
if os.name == 'nt':
    print('SKIP: POSIX fixture: built with cc against the pthread MFC fixture')
    raise SystemExit(0)

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPU = ROOT / 'runtime/spu'
COMMON = ['spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c', 'spu_workload.c', 'spurs_policy.c',
          'spurs_job.c', 'spu_tsp_weak.c', 'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c',
          '../diagnostics/job_io_recorder.c']
BASE = 0x10000
SRC_EA, DST_EA, LINE_EA = 0x20000, 0x30000, 0x38000   # within ila's 18-bit immediate
SWITCH = 'PS3RECOMP_SPU_CHANNEL_INLINE'

# --- a minimal SPU encoder ---------------------------------------------------
def rr(op, rt=0, ra=0, rb=0): return (op << 21) | (rb << 14) | (ra << 7) | rt
def ri10(op, rt, ra, i): return (op << 24) | ((i & 0x3FF) << 14) | (ra << 7) | rt
def ri16(op, rt, i): return (op << 23) | ((i & 0xFFFF) << 7) | rt
def ri18(op, rt, i): return (op << 25) | ((i & 0x3FFFF) << 7) | rt
def il(rt, v): return ri16(0x081, rt, v)
def ila(rt, v): return ri18(0x21, rt, v)
def lqd(rt, ra, disp): return ri10(0x34, rt, ra, disp >> 4)
def stop(code): return code & 0x3FFF
def wrch(ch, rt): return rr(0x10D, rt, ch)
def rdch(rt, ch): return rr(0x00D, rt, ch)
def rchcnt(rt, ch): return rr(0x00F, rt, ch)

LSA, EAH, EAL, SIZE, TAG, CMD, WRMASK, WRUPD, RDSTAT, RDSTALL, RDATOM, RDMASK = 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 12
WREVMASK, RDEVMASK, RDMACH, WROUTMBOX = 1, 11, 13, 28
GET, PUT, GETLLAR, PUTLLC, UPDATE_ALL = 0x40, 0x20, 0xD0, 0xB4, 2

def program():
    words = [
        # GET 0x100 bytes from SRC_EA into LS 0x1000 on tag 3.
        il(3, 0x1000), il(4, 0), ila(5, SRC_EA), il(6, 0x100), il(7, 3), il(8, GET),
        wrch(LSA, 3), wrch(EAH, 4), wrch(EAL, 5), wrch(SIZE, 6), wrch(TAG, 7), wrch(CMD, 8),
        il(9, 1 << 3), wrch(WRMASK, 9), il(10, UPDATE_ALL), wrch(WRUPD, 10),
        rdch(11, RDSTAT), rdch(12, RDMASK), rchcnt(13, RDSTAT), rchcnt(14, CMD),
        ila(16, 0x1000), lqd(15, 16, 0),
        # PUT the same bytes to DST_EA on tag 5.
        ila(17, DST_EA), wrch(LSA, 16), wrch(EAL, 17), il(18, 5), wrch(TAG, 18), il(19, PUT), wrch(CMD, 19),
        il(20, 1 << 5), wrch(WRMASK, 20), wrch(WRUPD, 10), rdch(21, RDSTAT),
        # Reserve a line and store it back conditionally.
        ila(22, LINE_EA), ila(23, 0x3000), wrch(LSA, 23), wrch(EAL, 22), il(24, GETLLAR), wrch(CMD, 24),
        rdch(25, RDATOM), il(26, PUTLLC), wrch(CMD, 26), rdch(27, RDATOM),
        # A tag latched last, so the final tag shows its five low bits (0x23 -> 3).
        il(7, 0x23), wrch(TAG, 7),
        # The channels that stay calls.
        rdch(28, RDSTALL), il(29, 3), wrch(WREVMASK, 29), rdch(30, RDEVMASK), wrch(WROUTMBOX, 11), rdch(31, RDMACH),
        stop(0x2000),
    ]
    return struct.pack('>%dI' % len(words), *words)

def lift(tmp, image, name, promote, inline, lifter=None):
    (tmp / 'bounds.json').write_text(json.dumps([{'start': hex(BASE), 'end': hex(BASE + len(image))}]))
    out = tmp / name
    cmd = [sys.executable, str(lifter or ROOT / 'tools/spu_lifter.py'), str(tmp / 'program.bin'), '--base', hex(BASE),
           '--functions', str(tmp / 'bounds.json'), '--symbol-prefix', 'channel_test_', '--output', str(out)]
    if promote:
        cmd.append('--promote-regs')
    if inline:
        cmd.append('--inline-channels')
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stdout + r.stderr
    for f in (out / 'spu_recomp.c', out / 'spu_recomp.h'):
        f.write_text(f.read_text().replace('../../runtime/spu/', ''))
    return out, (out / 'spu_recomp.c').read_text()

def build(tmp, src_dir, exe, spu=SPU):
    flags = ['-std=gnu17', '-O2', '-UNDEBUG', '-w', '-I' + str(ROOT / 'include'), '-I' + str(spu),
             '-I' + str(ROOT / 'runtime/platform'), '-I' + str(tmp),
             '-DCHANNEL_REGISTER=channel_test_spu_recomp_register']
    r = subprocess.run([os.environ.get('CC', 'cc'), *flags, str(spu / 'tests/test_spu_channel_inline.c'),
                        *[str(spu / c) for c in COMMON], str(ROOT / 'runtime/platform/win32_compat.c'),
                        str(src_dir / 'spu_recomp.c'), '-pthread', '-lm', '-o', str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr

BASE_ENV = {k: v for k, v in os.environ.items()
            if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_', 'YDKJ_', 'LBP_', 'GH3_'))}

def run(exe, **env):
    r = subprocess.run([str(exe), hex(BASE)], env=dict(BASE_ENV, **env), capture_output=True, text=True, timeout=30)
    m = re.search(r'RESULT (.*) failed=(\d+)', r.stdout)
    assert m and r.returncode == 0 and m.group(2) == '0', (exe.name, env, r.stdout, r.stderr[-3000:])
    return m.group(1), r.stderr

def histograms(err):
    """The last SPU_CHHIST write and read reports: {channel: count} each."""
    out = {}
    for tag, kind in (('chw', 'wrch'), ('chr', 'rdch')):
        blocks = re.findall(r'\[%s\] \d+ (?:writes|reads):\n((?:   %s ch\d+ +\d+\n)+)' % (tag, kind), err)
        assert blocks, (tag, err[-2000:])
        out[tag] = {int(c): int(n) for c, n in re.findall(r'ch(\d+) +(\d+)', blocks[-1])}
    return out

def strip_words(result):
    return re.sub(r'inline=-?\d fast=-?\d ', '', result)

def main():
    image = program()
    with tempfile.TemporaryDirectory(prefix='spu-chan-inline-') as td:
        tmp = pathlib.Path(td)
        (tmp / 'program.bin').write_bytes(image)
        (tmp / 'channel_program.h').write_text(
            '#define CHANNEL_BASE 0x%Xu\n#define CHANNEL_SRC_EA 0x%Xu\n#define CHANNEL_LINE_EA 0x%Xu\n' % (BASE, SRC_EA, LINE_EA) +
            'static const unsigned char channel_program[]={' + ','.join(str(x) for x in image) + '};\n')
        lifts = {}
        for promote in (False, True):
            for inline in (False, True):
                name = ('promoted' if promote else 'plain') + ('_inline' if inline else '')
                out, src = lift(tmp, image, name, promote, inline)
                assert out, src
                lifts[name] = (out, src)

        # 1. The option off is the lifter on main, byte for byte.
        ref = tmp / 'spu_lifter_main.py'
        main_text = subprocess.run(['git', '-C', str(ROOT), 'show', 'origin/main:tools/spu_lifter.py'],
                                   capture_output=True, text=True)
        if main_text.returncode == 0:
            ref.write_text(main_text.stdout)
            shutil.copy(ROOT / 'tools/spu_disasm.py', tmp / 'spu_disasm.py')
            for promote in (False, True):
                out, src = lift(tmp, image, 'ref_' + ('promoted' if promote else 'plain'), promote, False, lifter=ref)
                assert out, src
                mine = lifts['promoted' if promote else 'plain'][1]
                assert src == mine, 'the lift without --inline-channels differs from main (promote=%s)' % promote
            print('PASS off form: byte-identical to the lifter on origin/main')
        else:
            print('SKIP off-form comparison: no origin/main in this checkout')

        # 4. The generated text.
        src = lifts['plain_inline'][1]
        for name in ('MFC_LSA', 'MFC_EAH', 'MFC_EAL', 'MFC_Size', 'MFC_TagID', 'MFC_WrTagMask'):
            assert re.search(r'if \(spu_channel_inline_on\(\)\) spu_mfc_latch_write\(ctx, %s, ctx->gpr\[\d+\]\); '
                             r'else \{ spu_wrch\(ctx, %s, ctx->gpr\[\d+\]\); \}' % (name, name), src), name
            assert not re.search(r'^\s*spu_wrch\(ctx, %s,' % name, src, re.M), name
        for name in ('MFC_RdTagStat', 'MFC_RdTagMask', 'MFC_RdAtomicStat'):
            assert re.search(r'if \(spu_channel_inline_on\(\)\) ctx->gpr\[\d+\] = spu_rdch_latched\(ctx, %s\); '
                             r'else \{ ctx->gpr\[\d+\] = spu_rdch\(ctx, %s\); \}' % (name, name), src), name
        for call in ('spu_wrch(ctx, MFC_Cmd,', 'spu_wrch(ctx, MFC_WrTagUpdate,', 'spu_wrch(ctx, SPU_WrEventMask,',
                     'spu_wrch(ctx, SPU_WrOutMbox,', 'spu_rdch(ctx, MFC_RdListStallStat)', 'spu_rdch(ctx, SPU_RdEventMask)',
                     'spu_rdch(ctx, SPU_RdMachStat)', 'spu_rchcnt(ctx, MFC_RdTagStat)', 'spu_rchcnt(ctx, MFC_Cmd)'):
            assert call in src and 'spu_channel_inline_on()) ' + call not in src, call
        psrc = lifts['promoted_inline'][1]
        assert 'spu_pr_v rg' in psrc, 'promotion not applied'
        inline_arms = re.findall(r'if \(spu_channel_inline_on\(\)\) ([^;]*;) else \{ ((?:ctx->gpr\[\d+\] = SPU_PR_U\(rg\d+\); )*)([^}]*)\}', psrc)
        assert len(inline_arms) == 13 + 5, len(inline_arms)   # 13 latch writes, 5 status reads
        for arm, stores, call in inline_arms:
            assert 'ctx->gpr' not in arm, arm
            assert stores, 'the call arm lost its register store: ' + call
        for line in psrc.splitlines():
            if 'spu_channel_inline_on()' in line:
                assert 'ctx->gpr' not in line.split('}', 1)[1], 'reload after an inline channel op: ' + line
        print('PASS generated text: 18 inline arms without register traffic, 18 call arms with their stores, 9 calls kept')

        exes = {}
        for name, (out, _) in lifts.items():
            exes[name] = tmp / (name + '.exe')
            build(tmp, out, exes[name])

        # 2. Every lift, every switch state: the same architectural result.
        want, _ = run(exes['plain'])
        assert want.startswith('inline=0 fast=0 '), want
        assert ' stat=8 ' in want or ' stat=20 ' in want, want
        words = {}
        for name, exe in exes.items():
            for env in ({}, {SWITCH: '0'}, {SWITCH: '1'}, {SWITCH: '1', 'PS3RECOMP_MFC_FAST': '1'}, {'PS3RECOMP_MFC_FAST': '1'}):
                got, err = run(exe, **env)
                assert strip_words(got) == strip_words(want), (name, env, want, got)
                words[(name, tuple(sorted(env.items())))] = got.split(' ')[0]
                assert 'ignored' not in err, (name, env, err)
        assert words[('plain_inline', ((SWITCH, '1'),))] == 'inline=1', words
        assert words[('promoted_inline', ((SWITCH, '1'),))] == 'inline=1', words
        assert words[('plain_inline', ((SWITCH, '0'),))] == 'inline=0', words
        assert words[('plain_inline', ())] == 'inline=0', words
        assert ' r11=8 r12=8 r13=1 ' in want and ' r21=20 ' in want and ' tag=3 ' in want and ' r15=0 ' not in want and ' r25=0 ' not in want and ' mbox=1/8 ' in want, want
        print('PASS all four lifts, switch unset/0/1, with and without MFC_FAST:', want)

        # 3. Armed diagnostics: the inline form yields and the histograms agree.
        plain_h = histograms(run(exes['plain'], SPU_CHHIST='1')[1])
        assert plain_h['chw'][16] == 3 and plain_h['chw'][18] == 3 and plain_h['chw'][20] == 3 and plain_h['chw'][22] == 2, plain_h
        assert plain_h['chr'][24] == 2 and plain_h['chr'][12] == 1 and plain_h['chr'][27] == 2, plain_h
        for name in ('plain_inline', 'promoted_inline'):
            for diag in ({'SPU_CHHIST': '1'}, {'SPU_CHHIST': '1', 'PS3RECOMP_MFC_FAST': '1'}):
                got, err = run(exes[name], **{SWITCH: '1'}, **diag)
                assert got.startswith('inline=0 fast=0 '), (name, got)
                assert '[spu] %s=1 ignored: SPU_CHHIST is set' % SWITCH in err, err[-1500:]
                assert histograms(err) == plain_h, (name, diag, histograms(err), plain_h)
            got, err = run(exes[name], **{SWITCH: '1', 'SPU_DMA_WATCH': '%x' % (DST_EA + 0x40)})
            assert got.startswith('inline=0 ') and '[dma-watch]' in err and 'ignored: SPU_DMA_WATCH' in err, (got, err[-1500:])
        print('PASS armed diagnostics: SPU_CHHIST and SPU_DMA_WATCH force the call arm and see the same events')

        # Bug controls.
        def mutant(name, src, header=None):
            d = tmp / ('mutant_' + name); d.mkdir()
            (d / 'spu_recomp.c').write_text(src)
            (d / 'spu_recomp.h').write_text((lifts['promoted_inline'][0] / 'spu_recomp.h').read_text())
            spu = SPU
            if header:
                # The header is reached through spu_context.h's own directory,
                # so the mutant needs its own copy of the runtime tree.
                shutil.copytree(ROOT / 'runtime', tmp / ('runtime_' + name), ignore=shutil.ignore_patterns('tests', '*.o'))
                spu = tmp / ('runtime_' + name) / 'spu'; (spu / 'tests').mkdir()
                for f in ('test_spu_channel_inline.c', 'test_spu_mfc_slots.c'):
                    shutil.copy(SPU / 'tests' / f, spu / 'tests' / f)
                h = spu / 'spu_channel_inline.h'
                old, new = header
                assert h.read_text().count(old) == 1, old
                h.write_text(h.read_text().replace(old, new))
            exe = tmp / (name + '.exe')
            build(tmp, d, exe, spu)
            return exe
        p = lifts['promoted_inline'][1]
        # a. Guard dropped: the inline arm runs with a diagnostic armed.
        exe = mutant('guard', p.replace('if (spu_channel_inline_on()) ', 'if (1) '))
        got, err = run(exe, **{SWITCH: '1', 'SPU_CHHIST': '1'})
        assert strip_words(got) == strip_words(want), got
        assert histograms(err) != plain_h, 'bug control not detected: guard dropped'
        print('PASS rejected bug control: guard dropped (the histogram misses the latch writes)')
        # b. A latch into the wrong field, in the generated code and in the header.
        bad = p.replace('spu_mfc_latch_write(ctx, MFC_EAL, ', 'spu_mfc_latch_write(ctx, MFC_EAH, ')
        assert bad != p
        got, _ = run(mutant('field', bad), **{SWITCH: '1'})
        assert strip_words(got) != strip_words(want), 'bug control not detected: wrong latch field'
        got, _ = run(mutant('header', p, ('case MFC_EAL:       ctx->mfc_eal = v;', 'case MFC_EAL:       ctx->mfc_eah = v;')), **{SWITCH: '1'})
        assert strip_words(got) != strip_words(want), 'bug control not detected: header latch field'
        got, _ = run(mutant('header_off', p, ('case MFC_EAL:       ctx->mfc_eal = v;', 'case MFC_EAL:       ctx->mfc_eah = v;')))
        assert strip_words(got) != strip_words(want), 'the header is not the single definition: the call path did not use it'
        print('PASS rejected bug controls: a latch stored into the wrong field (generated code; header, both arms)')
        # c. The tag mask dropped.
        got, _ = run(mutant('tagmask', p, ('ctx->mfc_tag = v & 0x1F;', 'ctx->mfc_tag = v;')), **{SWITCH: '1'})
        assert strip_words(got) != strip_words(want), 'bug control not detected: tag mask dropped'
        # d. A status read from the wrong field.
        got, _ = run(mutant('status', p, ('case MFC_RdTagStat:    *out = ctx->mfc_tag_status;', 'case MFC_RdTagStat:    *out = ctx->atomic_stat;')), **{SWITCH: '1'})
        assert strip_words(got) != strip_words(want), 'bug control not detected: wrong status field'
        print('PASS rejected bug controls: tag mask dropped, status read from the wrong field')
        # e. The call arm's register store removed: the lifter refuses the text.
        lifter = (ROOT / 'tools/spu_lifter.py').read_text()
        for old in ('else {{ {_F}spu_wrch(ctx, {name}, {g(_reg(ops[1]))}); }}', 'else {{ {_F}{g(rt())} = spu_rdch(ctx, {name}); }}'):
            assert lifter.count(old) == 1, old
            bad_lifter = tmp / 'spu_lifter_bad.py'
            bad_lifter.write_text(lifter.replace(old, old.replace('{_F}', '')))
            shutil.copy(ROOT / 'tools/spu_disasm.py', tmp / 'spu_disasm.py')
            out, msg = lift(tmp, image, 'bad_' + str(len(old)), True, True, lifter=bad_lifter)
            assert out is None and 'unflushed call' in msg, msg[-800:]
        print('PASS rejected bug control: a call arm without its register store is refused by the lifter')
    print('PASS inline channel form matches the plain lift under every switch state and diagnostic')

if __name__ == '__main__':
    main()
