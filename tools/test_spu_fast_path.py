#!/usr/bin/env python3
"""The compact MFC/channel form (PS3RECOMP_MFC_FAST=1).

1. Every environment variable the MFC command path, the DMA observers, the
   channel entries, the overlay note and the lock guard read must be listed in
   runtime/spu/spu_fast_path_env.h: the compact form is taken only while none
   of them is set, so a name missing there would silently lose its diagnostic.
2. runtime/spu/tests/test_spu_fast_path.c runs one DMA sequence (GET/PUT of
   several sizes, refused transfers, a reservation broken by a plain PUT, the
   latched-state reads) in both forms; local store, guest memory, tag status and
   atomic status must digest identically.
3. With the switch set AND a diagnostic armed, the full form runs (the switch
   is reported as ignored) and the diagnostic still prints.
"""
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


getenv = re.compile(r'getenv\("([A-Z0-9_]+)"\)')
read = set()
for f in ('spu_dma.h', 'spu_dma_publication_impl.h', 'spu_dma_source_trace.h', '../output_provenance.h'):
    read |= set(getenv.findall((spu / f).read_text()))
channels = (spu / 'spu_channels.c').read_text()
for fn in ('spu_wrch_full', 'spu_rdch_full', 'spu_rchcnt', 'spu_ls_watch_list',
           'spu_overlay_note_get', 'spu_lockguard_check', 'spu_dcache_miss_stats'):
    read |= set(getenv.findall(body(channels, fn)))
listed = set(re.findall(r'^\s*"([A-Z0-9_]+)",\s*$', (spu / 'spu_fast_path_env.h').read_text(), re.M))
missing = sorted(read - listed)
assert not missing, f'read on the path but not in spu_fast_path_env.h: {missing}'
assert len(listed) >= 50, listed
print(f'PASS env list: {len(read)} variables read on the path, all listed ({len(listed)} listed)')

units = ['spu_drain', 'spu_lockstep', 'spu_coherency', 'spu_workload', 'spurs_policy',
         'spurs_job', 'spu_tsp_weak', 'spu_vm_pagemap', 'spurs_policy_blob_weak', 'spu_interp']
base_env = {k: v for k, v in os.environ.items()
            if not k.startswith(('SPU_', 'SPURS_', 'PS3_', 'PS3RECOMP_', 'YDKJ_', 'LBP_'))}

with tempfile.TemporaryDirectory(prefix='spu-fast-path-') as tmp:
    binary = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-w',
                    '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
                    str(spu / 'tests/test_spu_fast_path.c'), *[str(spu / (u + '.c')) for u in units],
                    str(root / 'runtime/platform/win32_compat.c'),
                    str(root / 'runtime/diagnostics/job_io_recorder.c'),
                    '-pthread', '-lm', '-o', str(binary)], check=True, timeout=300)

    def run(**extra):
        r = subprocess.run([str(binary)], env=dict(base_env, **extra), capture_output=True,
                           text=True, timeout=60, cwd=tmp)
        assert r.returncode == 0, (extra, r.stdout, r.stderr[-4000:])
        return r.stdout.splitlines(), r.stderr

    full, _ = run()
    compact, err = run(PS3RECOMP_MFC_FAST='1')
    assert full[0] == 'fast=0' and compact[0] == 'fast=1', (full[0], compact[0])
    assert full[1] == compact[1] == 'putllc held=0 lost=1', (full[1], compact[1])
    assert full[-1].startswith('digest=') and full[-1] == compact[-1], (full[-1], compact[-1])
    assert 'ignored' not in err, err
    print('PASS both forms: identical local store, guest memory, tag and atomic status', full[-1])

    out, err = run(PS3RECOMP_MFC_FAST='1', SPU_DMA_WATCH='61000')
    assert out[0] == 'fast=0' and out[-1] == full[-1], out
    assert '[spu] PS3RECOMP_MFC_FAST=1 ignored: SPU_DMA_WATCH is set' in err, err
    assert '[dma-watch] pc=0x00000 cmd=0x20 lsa=0x01000 ea=0x00060000 size=16384 covers 0x00061000' in err, err
    out, err = run(PS3RECOMP_MFC_FAST='1', SPU_PUTHIST='1')
    assert out[0] == 'fast=0' and '[put#1]' in err, (out[0], err[-2000:])
    out, err = run(PS3RECOMP_MFC_FAST='1', SPU_CHHIST='1')
    assert out[0] == 'fast=0' and '[chw]' in err and '[chr]' in err, (out[0], err[-2000:])
    out, err = run(PS3RECOMP_MFC_FAST='0')
    assert out[0] == 'fast=0', out
    print('PASS armed diagnostics: the switch yields to SPU_DMA_WATCH, SPU_PUTHIST and SPU_CHHIST, which still report')
