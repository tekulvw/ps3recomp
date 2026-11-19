#!/usr/bin/env python3
"""CPU-only synthetic row-mask ownership/replay against the current backend ABI."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='encode-queue-rows-') as tmp:
    binary = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-Wall', '-Wextra', '-Wno-unused-parameter', '-Iinclude', '-Ilibs/video',
                    'libs/video/tests/test_encode_queue_rows.c', 'libs/video/rsx_encode_queue.c',
                    'libs/video/rsx_fifo_busy.c', '-pthread', '-lm', '-o', str(binary)],
                   cwd=ROOT, check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)
