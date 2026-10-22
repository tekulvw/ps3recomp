#!/usr/bin/env python3
"""spu_promote (spu_lifter.py --promote-registers): the promoted form of a
lifted body must leave ctx->gpr and local store exactly as the original does.

Random straight-line programs in the lifter's statement forms -- integer,
float, shuffle and local-store reads and writes over a small register set,
split by impure statements and labels -- are compiled both ways against the
runtime helpers and run from the same random state; every register and every
local-store byte must match. Also checks which statements the pass admits.
"""
import os, random, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from spu_promote import promote_lines, _classify

REGS = 12
OPS = [
    'spu_a({a}, {b})', 'spu_sf({a}, {b})', 'spu_and({a}, {b})', 'spu_xor({a}, {b})',
    'spu_ai({a}, {i10})', 'spu_andi({a}, {i10})', 'spu_ceqi({a}, {i10})', 'spu_ceq({a}, {b})',
    'spu_cgt({a}, {b})', 'spu_shli({a}, {k})', 'spu_rotqbyi({a}, {k})', 'spu_rotmai({a}, -{k})',
    'spu_ilhu({u16})', 'spu_iohl({a}, {u16})', 'spu_ila({u18})', 'spu_il({i10})',
    'spu_fm({a}, {b})', 'spu_fa({a}, {b})', 'spu_fma({a}, {b}, {c})', 'spu_fnms({a}, {b}, {c})',
    'spu_shufb({a}, {b}, {c})', 'spu_selb({a}, {b}, {c})', 'spu_cflts({a}, 155)', 'spu_csflt({a}, 155)',
    'spu_ls_read128(ctx, {a}._u32[0] + 0x{off:X})', 'spu_ls_read128(ctx, 0x{lsa:X})',
]


def program(rng, n):
    reg = lambda: f'ctx->gpr[{rng.randrange(REGS)}]'
    lines = []
    for i in range(n):
        r = rng.random()
        f = dict(a=reg(), b=reg(), c=reg(), i10=rng.randrange(-512, 512), k=rng.randrange(0, 32),
                 u16=rng.randrange(0, 65536), u18=rng.randrange(0, 1 << 18), off=rng.randrange(0, 0x400) * 16,
                 lsa=rng.randrange(0, 0x4000) * 16)
        if r < 0.05:
            lines.append(f'loc_{i:08X}:')
        elif r < 0.10:
            lines.append(f'    if ({reg()}._u32[0] & 1) ctx->pc = 0x{i:X};')
        elif r < 0.20:
            lines.append(f'    spu_ls_write128(ctx, {reg()}._u32[0] + 0x{f["off"]:X}, {reg()});')
        elif r < 0.22:
            lines.append('    /* nop */;')
        else:
            lines.append(f'    ctx->gpr[{rng.randrange(REGS)}] = ' + rng.choice(OPS).format(**f) + ';')
    return lines


def admits():
    ok = ['ctx->gpr[3] = spu_ai(ctx->gpr[4], 1);', 'ctx->gpr[3] = spu_ls_read128(ctx, ctx->gpr[4]._u32[0] + 0x10);',
          'spu_ls_write128(ctx, ctx->gpr[4]._u32[0], ctx->gpr[5]);', '/* nop */;']
    no = ['ctx->gpr[3] = spu_rdch(ctx, SPU_RdDec);', 'spu_wrch(ctx, MFC_LSA, ctx->gpr[3]);',
          'ctx->gpr[0] = spu_link(0x10); spu_func_00000010(ctx);', 'ctx->pc = 0x10;',
          'ctx->gpr[3] = spu_ai(ctx->gpr[4], ctx->image_id);', 'ctx->gpr[3] = foo(ctx->gpr[4]);',
          'if (ctx->gpr[3]._u32[0] == 0) goto loc_00000010;', 'loc_00000010:']
    for s in ok:
        assert _classify(s) is not None, s
    for s in no:
        assert _classify(s) is None, s


def main():
    admits()
    rng = random.Random(1234)
    progs = [program(rng, 400) for _ in range(24)]
    src = ['#include "spu_context.h"', '#include "spu_helpers.h"', '#include <stdio.h>', '#include <stdlib.h>',
           '#include <string.h>']
    runs = 0
    for k, p in enumerate(progs):
        promoted = promote_lines(p)
        runs += sum(1 for l in promoted if l.strip() == '{')
        for name, body in (('o', p), ('p', promoted)):
            src.append(f'__attribute__((noinline)) static void f{name}{k}(spu_context* ctx) {{')
            src += body
            src.append('}')
    src.append('static void (*fo[])(spu_context*) = {' + ', '.join(f'fo{k}' for k in range(len(progs))) + '};')
    src.append('static void (*fp[])(spu_context*) = {' + ', '.join(f'fp{k}' for k in range(len(progs))) + '};')
    src.append('''
int g_spu_ls_watch_n;   /* the local-store watch diagnostics stay off */
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)a; (void)w; (void)p; (void)pc; (void)lr; }
static uint64_t s = 88172645463325252ull;
static uint32_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; }
int main(void)
{
    spu_context* a = calloc(1, sizeof *a); spu_context* b = calloc(1, sizeof *b);
    int bad = 0;
    for (unsigned k = 0; k < sizeof fo / sizeof fo[0]; k++)
        for (int trial = 0; trial < 8; trial++) {
            spu_context_init(a, 0);
            for (int r = 0; r < 128; r++) for (int w = 0; w < 4; w++) {
                uint32_t v = rnd();
                if (w == 0 && (rnd() & 1)) v &= 0x3FFF0;                  /* plausible addresses */
                if ((rnd() & 3) == 0) v = (v & 0x807FFFFFu) | 0x3F800000u; /* plausible floats   */
                a->gpr[r]._u32[w] = v;
            }
            for (size_t i = 0; i < SPU_LS_SIZE; i += 4) { uint32_t v = rnd(); memcpy(&a->ls_store[i], &v, 4); }
            memcpy(b, a, sizeof *a); b->ls = b->ls_store;
            fo[k](a); fp[k](b);
            if (memcmp(a->gpr, b->gpr, sizeof a->gpr) || memcmp(a->ls_store, b->ls_store, SPU_LS_SIZE) || a->pc != b->pc) {
                printf("MISMATCH program %u trial %d\\n", k, trial); bad++;
            }
        }
    printf("%s\\n", bad ? "FAIL" : "PASS");
    return bad != 0;
}''')
    with tempfile.TemporaryDirectory(prefix='spu-promote-') as td:
        c = Path(td) / 't.c'; exe = Path(td) / 't'
        c.write_text('\n'.join(src) + '\n')
        cc = os.environ.get('CC', 'cc')
        q = subprocess.run([cc, '-O2', '-std=gnu17', '-fno-strict-aliasing', '-w', '-I' + str(ROOT / 'runtime/spu'),
                            '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'runtime'), str(c), '-o', str(exe)],
                           capture_output=True, text=True)
        assert q.returncode == 0, q.stderr[-4000:]
        q = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
        print(q.stdout.strip().splitlines()[-1], f'({len(progs)} programs, {runs} promoted runs)')
        assert q.returncode == 0, q.stdout[-2000:]


if __name__ == '__main__':
    main()
