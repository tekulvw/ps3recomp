#!/usr/bin/env python3
"""PS3RECOMP_RSX_PRESENT_SKIP_UNCHANGED: unchanged presents are skipped, changed ones shown
(libs/video/tests/test_engine_present_skip.c, with the draw engine and a stub backend)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
if not cc or os.name == 'nt':
    print('SKIP: needs a POSIX C compiler')
    raise SystemExit(0)
sources = ['libs/video/tests/test_engine_present_skip.c'] + ['libs/video/' + n + '.c' for n in (
    'rsx_draw_engine', 'rsx_fifo_busy', 'rsx_dispatch', 'rsx_vertex_compact', 'rsx_texture_layout',
    'rsx_vp_decompiler', 'rsx_fp_decompiler')] + ['runtime/diagnostics/render_ablation.c',
    'libs/video/tests/gcm_deferred_fixture_stubs.c']
with tempfile.TemporaryDirectory(prefix='engine-present-skip-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-Iinclude', '-Ilibs/video',
                    *sources, '-pthread', '-lm', '-o', str(binary)], cwd=root, check=True, timeout=300)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3RECOMP_', 'RSX_'))}
    result = subprocess.run([str(binary)], cwd=root, timeout=120, capture_output=True, text=True, env=env)
    print('\n'.join(l for l in result.stdout.splitlines() if l.startswith(('[FAIL]', '[engine_present_skip]', '[PASS]',))))
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_engine_present_skip failed')
