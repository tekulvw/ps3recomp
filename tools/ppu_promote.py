#!/usr/bin/env python3
"""Opt-in PPU lift passes: guest registers in C locals, and inline stack accesses.

Lifted functions keep every guest register in the escaped `ppu_context`. Each
out-of-line guest load or store, and each call, is opaque to the compiler, so it
has to store every register result to `ctx` and load every operand back from it.
This pass rewrites one lifted function at a time so the registers it touches
live in function-scope locals:

  GPRs   ctx->gpr[N] -> uint64_t _gN       FPRs  ctx->fpr[N] -> double _fN
  CR     ctx->cr     -> uint32_t _cr       XER   ctx->xer    -> uint32_t _xer
  LR     ctx->lr     -> uint64_t _lr       CTR   ctx->ctr    -> uint64_t _ctr

Vector registers, FPSCR, VSCR and the reservation fields stay in `ctx`.

`ctx` is the truth wherever something other than this function can look at it.
In lifted code that happens only at the sync points:
  * a call: a lifted function `func_X(ctx)`, `ps3_indirect_call(ctx)`, each with
    its DRAIN_TRAMPOLINE, `lv2_syscall(ctx)` or `ps3_hle_call(nid, ctx)`;
  * a `return` (a plain or conditional return, a tail trampoline, a halt path).
Before a sync point the pass stores the registers that may differ from `ctx`,
found by a forward may-dirty analysis. After a call it reloads the registers
that may be read later, found by a backward liveness analysis that also counts
the later stores. Everything else (labels, gotos, the switch of a jump table) is
ordinary control flow over the locals. Guest loads and stores do not touch
`ctx`. Neither do the reservation helpers (ppu_load_reserve*, which only set
ctx->reserve_*). So none of them is a sync point.

A function is left exactly as it is when the pass cannot prove its rewrite
covers every use of the context:
  * any other use of `ctx` (an unknown helper taking ctx, a hook or trace
    object);
  * a preprocessor line (a hook guard);
  * nonlocal-return machinery (C++ try/catch);
  * a register indexed by anything but a literal;
  * a body matching a caller's --skip-pattern.
Nothing that executes is reordered. The guest loads and stores, the calls and
their order are those of the input text. Only where register values live
changes.

`--inline-stack` additionally rewrites r1-relative guest loads and stores
(`vm_readN(_g1 + d)`, `vm_writeN(_g1 + d, v)` and the indexed `(_g1 + _gK)`
forms) to inline helpers. These take the gate of the runtime's fast
accessors: no read/store diagnostic armed, an address in [0x10000, 0xE0000000)
inside the mapping, and for a store no dynamic store watch and no
coherence-marked line (a store crossing a 128-byte line goes out of line).
The diagnostic flags are sampled at entry and after each call. Anything else
falls back to the out-of-line accessor unchanged.

One known difference: a float moved by lfs/stfs through a promoted local may
keep a signaling NaN's payload where the plain lift's host conversion quiets
it (PPC keeps it). See docs/ppu-promote.md.

Default lifter output is not affected. The lifter applies this only when
asked (--promote-regs / --inline-stack). `tools/ppu_promote.py SRC OUT` applies
it to an already generated directory after any splices and hooks. It skips the
functions that provenance.json names as spliced or hooked, and every body the
pass rejects.
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

FUNC = re.compile(r'^void (\w*func_[0-9A-F]{8})\(ppu_context\* ctx\) \{\n(.*?)^\}\n', re.M | re.S)

# Variable numbering: gpr 0-31, fpr 32-63, cr 64, xer 65, lr 66, ctr 67.
SPECIAL = {'cr': 64, 'xer': 65, 'lr': 66, 'ctr': 67}
NAMES = [f'_g{i}' for i in range(32)] + [f'_f{i}' for i in range(32)] + ['_cr', '_xer', '_lr', '_ctr']
CFIELD = ([f'ctx->gpr[{i}]' for i in range(32)] + [f'ctx->fpr[{i}]' for i in range(32)]
          + ['ctx->cr', 'ctx->xer', 'ctx->lr', 'ctx->ctr'])
CTYPE = ['uint64_t'] * 32 + ['double'] * 32 + ['uint32_t', 'uint32_t', 'uint64_t', 'uint64_t']

# One scan over a body line, left to right.
SCAN = re.compile(
    r'(?P<call>(?:\b\w*func_[0-9A-F]{8}\(ctx\);|\bps3_indirect_call\(ctx\);)(?:\s*DRAIN_TRAMPOLINE\(ctx\);)?'
    r'|\blv2_syscall\(ctx\);|\bps3_hle_call\(0x[0-9A-Fa-f]+u, ctx\);)'
    r'|(?P<ret>\breturn;)'
    r'|(?P<reg>ctx->(?:gpr|fpr)\[(?P<idx>\d+)\]|ctx->(?P<spr>cr|xer|lr|ctr)\b)'
    r'|(?P<semi>;)')
ASSIGN_AFTER = re.compile(r'\s*(?:(?P<plain>=(?!=))|(?P<comp>(?:<<|>>|[-+*/%&|^])=)|(?P<inc>\+\+|--))')
GOTO = re.compile(r'\bgoto (loc_[0-9A-F]{8});')
LABEL = re.compile(r'^\s*(loc_[0-9A-F]{8}):')
# ctx uses that are not register state and need no sync.
ALLOWED_CTX = re.compile(r'ctx->(?:vr|fpscr|vscr|reserve_addr|reserve_value|reserve_valid)\b'
                         r'|\bppu_(?:load_reserve(?:32|64)|store_conditional(?:32|64))\(ctx, ')
STACK_ACCESS = re.compile(r'\bvm_(read|write)(8|16|32|64)\(\(?_g1 \+ ')

PREAMBLE_MARK = '/* ppu_promote: inline stack accessors */'
STACK_PREAMBLE = PREAMBLE_MARK + r'''
/* r1-relative guest loads and stores (tools/ppu_promote.py --inline-stack). The
 * gate is the runtime fast accessors' gate: no diagnostic armed, an ordinary
 * address inside the mapping and, for a store, no dynamic store watch and no
 * coherence-marked line. Everything else takes the out-of-line accessor. */
#ifdef __cplusplus
extern "C" {
#endif
extern int      vm_inline_ok;
extern int      vm_inline_store_ok;
extern uint32_t ppu_vm_size;
extern uint32_t g_barrier_sync_watch;
extern uint32_t g_null_sweep_hi;
extern const unsigned char* const g_spu_coh_bitmap;
extern const int* const g_spu_coh_armed;
extern const uint64_t* const g_vm_critical_range;
#ifdef __cplusplus
}
#endif
/* The diagnostic gates, vm_base and the coherence bitmap pointers are sampled
 * into a function local at entry and after every call (`_stk`), so a run of
 * stack accesses does not reload them through every guest store; a watch armed
 * by another thread is seen at this thread's next call. The coherence armed
 * flag and the line's bitmap bit are still read on every store, and a store
 * that crosses a 128-byte line takes the out-of-line path. */
typedef struct ppu_stk_state { uint32_t ld, st; uint8_t* base; const unsigned char* bitmap; const int* armed; } ppu_stk_state;
static inline ppu_stk_state ppu_stk_gate(void)
{
    const uint32_t lim = (ppu_vm_size == 0 || ppu_vm_size > 0xE0000000u) ? 0xE0000000u : ppu_vm_size;
    ppu_stk_state s;
    s.ld = vm_inline_ok ? lim : 0;
    s.st = (vm_inline_store_ok && !g_barrier_sync_watch && !g_null_sweep_hi && !*g_vm_critical_range) ? lim : 0;
    s.base = vm_base; s.bitmap = g_spu_coh_bitmap; s.armed = g_spu_coh_armed;
    return s;
}
#define PPU_STK_LOAD(N, T, SWAP) \
static inline T ppu_stk_read##N(const ppu_stk_state* g, uint64_t a) { \
    const uint32_t ea = (uint32_t)a; \
    if (__builtin_expect(ea >= 0x10000u && (uint64_t)ea + N / 8 <= g->ld, 1)) { \
        T v; memcpy(&v, g->base + ea, N / 8); return SWAP(v); } \
    return vm_read##N(a); }
#define PPU_STK_STORE(N, T, SWAP) \
static inline void ppu_stk_write##N(const ppu_stk_state* g, uint64_t a, T v) { \
    const uint32_t ea = (uint32_t)a, l = ea >> 7; \
    if (__builtin_expect(ea >= 0x10000u && (uint64_t)ea + N / 8 <= g->st && (ea & 127u) + N / 8 <= 128u \
                         && !(*g->armed && ((g->bitmap[l >> 3] >> (l & 7)) & 1u)), 1)) { \
        T s = SWAP(v); memcpy(g->base + ea, &s, N / 8); return; } \
    vm_write##N(a, v); }
#define PPU_STK_NOSWAP(v) (v)
PPU_STK_LOAD(8, uint8_t, PPU_STK_NOSWAP)
PPU_STK_LOAD(16, uint16_t, __builtin_bswap16)
PPU_STK_LOAD(32, uint32_t, __builtin_bswap32)
PPU_STK_LOAD(64, uint64_t, __builtin_bswap64)
PPU_STK_STORE(8, uint8_t, PPU_STK_NOSWAP)
PPU_STK_STORE(16, uint16_t, __builtin_bswap16)
PPU_STK_STORE(32, uint32_t, __builtin_bswap32)
PPU_STK_STORE(64, uint64_t, __builtin_bswap64)
#undef PPU_STK_LOAD
#undef PPU_STK_STORE
'''


class Reject(Exception):
    """The body uses the context in a way the pass does not model."""


def _var(m):
    return int(m['idx']) + (32 if m['reg'].startswith('ctx->fpr') else 0) if m['idx'] else SPECIAL[m['spr']]


def _bits(mask):
    i = 0
    while mask:
        if mask & 1:
            yield i
        mask >>= 1
        i += 1


class Line:
    """One body line: its rewritten pieces and its ordered events.

    Events: ('use', v) | ('def', v, kills) | ('sync', k) | ('ret', k).
    Pieces: text, or ('sync', k, call_text) / ('ret', k) placeholders."""
    __slots__ = ('pieces', 'events', 'cond', 'gotos', 'label', 'term', 'defs', 'uses')

    def __init__(self, text, counter):
        s = text.strip()
        self.label = None
        m = LABEL.match(text)
        if m:
            self.label = m.group(1)
        # Anything that may skip part of the line makes its sync points and stores conditional.
        self.cond = bool(re.search(r'\b(?:if|switch|while|for)\b|\?|&&|\|\|', s))
        self.gotos = GOTO.findall(text)
        # A loop or a condition makes an assignment conditional; it never kills.
        straight = not re.search(r'\b(?:for|while|if|switch)\b|&&|\|\|', s)
        self.pieces, self.events = [], []
        pending = []  # plain assignments: their def lands at the end of the statement
        pos = 0
        for m in SCAN.finditer(text):
            if m['semi']:
                for v in pending:
                    self.events.append(('def', v, straight))
                pending = []
                continue
            self.pieces.append(text[pos:m.start()])
            pos = m.end()
            if m['call']:
                counter[0] += 1
                self.events.append(('sync', counter[0]))
                self.pieces.append(('sync', counter[0], m['call']))
            elif m['ret']:
                counter[0] += 1
                self.events.append(('ret', counter[0]))
                self.pieces.append(('ret', counter[0]))
            else:
                v = _var(m)
                self.pieces.append(NAMES[v])
                after = ASSIGN_AFTER.match(text, m.end())
                before = text[max(0, m.start() - 2):m.start()]
                if before.endswith(('++', '--')) or (before.endswith('&') and not before.endswith('&&')):
                    self.events += [('use', v), ('def', v, False)]
                elif after and after['plain']:
                    pending.append(v)
                elif after and (after['comp'] or after['inc']):
                    self.events += [('use', v), ('def', v, False)]
                else:
                    self.events.append(('use', v))
        self.pieces.append(text[pos:])
        for v in pending:
            self.events.append(('def', v, straight))
        self.defs = 0
        self.uses = 0
        for e in self.events:
            if e[0] == 'def':
                self.defs |= 1 << e[1]
            elif e[0] == 'use':
                self.uses |= 1 << e[1]
        code = re.sub(r'/\*.*?\*/', '', s).strip()
        self.term = (not self.cond and ('return;' in code or code.startswith('goto ')
                                        or bool(re.match(r'^\{ g_trampoline_fn = [^;]*; return; \}$', code))))


def _store_text(mask):
    """C that writes the locals in `mask` back to the context."""
    return ' '.join(f'{CFIELD[v]} = {NAMES[v]};' for v in _bits(mask))


def _load_text(mask):
    """C that reloads the locals in `mask` from the context."""
    return ' '.join(f'{NAMES[v]} = {CFIELD[v]};' for v in _bits(mask))


def _sync_clears(line):
    """After a sync point every local equals the context again, unless the
    call sat under a condition and may not have run."""
    return not line.cond


def promote_body(body, inline_stack=False):
    """Rewrite one lifted function body (the text between its braces).

    Returns the new body, or raises Reject. A body with no register use is
    returned unchanged."""
    if re.search(r'^\s*#', body, re.M) or 'ps3recomp_nonlocal' in body or '_nonlocal_' in body \
            or re.search(r'\btry\b', body):
        raise Reject('hook, guard or nonlocal machinery')
    if re.search(r'ctx->(?:gpr|fpr)\[(?!\d+\])', body) or re.search(r'ctx->(?:gpr|fpr)\b(?!\[)', body):
        raise Reject('register indexed by a non-literal')
    if re.search(r'\b_(?:g|f)\d+\b|\b_(?:cr|xer|lr|ctr|stk)\b', body):
        raise Reject('local name collision')
    raw = body.split('\n')
    counter = [0]
    lines = [Line(t, counter) for t in raw]
    # Anything left that names ctx must be an allowed non-register use.
    for ln in lines:
        rest = ''.join(p for p in ln.pieces if isinstance(p, str))
        rest = ALLOWED_CTX.sub('', rest)
        if re.search(r'\bctx\b', rest):
            raise Reject('unmodelled use of ctx: ' + rest.strip()[:120])
    used = 0
    for ln in lines:
        used |= ln.uses | ln.defs
    if not used:
        return body
    n = len(lines)
    labels = {ln.label: i for i, ln in enumerate(lines) if ln.label}
    succ = []
    for i, ln in enumerate(lines):
        s = [labels[g] for g in ln.gotos if g in labels]
        if not ln.term and i + 1 < n:
            s.append(i + 1)
        succ.append(s)
    pred = [[] for _ in range(n)]
    for i, s in enumerate(succ):
        for j in s:
            pred[j].append(i)

    # Forward may-dirty: registers whose local may differ from ctx.
    def forward(ln, din, record=None):
        d = din
        for e in ln.events:
            if e[0] == 'def':
                d |= 1 << e[1]
            elif e[0] in ('sync', 'ret'):
                if record is not None:
                    record[e[1]] = d
                if e[0] == 'sync' and _sync_clears(ln):
                    d = 0
        return d if _sync_clears(ln) else (din | ln.defs | d)

    din = [0] * n
    dout = [None] * n
    work = list(range(n))
    inwork = [True] * n
    while work:
        i = work.pop()
        inwork[i] = False
        o = forward(lines[i], din[i])
        if o != dout[i]:
            dout[i] = o
            for j in succ[i]:
                nd = din[j] | o
                if nd != din[j] or dout[j] is None:
                    din[j] = nd
                    if not inwork[j]:
                        inwork[j] = True
                        work.append(j)
    stores = {}
    for i, ln in enumerate(lines):
        forward(ln, din[i], stores)

    # Backward liveness; a store at a sync point reads its registers.
    def backward(ln, lout, record=None):
        live = lout
        for e in reversed(ln.events):
            if e[0] == 'use':
                live |= 1 << e[1]
            elif e[0] == 'def':
                if e[2]:
                    live &= ~(1 << e[1])
            elif e[0] == 'sync':
                if record is not None:
                    record[e[1]] = live
                live = (live | stores[e[1]]) if ln.cond else stores[e[1]]
            else:  # ret
                live = (live | stores[e[1]]) if ln.cond else stores[e[1]]
        return live

    lin = [0] * n
    lout = [0] * n
    work = list(range(n))
    inwork = [True] * n
    while work:
        i = work.pop()
        inwork[i] = False
        o = 0
        for j in succ[i]:
            o |= lin[j]
        lout[i] = o
        li = backward(lines[i], o)
        if li != lin[i]:
            lin[i] = li
            for p in pred[i]:
                if not inwork[p]:
                    inwork[p] = True
                    work.append(p)
    reloads = {}
    for i, ln in enumerate(lines):
        backward(ln, lout[i], reloads)

    store_text, load_text = _store_text, _load_text
    out = []
    entry = lin[0] if n else 0
    stack = inline_stack and bool(STACK_ACCESS.search('\n'.join(''.join(p for p in ln.pieces if isinstance(p, str)) for ln in lines)))
    if stack:
        out.append('    ppu_stk_state _stk = ppu_stk_gate();')
    for v in _bits(used):
        init = f' = {CFIELD[v]}' if entry >> v & 1 else ''
        out.append(f'    {CTYPE[v]} {NAMES[v]}{init};')
    for ln in lines:
        text = []
        for p in ln.pieces:
            if isinstance(p, str):
                text.append(p)
            elif p[0] == 'sync':
                st, ld = store_text(stores[p[1]]), load_text(reloads[p[1]])
                if stack:
                    ld = (ld + ' ' if ld else '') + '_stk = ppu_stk_gate();'
                text.append('{ ' + (st + ' ' if st else '') + p[2] + (' ' + ld if ld else '') + ' }')
            else:
                st = store_text(stores[p[1]])
                text.append('{ ' + st + ' return; }' if st else 'return;')
        out.append(''.join(text))
    result = '\n'.join(out)
    if stack:
        result = STACK_ACCESS.sub(lambda m: f'ppu_stk_{m.group(1)}{m.group(2)}(&_stk, ' + m.group(0).split('(', 1)[1], result)
    return result


def promote_function(text, inline_stack=False):
    """`void func_X(ppu_context* ctx) {\\n<body>}\\n` -> rewritten text (or raises Reject)."""
    m = FUNC.fullmatch(text)
    if not m:
        raise Reject('not a lifted function definition')
    body = m.group(2)
    trailing_newline = body.endswith('\n')
    new = promote_body(body[:-1] if trailing_newline else body, inline_stack)
    return text[:m.start(2)] + new + ('\n' if trailing_newline else '') + '}\n'


def add_preamble(source, inline_stack):
    if not inline_stack or PREAMBLE_MARK in source:
        return source
    m = re.search(r'^(?:/\* [^\n]* \*/\n)?void \w*func_[0-9A-F]{8}\(ppu_context\* ctx\) \{$', source, re.M)
    at = m.start() if m else len(source)
    return source[:at] + STACK_PREAMBLE + '\n' + source[at:]


def transform_source(source, inline_stack=False, skip=frozenset(), stats=None, skip_pattern=None):
    """Rewrite every eligible function of one lifted source file."""
    stats = stats if stats is not None else {}
    parts, pos = [], 0
    for m in FUNC.finditer(source):
        parts.append(source[pos:m.start()])
        pos = m.end()
        text = m.group(0)
        if m.group(1) in skip or (skip_pattern and skip_pattern.search(text)):
            stats['skipped'] = stats.get('skipped', 0) + 1
            parts.append(text)
            continue
        try:
            new = promote_function(text, inline_stack)
            stats['promoted' if new != text else 'unchanged'] = stats.get('promoted' if new != text else 'unchanged', 0) + 1
            parts.append(new)
        except Reject as exc:
            reason = str(exc).split(':')[0]
            stats['rejected: ' + reason] = stats.get('rejected: ' + reason, 0) + 1
            parts.append(text)
    parts.append(source[pos:])
    return add_preamble(''.join(parts), inline_stack)


def provenance_skips(record):
    """Functions earlier transforms of this directory own: splices and hooks."""
    names = set()
    for splice in record.get('function_splices', []):
        names.update(splice.get('changed', []))
        names.update(splice.get('added', []))

    # Render/controls observer stages: {'functions': {stage: ['XXXXXXXX', ...]}}. Their
    # bodies also carry #if guards, which the pass rejects on its own.
    hooks = record.get('render_adapter_hooks', {}).get('functions', {})
    for addresses in hooks.values():
        names.update(a if a.startswith('func_') else 'func_' + a for a in addresses)
    return names


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('source', type=Path, help='generated PPU directory (ppu_recomp_*.cpp)')
    ap.add_argument('output', type=Path, help='new directory to write')
    ap.add_argument('--inline-stack', action='store_true', help='also inline r1-relative guest loads and stores')
    ap.add_argument('--skip', action='append', default=[], help='function name to leave unchanged (repeatable)')
    ap.add_argument('--skip-pattern', type=re.compile, help='leave every function whose text matches this regex')
    a = ap.parse_args(argv)
    if a.output.exists():
        raise SystemExit('output must not exist')
    record_path = a.source / 'provenance.json'
    record = json.loads(record_path.read_text()) if record_path.is_file() else None
    skip = set(a.skip) | (provenance_skips(record) if record else set())
    a.output.mkdir(parents=True)
    stats = {}
    before = {}
    for path in sorted(a.source.iterdir()):
        if not path.is_file():
            continue
        target = a.output / path.name
        if re.fullmatch(r'ppu_recomp_\d{3}\.(?:cpp|c)', path.name):
            before[path.name] = sha256(path)
            target.write_text(transform_source(path.read_text(), a.inline_stack, skip, stats, a.skip_pattern))
        else:
            shutil.copy2(path, target)
    if record is not None:
        record['outputs'] = {p.name: sha256(p) for p in sorted(a.output.iterdir())
                             if p.suffix in ('.c', '.cpp', '.h')}
        record.setdefault('transforms', []).append(dict(
            tool='tools/ppu_promote.py', tool_sha256=sha256(Path(__file__)),
            options=dict(promote_regs=True, inline_stack=a.inline_stack,
                         skip_pattern=a.skip_pattern.pattern if a.skip_pattern else None),
            skipped_functions=sorted(skip), stats=stats, inputs=before))
        (a.output / 'provenance.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(stats, indent=1, sort_keys=True))


if __name__ == '__main__':
    main()
