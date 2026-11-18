#!/usr/bin/env python3
"""PS3RECOMP_RSX_PIPELINE_REUSE: kept pipelines equal a full gather on a synthetic stream
(libs/video/tests/test_engine_pipeline_reuse.c, with the draw engine and a stub backend)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
if not cc or os.name == 'nt':
    print('SKIP: needs a POSIX C compiler')
    raise SystemExit(0)
sources = ['libs/video/tests/test_engine_pipeline_reuse.c'] + ['libs/video/' + n + '.c' for n in (
    'rsx_draw_engine', 'rsx_fifo_busy', 'rsx_dispatch', 'rsx_vertex_compact', 'rsx_texture_layout',
    'rsx_vp_decompiler', 'rsx_fp_decompiler')] + ['runtime/diagnostics/render_ablation.c',
    'libs/video/tests/gcm_deferred_fixture_stubs.c']
with tempfile.TemporaryDirectory(prefix='engine-pipeline-reuse-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-Iinclude', '-Ilibs/video',
                    *sources, '-pthread', '-lm', '-o', str(binary)], cwd=root, check=True, timeout=300)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3RECOMP_', 'RSX_'))}
    result = subprocess.run([str(binary)], cwd=root, timeout=120, capture_output=True, text=True, env=env)
    print('\n'.join(l for l in result.stdout.splitlines() if l.startswith(('[FAIL]', '[engine_pipeline_reuse]', '[PASS] on', '[PASS] every', '[PASS] verify', '[PASS] the register'))))
    if result.returncode:
        print(result.stderr[-2000:])
        raise SystemExit('test_engine_pipeline_reuse failed')
    # --mutants: each text mutation of the engine's reuse path must make the fixture fail.
    if '--mutants' in sys.argv:
        engine = root / 'libs/video/rsx_draw_engine.c'
        original = engine.read_text()
        mutants = {
            'table-marks-a-key-register-safe': ('if (!eng_preuse_safe_method(m)) t |= ENG_PREUSE_RELEVANT;',
                                                'if (!eng_preuse_safe_method(m) && m != 0x1D60u) t |= ENG_PREUSE_RELEVANT;'),
            'table-treats-exec-as-comparable': ('if (g.rsx.klass[ridx] != RSX_DSP_CLASS_EXEC && !(m >= 0x0B80u && m < 0x0C00u)) t |= ENG_PREUSE_COMPARABLE;',
                                                'if (!(m >= 0x0B80u && m < 0x0C00u)) t |= ENG_PREUSE_COMPARABLE;'),
            'hot-path-never-bumps': ('            s_preuse_gen++;\n            /* The per-method histogram',
                                     '            /* The per-method histogram'),
        }
        for name, (old, new) in mutants.items():
            assert original.count(old) == 1, name
            mutant = Path(tmp) / f'mutant-{name}.c'
            mutant.write_text(original.replace(old, new))
            msources = [str(mutant) if src.endswith('rsx_draw_engine.c') else src for src in sources]
            mbinary = Path(tmp) / f'mutant-{name}'
            subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-Iinclude', '-Ilibs/video',
                            *msources, '-pthread', '-lm', '-o', str(mbinary)], cwd=root, check=True, timeout=300)
            caught = subprocess.run([str(mbinary)], cwd=root, timeout=120, capture_output=True, text=True, env=env).returncode != 0
            print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
            if not caught:
                raise SystemExit('test_engine_pipeline_reuse: a mutant was not caught')
