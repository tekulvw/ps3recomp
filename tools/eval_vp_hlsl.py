#!/usr/bin/env python3
"""Evaluate a decompiled RSX vertex program on the CPU for the vertices of one
RSX_ENGINE_VTX_PROBE_FP draw.

  eval_vp_hlsl.py <pipeN.vs.hlsl> <log> [--fp HASH] [--draw N] [--frame F]

The vertex program is the HLSL RSX_ENGINE_SHADER_DUMP wrote for the pipeline
the probe names (pipe<N>.vs.hlsl). Its inputs are the probe's fetched vertices
(the values the GPU was given) and the probe's transform constants (list every
row the program reads, or leave RSX_ENGINE_VTX_PROBE_CONST unset so all
non-zero rows are printed). For each vertex it prints the clip position, the
window position the probe's viewport maps it to, and every other output
register the program wrote -- so a draw that changes no pixels can be told
apart as zero-area, faded out (an alpha varying of 0) or off-screen.

Straight-line programs only: the decompiler's branching form (_pc blocks) is
refused rather than evaluated wrongly.
"""
import argparse
import math
import re
import sys

COMP = "xyzw"


class F4:
    __slots__ = ("c",)

    def __init__(self, *a):
        if len(a) == 1:
            x = a[0]
            if isinstance(x, F4):
                self.c = list(x.c)
            elif isinstance(x, (list, tuple)):
                self.c = [float(v) for v in x]
            else:
                self.c = [float(x)] * 4
        else:
            self.c = [float(v) for v in a]

    def __getattr__(self, name):
        if name and all(ch in COMP for ch in name):
            vals = [self.c[COMP.index(ch)] for ch in name]
            return vals[0] if len(vals) == 1 else F4(vals + [vals[-1]] * (4 - len(vals)))
        raise AttributeError(name)

    def _bin(self, o, f):
        o = o if isinstance(o, F4) else F4(o)
        return F4([f(a, b) for a, b in zip(self.c, o.c)])

    def __add__(self, o): return self._bin(o, lambda a, b: a + b)
    __radd__ = __add__
    def __sub__(self, o): return self._bin(o, lambda a, b: a - b)
    def __rsub__(self, o): return F4(o)._bin(self, lambda a, b: a - b)
    def __mul__(self, o): return self._bin(o, lambda a, b: a * b)
    __rmul__ = __mul__
    def __truediv__(self, o): return self._bin(o, _div)
    def __rtruediv__(self, o): return F4(o)._bin(self, _div)
    def __neg__(self): return F4([-a for a in self.c])
    def __lt__(self, o): return self._bin(o, lambda a, b: float(a < b))
    def __le__(self, o): return self._bin(o, lambda a, b: float(a <= b))
    def __gt__(self, o): return self._bin(o, lambda a, b: float(a > b))
    def __ge__(self, o): return self._bin(o, lambda a, b: float(a >= b))
    def __repr__(self): return "(" + " ".join("%.6g" % v for v in self.c) + ")"


def _div(a, b):
    if b == 0.0:
        return math.copysign(math.inf, a) if a else math.nan
    return a / b


def _map(f, x):
    return F4([f(v) for v in x.c]) if isinstance(x, F4) else f(x)


def vp_mul1(a, b): return 0.0 if (a == 0.0 or b == 0.0) else a * b
def vp_mul(a, b): return F4([vp_mul1(x, y) for x, y in zip(F4(a).c, F4(b).c)])
def vp_dot3(a, b): m = vp_mul(a, b).c; return m[0] + m[1] + m[2]
def vp_dot4(a, b): m = vp_mul(a, b).c; return m[0] + m[1] + m[2] + m[3]
def saturate(x): return _map(lambda v: min(max(v, 0.0), 1.0) if v == v else 0.0, x)
def rsqrt(x): return _map(lambda v: 1.0 / math.sqrt(v), x)
def cos(x): return _map(math.cos, x)
def sin(x): return _map(math.sin, x)
def exp2(x): return _map(lambda v: 2.0 ** v if v < 1024 else math.inf, x)
def log2(x): return _map(lambda v: math.log2(v) if v > 0 else -math.inf, x)
def floor(x): return _map(math.floor, x)
def frac(x): return _map(lambda v: v - math.floor(v), x)
def sign(x): return _map(lambda v: float((v > 0) - (v < 0)), x)


def clamp(x, lo, hi):
    x, lo, hi = F4(x), F4(lo), F4(hi)
    return F4([min(max(a, b), c) for a, b, c in zip(x.c, lo.c, hi.c)])


def hmin(a, b): return F4(a)._bin(b, min) if isinstance(a, F4) or isinstance(b, F4) else min(a, b)
def hmax(a, b): return F4(a)._bin(b, max) if isinstance(a, F4) or isinstance(b, F4) else max(a, b)
def habs(x): return _map(abs, x)
def dot(a, b):
    a, b = F4(a).c, F4(b).c
    return sum(x * y for x, y in zip(a, b))


def compile_body(hlsl):
    m = re.search(r"VSOutput main\(VSInput input\) \{\n(.*?)\n    VSOutput Out;", hlsl, re.S)
    if not m:
        sys.exit("no vertex program body")
    body = m.group(1)
    if "_pc" in body:
        sys.exit("branching vertex program (_pc blocks): not evaluated")
    out = []
    for line in body.splitlines():
        s = line.strip()
        if not s or s.startswith(("float4 v[16]", "[unroll]", "float4 r[32]", "int4 a0", "float4 cc[2]",
                                  "v[", "}")):
            continue
        s = s.lstrip("{ ").rstrip()
        for stmt in [p.strip() for p in s.split(";") if p.strip()]:
            if stmt == "}":
                continue
            mm = re.match(r"(r|o|a0|a1|cc)(\[\d+\])?\.([xyzw]+) = _v\.([xyzw]+)$", stmt)
            if mm:
                tgt = mm.group(1) + (mm.group(2) or "")
                out.append("assign(%s, %r, _v)" % (tgt, mm.group(3)))
                continue
            stmt = stmt.replace("float4 _v = ", "_v = ").replace("(float4)", "F4")
            stmt = stmt.replace("(int4)", "")
            stmt = re.sub(r"\bmin\(", "hmin(", stmt)
            stmt = re.sub(r"\bmax\(", "hmax(", stmt)
            stmt = re.sub(r"\babs\(", "habs(", stmt)
            stmt = re.sub(r"vp_c\[([^\]]+)\]", r"vp_c[int(\1)]", stmt)
            out.append(stmt)
    return compile("\n".join(out), "<vp>", "exec")


def assign(reg, mask, val):
    for ch in mask:
        reg.c[COMP.index(ch)] = val.c[COMP.index(ch)]


def parse_probe(log, fp, draw, frame):
    lines = open(log, errors="replace").read().splitlines()
    starts = []
    for i, l in enumerate(lines):
        m = re.match(r"\[vtx probe\] fp ([0-9a-f]{16}) pipe (\d+) frame (\d+) draw (\d+)", l)
        if m and (fp is None or m.group(1) == fp) and (draw is None or int(m.group(4)) == draw) \
                and (frame is None or int(m.group(3)) == frame):
            starts.append((i, m))
    if not starts:
        sys.exit("no matching probe record")
    i, head = starts[0]
    verts, consts, vp = [], {}, None
    for l in lines[i + 1:]:
        if l.startswith("[vtx probe] fp "):
            break
        mv = re.match(r"\[vtx probe\]  v(\d+) ref (\d+):(.*)", l)
        if mv:
            attrs = {int(a): F4([float(x) for x in vals.split()])
                     for a, vals in re.findall(r" a(\d+)\(([^)]*)\)", mv.group(3))}
            verts.append((int(mv.group(2)), attrs))
            continue
        mc = re.match(r"\[vtx probe\]  c(\d+) \(([^)]*)\)", l)
        if mc:
            consts[int(mc.group(1))] = F4([float(x) for x in mc.group(2).split()])
            continue
        mp = re.match(r"\[vtx probe\]  viewport scale \(([^)]*)\) translate \(([^)]*)\)", l)
        if mp:
            vp = ([float(x) for x in mp.group(1).split()], [float(x) for x in mp.group(2).split()])
    return head, verts, consts, vp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vs")
    ap.add_argument("log")
    ap.add_argument("--fp")
    ap.add_argument("--draw", type=int)
    ap.add_argument("--frame", type=int)
    a = ap.parse_args()
    code = compile_body(open(a.vs).read())
    head, verts, consts, vp = parse_probe(a.log, a.fp, a.draw, a.frame)
    print("probe:", head.group(0))
    vp_c = [consts.get(k, F4(0.0)) for k in range(512)]
    windows = []
    for ref, attrs in verts:
        v = [attrs.get(k, F4(0, 0, 0, 1)) for k in range(16)]
        r = [F4(0.0) for _ in range(32)]
        o = [F4(0, 0, 0, 1) for _ in range(16)]
        env = dict(globals())
        env.update(v=v, r=r, o=o, vp_c=vp_c, a0=F4(0.0), a1=F4(0.0), cc=[F4(0.0), F4(0.0)])
        exec(code, env)
        p = o[0]
        line = "v ref %u: clip %r" % (ref, p)
        if vp and p.c[3]:
            nx, ny, nz = (p.c[0] / p.c[3], p.c[1] / p.c[3], p.c[2] / p.c[3])
            wx, wy = nx * vp[0][0] + vp[1][0], ny * vp[0][1] + vp[1][1]
            windows.append((wx, wy))
            line += " window (%.2f %.2f %.4f)" % (wx, wy, nz * vp[0][2] + vp[1][2])
        print(line)
        for k in range(1, 16):
            if o[k].c != [0.0, 0.0, 0.0, 1.0]:
                print("    o[%d] %r" % (k, o[k]))
    # QUADS (primitive 8): each four consecutive vertices are one quad.
    if " prim 8 " in head.string and len(windows) >= 4:
        for q in range(len(windows) // 4):
            pts = windows[4 * q:4 * q + 4]
            area = 0.5 * abs(sum(pts[i][0] * pts[(i + 1) % 4][1] - pts[(i + 1) % 4][0] * pts[i][1]
                                 for i in range(4)))
            print("quad %d: window area %.1f px" % (q, area))


if __name__ == "__main__":
    main()
