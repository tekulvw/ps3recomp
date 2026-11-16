#!/usr/bin/env python3
"""rsx_ppm.h: packed-row PPM writes equal the per-pixel writer
(libs/video/tests/test_rsx_ppm.c; pure logic, any host)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC') or shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
if not cc:
    print('SKIP: no C compiler')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='rsx-ppm-') as tmp:
    binary = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run([cc, '-std=gnu11', '-O1', '-UNDEBUG', str(root / 'libs/video/tests/test_rsx_ppm.c'),
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode:
        raise SystemExit('test_rsx_ppm failed')
