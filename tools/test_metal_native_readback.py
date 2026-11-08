#!/usr/bin/env python3
"""CPU-only layout/lease guards. Never creates a GPU device or submits work."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='native-pixel-guards-') as directory:
    binary=Path(directory)/'layout'
    subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror',
                    '-Iinclude','libs/video/tests/test_native_readback_layout.c','-o',str(binary)],cwd=root,check=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=10)
    if sys.platform=='darwin':
        binary=Path(directory)/'lease'
        subprocess.run([os.environ.get('CC','clang'),'-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK',
                        '-Iinclude','-Ilibs/video','libs/video/tests/test_metal_native_image_format.m',
                        'libs/video/rsx_texture_layout.c','runtime/diagnostics/critical_trace.c','libs/video/rsx_fifo_busy.c','libs/input/pad_macos.m',
                        '-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],
                       cwd=root,check=True,timeout=60)
        subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: bounded native readback layout and missing-lease guards (CPU only)')
