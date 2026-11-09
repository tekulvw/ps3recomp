#!/usr/bin/env python3
"""Differential test for the opt-in register-promotion pass (tools/ppu_promote.py).

Hand-written PPU instruction sequences are encoded and decoded, then lifted by the
actual PPULifter. Each function is compiled twice into one driver: the plain lift
and its promoted rewrite (--promote-regs, plus a second build with --inline-stack).
Both run against the same seeded register and memory states.

The callees are stubs. Each stub records the whole context it sees, then
clobbers volatile and sometimes non-volatile state, and sometimes leaves a
trampoline pending. Indirect calls, system calls, and lifted calls under a
condition all reach those stubs.

The driver compares the two versions on:
  * the ordered guest access log;
  * the context at every call;
  * the final context, trampoline and memory.
The test also checks that:
  * mutated promotions are caught: a dropped CR store, a dropped reload, and a
    conditional call treated as always taken;
  * the lifter's default output (flags off) is unchanged by the pass's
    presence.
Synthetic instruction words and values only; no game data.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ppu_disasm import decode                                   # noqa: E402
from ppu_lifter import PPULifter                                # noqa: E402
import ppu_promote                                              # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
EXT = 0x00900000          # external callee
EXT2 = 0x00900100         # tail-call / trampoline target


# ---- encoders -------------------------------------------------------------
def D(op, rt, ra, imm): return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)
def DS(op, rt, ra, ds, xo): return (op << 26) | (rt << 21) | (ra << 16) | (ds & 0xFFFC) | xo
def X(xo, rt, ra, rb, rc=0, op=31): return (op << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc
def SPR(xo, rt, spr): return (31 << 26) | (rt << 21) | (((spr & 31) << 5 | spr >> 5) << 11) | (xo << 1)
def B(at, target, lk=0): return (18 << 26) | ((target - at) & 0x3FFFFFC) | lk
def BC(at, bo, bi, target, lk=0): return (16 << 26) | (bo << 21) | (bi << 16) | ((target - at) & 0xFFFC) | lk
def BCLR(bo, bi, lk=0): return (19 << 26) | (bo << 21) | (bi << 16) | (16 << 1) | lk
def BCCTR(bo, bi, lk=0): return (19 << 26) | (bo << 21) | (bi << 16) | (528 << 1) | lk
def A(xo, frt, fra, frb, frc=0): return (63 << 26) | (frt << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo << 1)

li = lambda rt, v: D(14, rt, 0, v)
addi = lambda rt, ra, v: D(14, rt, ra, v)
lwz = lambda rt, ra, d: D(32, rt, ra, d)
stw = lambda rs, ra, d: D(36, rs, ra, d)
lfs = lambda ft, ra, d: D(48, ft, ra, d)
lfd = lambda ft, ra, d: D(50, ft, ra, d)
stfs = lambda fs, ra, d: D(52, fs, ra, d)
stfd = lambda fs, ra, d: D(54, fs, ra, d)
ld = lambda rt, ra, d: DS(58, rt, ra, d, 0)
std = lambda rs, ra, d: DS(62, rs, ra, d, 0)
stdu = lambda rs, ra, d: DS(62, rs, ra, d, 1)
cmpwi = lambda bf, ra, v: D(11, bf << 2, ra, v)
cmpw = lambda bf, ra, rb: X(0, bf << 2, ra, rb)
add = lambda rt, ra, rb, rc=0: X(266, rt, ra, rb, rc)
addc = lambda rt, ra, rb: X(10, rt, ra, rb)
adde = lambda rt, ra, rb: X(138, rt, ra, rb)
addze = lambda rt, ra: X(202, rt, ra, 0)
mr = lambda ra, rs: X(444, rs, ra, rs)
srawi = lambda ra, rs, sh: X(824, rs, ra, sh)
lwarx = lambda rt, ra, rb: X(20, rt, ra, rb)
stwcx = lambda rs, ra, rb: X(150, rs, ra, rb, 1)
mflr = lambda rt: SPR(339, rt, 8)
mtlr = lambda rs: SPR(467, rs, 8)
mtctr = lambda rs: SPR(467, rs, 9)
mfctr = lambda rt: SPR(339, rt, 9)
mfcr = lambda rt: X(19, rt, 0, 0)
mtcrf = lambda fxm, rs: (31 << 26) | (rs << 21) | (fxm << 12) | (144 << 1)
cror = lambda bt, ba, bb: X(449, bt, ba, bb, op=19)
fadd = lambda t, a, b: A(21, t, a, b)
fmul = lambda t, a, c: A(25, t, a, 0, c)
SC = 0x44000002
BLR = BCLR(20, 0)


def program(start, build):
    """build(at) -> list of (word or callable(addr)->word); returns decoded instructions."""
    words = build(start)
    out = []
    for i, w in enumerate(words):
        at = start + 4 * i
        word = w(at) if callable(w) else w
        ins = decode(word, at)
        assert ins.mnemonic not in ("???", ".word"), hex(word)
        out.append(ins)
    return out


def programs():
    P = {}
    # Frame, callee-saved register, direct call, record compare, merge.
    P[0x10000] = lambda s: [mflr(0), stdu(1, 1, -0x80), std(31, 1, 0x78), std(0, 1, 0x90), mr(31, 3),
                            addi(3, 3, 1), lambda a: B(a, EXT, 1), add(3, 3, 31, 1),
                            lambda a: BC(a, 12, 2, s + 4 * 11), li(4, 5), addi(4, 4, 2),
                            ld(0, 1, 0x90), mtlr(0), ld(31, 1, 0x78), addi(1, 1, 0x80), BLR]
    # Counted loop over memory with carries, an srawi through XER, and CTR read back.
    P[0x10100] = lambda s: [li(5, 6), mtctr(5), li(6, 0), li(8, 0), lwz(7, 4, 0), addc(6, 6, 7),
                            addze(8, 8), addi(4, 4, 4), lambda a: BC(a, 16, 0, s + 16), srawi(9, 6, 3),
                            adde(10, 9, 8), mfctr(11), stw(10, 4, 0), BLR]
    # Conditional indirect call (bngtctrl, taken about half the time), then a value live across it, and a conditional return.
    P[0x10200] = lambda s: [cmpwi(0, 9, 32), mtctr(4), li(5, 7), li(30, 9), BCCTR(4, 1, 1), addi(6, 5, 1),
                            add(7, 30, 6), cmpwi(7, 7, 20), BCLR(12, 30), li(3, 0), stw(7, 4, 8), BLR]
    # FP loads, arithmetic, single and double stores, a call, a value in a non-volatile FPR.
    P[0x10300] = lambda s: [mflr(0), std(0, 1, 0x10), stdu(1, 1, -0x70), lfs(1, 3, 0), lfd(14, 3, 8),
                            fadd(3, 1, 14), stfd(3, 3, 16), fmul(4, 3, 1), stfs(4, 3, 24),
                            lambda a: B(a, EXT, 1), fadd(5, 14, 1), stfd(5, 3, 32), addi(1, 1, 0x70),
                            ld(0, 1, 0x10), mtlr(0), BLR]
    # System call, a conditional tail branch out of the function, and an unconditional one.
    P[0x10400] = lambda s: [li(11, 0x10), mr(29, 3), SC, cmpwi(0, 3, 0), lambda a: BC(a, 4, 2, EXT2),
                            add(3, 3, 29), lambda a: B(a, EXT2)]
    # A reservation loop.
    P[0x10500] = lambda s: [li(6, 0), lwarx(5, 3, 6), addi(5, 5, 1), stwcx(5, 3, 6),
                            lambda a: BC(a, 4, 2, s + 4), mr(3, 5), BLR]
    # CR logic and whole-CR moves around an indirect call through LR.
    P[0x10600] = lambda s: [cmpw(1, 3, 4), cmpwi(6, 5, 0), cror(2, 4, 26), mfcr(7), mtlr(4),
                            BCLR(20, 0, 1), mtcrf(0x80, 7), mfcr(8), add(3, 7, 8), BLR]
    # Stack traffic of every width plus an indexed stack load, around calls.
    P[0x10700] = lambda s: [stdu(1, 1, -0x60), std(3, 1, 0x20), stw(4, 1, 0x28), stfd(1, 1, 0x30),
                            lambda a: B(a, EXT, 1), ld(5, 1, 0x20), lwz(6, 1, 0x28), lfd(2, 1, 0x30),
                            li(7, 0x20), X(21, 8, 1, 7), add(3, 5, 6), add(3, 3, 8), stfd(2, 4, 0),
                            addi(1, 1, 0x60), BLR]
    return P


def lift_all(promote, inline_stack):
    lifter = PPULifter()
    texts = []
    funcs = []
    for start, build in sorted(programs().items()):
        insns = program(start, build)
        funcs.append(lifter.lift_function(insns, start, start + 4 * len(insns)))
    by_addr = {f.start_addr: f for f in funcs}
    addrs = sorted(by_addr)
    index = {a: i for i, a in enumerate(addrs)}
    lifter.promote_regs, lifter.inline_stack = promote, inline_stack
    for f in funcs:
        lines = lifter._function_def_lines(f, by_addr, addrs, index)
        texts.append((f.name, "\n".join(lifter._promoted(lines)).rstrip("\n") + "\n"))
    return lifter, texts


DRIVER = r'''
#include <setjmp.h>
#include <vector>
extern "C" {
uint8_t* vm_base;
int vm_inline_ok, vm_inline_store_ok;
uint32_t ppu_vm_size = 1u << 24, g_barrier_sync_watch, g_null_sweep_hi;
static unsigned char s_bitmap[1u << 15]; static int s_armed = 1; static const uint64_t s_crit = 0;
extern const unsigned char* const g_spu_coh_bitmap = s_bitmap;
extern const int* const g_spu_coh_armed = &s_armed;
extern const uint64_t* const g_vm_critical_range = &s_crit;
PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
}
static uint8_t s_mem[1u << 24];
static std::vector<uint64_t> s_log;
static uint64_t s_rng;
static int s_log_access = 1;
static uint64_t rnd() { s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL; uint64_t x = s_rng;
    x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ULL; return x ^ (x >> 32); }
static uint64_t small() { uint64_t r = rnd(); return (r & 1) ? (r >> 1) % 64 : (uint64_t)(int64_t)(int32_t)(r >> 8); }
static uint32_t mask(uint64_t a) { return (uint32_t)a & ((1u << 24) - 1); }
#define RD(N, T, SW) extern "C" T vm_read##N(uint64_t a) { T v; memcpy(&v, s_mem + mask(a), N / 8); v = SW(v); \
    if (s_log_access) { s_log.push_back(N); s_log.push_back(mask(a)); s_log.push_back(v); } return v; }
#define WR(N, T, SW) extern "C" void vm_write##N(uint64_t a, T v) { T s = SW(v); memcpy(s_mem + mask(a), &s, N / 8); \
    if (s_log_access) { s_log.push_back(100 + N); s_log.push_back(mask(a)); s_log.push_back(v); } }
#define ID(v) (v)
RD(8, uint8_t, ID) RD(16, uint16_t, __builtin_bswap16) RD(32, uint32_t, __builtin_bswap32) RD(64, uint64_t, __builtin_bswap64)
WR(8, uint8_t, ID) WR(16, uint16_t, __builtin_bswap16) WR(32, uint32_t, __builtin_bswap32) WR(64, uint64_t, __builtin_bswap64)
extern "C" uint32_t ppu_load_reserve32(ppu_context* c, uint64_t ea) { uint32_t v = vm_read32(ea);
    c->reserve_addr = mask(ea); c->reserve_value = v; c->reserve_valid = 1; return v; }
extern "C" uint64_t ppu_load_reserve64(ppu_context* c, uint64_t ea) { uint64_t v = vm_read64(ea);
    c->reserve_addr = mask(ea); c->reserve_value = v; c->reserve_valid = 1; return v; }
extern "C" int ppu_store_conditional32(ppu_context* c, uint64_t ea, uint32_t v) {
    int ok = c->reserve_valid && c->reserve_addr == mask(ea) && (rnd() & 3); if (ok) vm_write32(ea, v); c->reserve_valid = 0; return ok; }
extern "C" int ppu_store_conditional64(ppu_context* c, uint64_t ea, uint64_t v) {
    int ok = c->reserve_valid && c->reserve_addr == mask(ea) && (rnd() & 3); if (ok) vm_write64(ea, v); c->reserve_valid = 0; return ok; }
extern "C" uint64_t ppu_timebase_now(void) { return 0; }
static void observe(ppu_context* c, uint64_t tag) {
    s_log.push_back(0xCA11000000000000ULL | tag);
    for (int i = 0; i < 32; i++) s_log.push_back(c->gpr[i]);
    for (int i = 0; i < 32; i++) { uint64_t b; memcpy(&b, &c->fpr[i], 8); s_log.push_back(b); }
    s_log.push_back(c->cr); s_log.push_back(c->xer); s_log.push_back(c->lr); s_log.push_back(c->ctr);
}
static void pending(void* p) { ppu_context* c = (ppu_context*)p; observe(c, 0x77); c->gpr[3] ^= 0x5A5A; }
static void callee(ppu_context* c, uint64_t tag) {
    observe(c, tag);
    for (int i = 0; i <= 12; i++) if (i != 1 && i != 2) c->gpr[i] = small();
    for (int i = 0; i <= 13; i++) c->fpr[i] = (double)(int64_t)small() * 0.5;
    c->cr = (uint32_t)rnd(); c->xer = (uint32_t)rnd() & 0xE0000000u; c->ctr = small(); c->lr = small();
    if (!(rnd() & 3)) for (int i = 14; i < 32; i++) c->gpr[i] = small();
    if (!(rnd() & 3)) c->fpr[14] = 3.25;
    if (!(rnd() & 3)) g_trampoline_fn = pending;
}
extern "C" void ps3_indirect_call(ppu_context* c) { callee(c, 0x100000000ULL | c->ctr); }
extern "C" void ps3_hle_call(unsigned nid, ppu_context* c) { callee(c, 0x200000000ULL | nid); }
extern "C" void lv2_syscall(ppu_context* c) { callee(c, 0x300000000ULL | c->gpr[11]); }
'''


def driver(plain, promoted, inline):
    pre = PPULifter()._preamble_lines()
    pre[0] = pre[0].replace('#include "ppu_recomp.h"', '#include "ppu_recomp.h"')
    src = list(pre) + [ppu_promote.STACK_PREAMBLE, DRIVER]
    names = []
    for (name, p), (_, q) in zip(plain, promoted):
        src.append(p.replace(f"void {name}(", f"void P_{name}(", 1))
        src.append(q.replace(f"void {name}(", f"void Q_{name}(", 1))
        names.append(name)
    # Every function the cases name (callees, tail targets) is a recording stub here;
    # the cases themselves were renamed above.
    import re
    called = sorted(set(re.findall(r"\bfunc_([0-9A-F]{8})\b", "".join(p + q for (_, p), (_, q) in zip(plain, promoted)))))
    src[len(pre) + 1:len(pre) + 1] = [f"void func_{t}(ppu_context* ctx);" for t in called]
    for t in called:
        src.append(f"void func_{t}(ppu_context* ctx) {{ callee(ctx, 0x{t}ULL); }}")
    rows = ",".join(f'{{"{n}", P_{n}, Q_{n}}}' for n in names)
    src.append(r'''
struct Case { const char* name; void (*p)(ppu_context*); void (*q)(ppu_context*); };
static const Case cases[] = {''' + rows + r'''};
struct Outcome { std::vector<uint64_t> log; ppu_context ctx; void* tramp; std::vector<uint8_t> mem; };
static Outcome run(void (*fn)(ppu_context*), uint64_t seed) {
    s_rng = seed; s_log.clear(); g_trampoline_fn = 0;
    for (size_t i = 0; i < sizeof s_mem; i += 8) { uint64_t v = (i * 0x9E3779B97F4A7C15ULL) ^ seed; memcpy(s_mem + i, &v, 8); }
    static ppu_context c; memset(&c, 0, sizeof c);
    for (int i = 0; i < 32; i++) c.gpr[i] = small();
    for (int i = 0; i < 32; i++) c.fpr[i] = (double)(int64_t)small() * 0.25;
    for (int i = 0; i < 32; i++) { c.vr[i].d[0] = rnd(); c.vr[i].d[1] = rnd(); }
    c.gpr[1] = 0x00F00000 + (rnd() & 0xFF0); c.gpr[3] = 0x00100000 + (rnd() & 0xFF0); c.gpr[4] = 0x00200000 + (rnd() & 0xFF0);
    c.cr = (uint32_t)rnd(); c.xer = (uint32_t)rnd() & 0xE0000000u; c.lr = 0xABCD00; c.ctr = small();
    fn(&c);
    Outcome o; o.log = s_log; o.ctx = c; o.tramp = (void*)g_trampoline_fn;
    o.mem.assign(s_mem + 0x000F0000, s_mem + 0x00F10000);
    return o;
}
int main(int argc, char** argv) {
    vm_base = s_mem;
    int inline_mode = argc > 1 && argv[1][0] == '1';
    vm_inline_ok = vm_inline_store_ok = inline_mode;
    s_log_access = !inline_mode;
    for (uint32_t l = 0x00F00000u >> 7; l < 0x00F01000u >> 7; l += 3) s_bitmap[l >> 3] |= 1u << (l & 7);
    int bad = 0, runs = 0;
    for (const Case& k : cases) for (uint64_t seed = 1; seed <= 400; seed++) {
        Outcome a = run(k.p, seed), b = run(k.q, seed); runs++;
        if (a.log != b.log || memcmp(&a.ctx, &b.ctx, sizeof a.ctx) || a.tramp != b.tramp || a.mem != b.mem) {
            if (bad < 10) fprintf(stderr, "MISMATCH %s seed %llu\n", k.name, (unsigned long long)seed);
            bad++;
        }
    }
    printf("[ppu-promote] runs=%d mismatches=%d inline=%d\n", runs, bad, inline_mode);
    return bad ? 1 : 0;
}
''')
    return "\n".join(src)


def compile_and_run(source, work, tag, inline):
    work = Path(work)
    cpp, exe = work / f"{tag}.cpp", work / f"{tag}"
    cpp.write_text(source)
    hdr = work / "ppu_recomp.h"
    if not hdr.exists():
        lifter = PPULifter()
        hdr.write_text(lifter.emit_header())
    cxx = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++")
    if not cxx:
        raise SystemExit("A C++ compiler is required; no silent skip.")
    subprocess.run([cxx, "-std=c++17", "-O1", "-w", "-ffp-contract=off", "-I", str(work), str(cpp), "-o", str(exe)],
                   check=True)
    return subprocess.run([str(exe), "1" if inline else "0"], capture_output=True, text=True)


def main():
    if os.name == "nt":
        raise SystemExit("test_ppu_promote.py builds with clang/gcc; run it on macOS or Linux "
                         "(the pass itself is host-independent)")
    failures = 0
    _, plain = lift_all(False, False)
    # Flags off: the lifter output is the plain output.
    assert all("ctx->gpr[" in text for _, text in plain)
    with tempfile.TemporaryDirectory(prefix="ppu-promote-") as work:
        for inline in (False, True):
            _, promoted = lift_all(True, inline)
            assert all(p != q for (_, p), (_, q) in zip(plain, promoted)), "a case was not promoted"
            result = compile_and_run(driver(plain, promoted, inline), work, f"promote{int(inline)}", inline)
            print(result.stdout.strip())
            if result.returncode:
                print(result.stderr.strip())
                failures += 1
        # Bug controls: each mutation must be detected.
        controls = {
            "dropped CR store": ("_store_text", lambda orig: (lambda m: orig(m & ~(1 << 64)))),
            "dropped r3 reload": ("_load_text", lambda orig: (lambda m: orig(m & ~(1 << 3)))),
            "conditional call treated as taken": ("_sync_clears", lambda orig: (lambda line: True)),
        }
        for label, (name, mutate) in controls.items():
            original = getattr(ppu_promote, name)
            setattr(ppu_promote, name, mutate(original))
            try:
                _, mutated = lift_all(True, False)
            finally:
                setattr(ppu_promote, name, original)
            result = compile_and_run(driver(plain, mutated, False), work, "control", False)
            caught = result.returncode != 0
            print(f"control ({label}): {'caught' if caught else 'NOT CAUGHT'}")
            failures += not caught
    print("test_ppu_promote:", "FAILED" if failures else "ok")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
