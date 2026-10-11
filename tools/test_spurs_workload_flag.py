#!/usr/bin/env python3
"""Exercise the real SPURS workload flag layout/guest ABI without game assets."""
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: workload flag fixture requires the POSIX toolchain')
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-spurs-flag-') as tmp:
    binary = Path(tmp) / 'test'
    linker = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', linker, '-pthread',
                    '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu'),
                    str(root / 'tests/test_spurs_workload_flag.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    generated = Path(tmp) / 'nids.cpp'
    subprocess.run([sys.executable, str(root / 'tools/gen_hle_nids.py'),
                    '--out', str(generated), 'cellSpurs'], check=True)
    assert 'ps3_hle_register(0xC765B995u, "cellSpursGetWorkloadFlag",' in generated.read_text()
    print('SPURS workload flag firmware NID registration PASS')
