#!/usr/bin/env python3
"""Bounded CPU checks for exact FIFO publication and real decoder lookup."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    raise SystemExit('POSIX standalone fixture; production API remains portable C')
with tempfile.TemporaryDirectory(prefix='gcm-native-handoff-') as temp:
    common = [os.environ.get('CC','cc'), '-std=gnu17', '-O1', '-UNDEBUG',
              '-ffunction-sections', '-fdata-sections', '-Iinclude', '-Ilibs/video']
    linker = ['-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections', '-pthread', '-lm']
    for name, sources in (
        ('handoff', ['libs/video/tests/test_gcm_native_handoff.c', 'libs/video/tests/gcm_deferred_fixture_stubs.c']),
        ('target', ['libs/video/tests/test_native_source_target.c'] + ['libs/video/'+x+'.c' for x in (
            'rsx_draw_engine','rsx_dispatch','rsx_texture_layout','rsx_vertex_compact','rsx_vertex_fetch','rsx_vp_decompiler','rsx_fp_decompiler')] +
                   ['runtime/diagnostics/critical_trace.c','runtime/diagnostics/render_ablation.c']),
    ):
        output = Path(temp) / name
        subprocess.run(common + sources + linker + ['-o', str(output)], cwd=ROOT, check=True, timeout=60)
        subprocess.run([str(output)], check=True, timeout=10)
