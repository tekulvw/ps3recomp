#!/usr/bin/env python3
"""Synthetic fixture for the opt-in job-manager censuses (runtime/diagnostics/wws_census_impl.h)."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
diag = root / 'runtime/diagnostics'
with tempfile.TemporaryDirectory(prefix='wws-census-') as tmp:
    for mode in ('optimized', 'ubsan'):
        exe = Path(tmp) / mode
        out = Path(tmp) / (mode + '-jobio')
        out.mkdir()
        cmd = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu'),
               '-I' + str(root / 'runtime/platform'), str(diag / 'tests/test_wws_census.c'),
               str(diag / 'job_io_recorder.c'), '-pthread', '-o', str(exe)]
        if mode == 'ubsan':
            cmd[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe), str(out)], check=True, timeout=60)
        print(mode, 'ok')
