#!/usr/bin/env python3
"""Run deterministic POSIX taskset workload registration tests without a game or GPU."""
import os
import platform
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: taskset fixture requires the POSIX toolchain')
    raise SystemExit(0)

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-taskset-') as tmp:
    binary=Path(tmp)/'test'
    linker='-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
                    '-ffunction-sections','-fdata-sections',linker,'-pthread',
                    '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),
                    str(root/'tests/test_spurs_taskset_registration.c'),
                    str(root/'libs/spurs/spurs_taskset.c'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=10)
