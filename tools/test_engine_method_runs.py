#!/usr/bin/env python3
"""rsx_draw_engine_methods_at (a packet's words as one run) against rsx_draw_engine_method_at
(libs/video/tests/test_engine_method_runs.c, with the draw engine and a stub backend)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the engine fixture uses POSIX linking')
    raise SystemExit(0)
cc = os.environ.get('CC', 'cc')
sources = ['libs/video/tests/test_engine_method_runs.c'] + ['libs/video/' + n + '.c' for n in (
    'rsx_draw_engine', 'rsx_fifo_busy', 'rsx_dispatch', 'rsx_vertex_compact', 'rsx_texture_layout',
    'rsx_vp_decompiler', 'rsx_fp_decompiler')] + ['runtime/diagnostics/render_ablation.c',
    'libs/video/tests/gcm_deferred_fixture_stubs.c']
with tempfile.TemporaryDirectory(prefix='engine-method-runs-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-Iinclude', '-Ilibs/video',
                    *sources, '-pthread', '-lm', '-o', str(binary)], cwd=root, check=True, timeout=300)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3RECOMP_', 'RSX_'))}
    result = subprocess.run([str(binary)], timeout=300, capture_output=True, text=True, env=env)
    print(result.stdout.strip()[-6000:])
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_engine_method_runs failed')
print('[engine_method_runs] PASS')
