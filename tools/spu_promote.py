"""spu_promote.py - keep SPU registers in C locals across straight-line runs.

The lifter writes every instruction as ``ctx->gpr[D] = helper(ctx->gpr[A], ...)``.
ctx->gpr is a union array in memory, and runtime builds use
-fno-strict-aliasing, so every store to it may alias every later load: the C
compiler reloads each operand from memory, stores each result back, and
cannot see that a shuffle pattern built by ilhu/iohl two lines earlier is a
constant. This pass rewrites each maximal run of instructions that touch
nothing but registers and local store into a block over locals:

    {
        u128 pr3 = ctx->gpr[3];          registers read before written
        u128 pr49;                       registers only written
        pr49 = spu_ai(pr3, 224);
        ...
        ctx->gpr[49] = pr49;             every written register, once
    }

The block's effect on ctx->gpr and local store is the same as the original
lines'. A run ends at anything else: labels (branch targets), branches,
calls, channel instructions, stops, the function's end. Nothing inside a run
can leave the run early -- the helpers it admits do not call out of the
generated code or longjmp -- so no register write is lost to an early exit.
Read-only uses of ctx->gpr by local-store diagnostics (SPU_LS_WATCH logging
r0) may see the values from the start of the run.
"""
from __future__ import annotations

import re

_GPR = re.compile(r"ctx->gpr\[(\d+)\]")
_ASSIGN = re.compile(r"^ctx->gpr\[(\d+)\] = (.*);$")
_STORE = re.compile(r"^spu_ls_write128\(ctx, (.*)\);$")
_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# Helpers that read or change state other than their arguments.
_IMPURE = {"spu_rdch", "spu_rchcnt", "spu_wrch", "spu_halt", "spu_stop",
           "spu_indirect_branch", "spu_drain_call", "spu_unsupported",
           "spu_img_restore", "spu_trace_pc", "spu_trace_rt"}


def _pure_expr(expr: str) -> bool:
    rest = _GPR.sub("R", expr).replace("spu_ls_read128(ctx,", "spu_ls_read128(")
    if re.search(r"\bctx\b|\bg_|[;{}]|(?<![=!<>])=(?!=)", rest):
        return False
    for name in _CALL.findall(rest):
        if not name.startswith("spu_") or name in _IMPURE:
            return False
    return True


def _classify(stmt: str):
    """(reads, write) for a register/local-store-only statement, else None."""
    m = _ASSIGN.match(stmt)
    if m and _pure_expr(m.group(2)):
        return [int(r) for r in _GPR.findall(m.group(2))], int(m.group(1))
    m = _STORE.match(stmt)
    if m and _pure_expr(m.group(1)):
        return [int(r) for r in _GPR.findall(m.group(1))], None
    if stmt == "/* nop */;":
        return [], None
    return None


def _emit_run(run: list[tuple[str, str, list[int], int | None]]) -> list[str]:
    indent = run[0][0]
    order, loaded, written = [], set(), []
    seen = set()
    for _, _, reads, write in run:
        for r in reads:
            if r not in seen:
                seen.add(r); loaded.add(r); order.append(r)
        if write is not None:
            if write not in seen:
                seen.add(write); order.append(write)
            if write not in written:
                written.append(write)
    out = [f"{indent}{{"]
    loads = [f"pr{r} = ctx->gpr[{r}]" for r in order if r in loaded]
    plain = [f"pr{r}" for r in order if r not in loaded]
    for i in range(0, len(loads), 8):
        out.append(f"{indent}    u128 " + ", ".join(loads[i:i + 8]) + ";")
    for i in range(0, len(plain), 12):
        out.append(f"{indent}    u128 " + ", ".join(plain[i:i + 12]) + ";")
    for _, stmt, _, _ in run:
        if stmt == "/* nop */;":
            continue
        out.append(f"{indent}    " + _GPR.sub(lambda m: f"pr{m.group(1)}", stmt))
    for r in sorted(written):
        out.append(f"{indent}    ctx->gpr[{r}] = pr{r};")
    out.append(f"{indent}}}")
    return out


def promote_lines(lines: list[str], min_run: int = 2) -> list[str]:
    """Rewrite every eligible run in a list of C source lines."""
    out: list[str] = []
    run: list = []

    def flush():
        nonlocal run
        if len([r for r in run if r[3] is not None or r[1] != "/* nop */;"]) >= min_run:
            out.extend(_emit_run(run))
        else:
            out.extend(r[0] + r[1] for r in run)
        run = []

    for line in lines:
        stmt = line.strip()
        info = _classify(stmt) if stmt else None
        if info is None:
            flush()
            out.append(line)
            continue
        indent = line[: len(line) - len(line.lstrip())]
        run.append((indent, stmt, info[0], info[1]))
    flush()
    return out
