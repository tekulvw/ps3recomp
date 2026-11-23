#!/usr/bin/env python3
"""The short dispatch entry (PS3RECOMP_SPU_IB_FAST=1, runtime/spu/spu_channels.c).

1. Every environment variable read by spu_indirect_branch up to its call of
   the resolved function, by the dispatch cache and by the dispatch histogram
   must be listed in runtime/spu/spu_fast_path_env.h (spu_ib_fast_env): the
   short entry runs none of them, so it is taken only while none is set.
2. runtime/spu/tests/test_spu_ib_fast.c drives spu_indirect_branch through
   synthetic images with the switch off and on: the function reached is the
   resolver's in both forms; with the switch on a repeated target under an
   unchanged key skips the full entry, and every input the full entry reads
   sends the next dispatch back through it (the full entry is counted through
   a patched source copy).
3. With the switch set AND a listed diagnostic armed, the full entry runs and
   the switch is reported as ignored.
4. --mutants: dropping one key field, the interrupt guard, the taskset guard
   or the reserved-address exclusion from the short entry is caught by 2.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
units = ['spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy',
         'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
base_env = {k: v for k, v in os.environ.items()
            if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_', 'YDKJ_', 'LBP_', 'GH3_'))}
channels = (spu / 'spu_channels.c').read_text()


def body(text, name):
    m = re.search(r'^[A-Za-z_][\w* ]*\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', text, re.M)
    assert m, name
    i = text.index('{', m.start())
    depth, k, instr = 0, i, None
    while k < len(text):
        c = text[k]
        if instr:
            if c == '\\':
                k += 2
                continue
            if c == instr:
                instr = None
        elif text.startswith('//', k):
            k = text.index('\n', k)
            continue
        elif text.startswith('/*', k):
            k = text.index('*/', k) + 2
            continue
        elif c in '"\'':
            instr = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:k + 1]
        k += 1
    raise AssertionError(name)


# 1. the diagnostics list
getenv = re.compile(r'getenv\("([A-Z0-9_]+)"\)')
read = set()
for fn in ('spu_indirect_branch', 'spu_lifted_lookup', 'spu_dcache_mode', 'spu_ibhist_note',
           'spu_verified_leaf_mode', 'spu_overlay_residency_shadow', 'spu_dispatch_kind_on'):
    read |= set(getenv.findall(body(channels, fn)))
env_h = (spu / 'spu_fast_path_env.h').read_text()
listed = set(re.findall(r'"([A-Z0-9_]+)"', env_h[env_h.index('spu_ib_fast_env'):]))
read.discard('SPU_DISPATCH_CACHE')   # required to be exactly 1 instead (spu_fast_path_resolve)
missing = sorted(read - listed)
assert not missing, 'read on the dispatch path but not in spu_ib_fast_env: ' + ', '.join(missing)
print('PASS env list: %d variables read on the dispatch path, all listed (%d listed)' % (len(read), len(listed)))

counter = 'extern unsigned ibf_slow; ++ibf_slow;\n    spu_ibhist_note(ctx);'
assert channels.count('    spu_ibhist_note(ctx);') == 1


def build(tmp, name, source):
    source = source.replace('    spu_ibhist_note(ctx);', '    ' + counter)
    # The copy's quoted includes resolve through -I (runtime/spu and its
    # parent's diagnostics/platform via ../); only the fixture's own include
    # of ../spu_channels.c is redirected to the copy.
    src = Path(tmp) / (name + '_channels.c')
    src.write_text(source)
    fixture = (spu / 'tests/test_spu_ib_fast.c').read_text().replace(
        '#include "../spu_channels.c"', '#include "' + str(src) + '"')
    fx = Path(tmp) / (name + '_fixture.c')
    fx.write_text(fixture)
    binary = Path(tmp) / name
    r = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                        '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                        str(fx), *[str(spu / (u + '.c')) for u in units],
                        str(root / 'runtime/platform/win32_compat.c'),
                        str(root / 'runtime/diagnostics/job_io_recorder.c'),
                        '-pthread', '-lm', '-o', str(binary)], capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, r.stdout + r.stderr[-4000:]
    return binary


def run(binary, **extra):
    r = subprocess.run([str(binary)], env=dict(base_env, **extra), capture_output=True,
                       text=True, timeout=60)
    return r.returncode, r.stdout, r.stderr


with tempfile.TemporaryDirectory(prefix='spu-ib-fast-') as tmp:
    binary = build(tmp, 'ibfast', channels)
    for extra, switch in (({}, 0), ({'PS3RECOMP_SPU_IB_FAST': '1'}, 1)):
        rc, out, err = run(binary, SPU_DISPATCH_CACHE='1', **extra)
        assert rc == 0 and ('IB_FAST switch=%d' % switch) in out and 'failed=0' in out, (extra, out, err[-2000:])
    print('PASS both forms: the resolver\'s function every time; the short entry answers repeated keys only')
    # 3. a listed diagnostic keeps the full entry
    rc, out, err = run(binary, PS3RECOMP_SPU_IB_FAST='1', SPU_DISPATCH_CACHE='1', SPU_IBHIST='1')
    assert rc == 0 and 'IB_FAST switch=0' in out, (out, err[-2000:])
    assert '[spu] PS3RECOMP_SPU_IB_FAST=1 ignored: SPU_IBHIST is set' in err, err[-2000:]
    for dc in ({}, {'SPU_DISPATCH_CACHE': 'shadow'}, {'SPU_DISPATCH_CACHE': '0'}):
        rc, out, err = run(binary, PS3RECOMP_SPU_IB_FAST='1', **dc)
        assert rc == 0 and 'IB_FAST switch=0' in out, (dc, out, err[-2000:])
        assert '[spu] PS3RECOMP_SPU_IB_FAST=1 ignored: SPU_DISPATCH_CACHE is not 1' in err, (dc, err[-2000:])
    print('PASS armed diagnostics: the switch yields to SPU_IBHIST (reported) and needs SPU_DISPATCH_CACHE=1')
    if args.mutants:
        hit = ('e->image == ctx->image_id && e->ovl == ctx->resident_ovl && e->task == ctx->resident_task &&\n'
               '        e->flags == spu_ibf_flags(ctx) && e->residency == ctx->dcache_residency &&')
        controls = {
            'image': (hit, hit.replace('e->image == ctx->image_id && ', '')),
            'overlay': (hit, hit.replace('e->ovl == ctx->resident_ovl && ', '')),
            'task': (hit, hit.replace('e->task == ctx->resident_task &&', '1 &&')),
            'flags': (hit, hit.replace('e->flags == spu_ibf_flags(ctx) && ', '')),
            'residency': (hit, hit.replace('e->residency == ctx->dcache_residency &&', '1 &&')),
            'bytes-epoch': ('e->vtotal == t_dcache.vtotal &&', '1 &&'),
            'registry': ('e->rgen == __atomic_load_n(&s_registry_count, __ATOMIC_ACQUIRE) &&', '1 &&'),
            'lifetime': ('ctx->dcache_id && e->id == ctx->dcache_id &&', '1 &&'),
            'interrupt': ('return !ctx->irq_saved && !s_task_entry_count', 'return !s_task_entry_count'),
            'taskset': ('&& !s_task_entry_count && !s_task_elf_count;', ';'),
            'reserved': ('|| spu_ibf_reserved(pc) ||', '||'),
        }
        for name, (old, new) in controls.items():
            assert channels.count(old) == 1, (name, channels.count(old))
            mutant = build(tmp, 'mutant_' + name, channels.replace(old, new))
            rc, out, err = run(mutant, PS3RECOMP_SPU_IB_FAST='1', SPU_DISPATCH_CACHE='1')
            assert rc != 0 and 'FAIL:' in out, (name, out, err[-1000:])
            print('PASS rejected bug control:', name)
print('PASS short dispatch entry')
