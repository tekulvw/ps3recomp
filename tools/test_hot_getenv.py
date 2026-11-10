#!/usr/bin/env python3
"""Hot syscalls read their diagnostic switches once, not on every call.

Builds runtime/syscalls/tests/test_hot_getenv.c once per syscall unit, with
getenv() routed through a counter, and runs each: thousands of semaphore and
condition-variable calls must make a bounded number of getenv() calls.
"""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: POSIX fixture')
    raise SystemExit(0)
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/syscalls/tests/test_hot_getenv.c'
compiler = os.environ.get('CC', 'cc')
includes = [ROOT / 'include', ROOT / 'runtime', ROOT / 'runtime/syscalls']

with tempfile.TemporaryDirectory(prefix='hot-getenv-') as tmp:
    for unit in ('SEMAPHORE', 'COND'):
        binary = Path(tmp) / f'test_{unit.lower()}'
        subprocess.run([compiler, '-std=gnu17', '-O1', '-UNDEBUG', f'-DHOT_UNIT_{unit}',
                        *('-I' + str(i) for i in includes), str(SOURCE), '-pthread',
                        '-o', str(binary)], check=True, timeout=120, cwd=tmp)
        run = subprocess.run([str(binary)], cwd=tmp, timeout=60, capture_output=True, text=True)
        print(run.stdout.strip(), flush=True)
        if run.returncode != 0:
            print(run.stderr[-2000:])
            raise SystemExit(f'test_hot_getenv ({unit}) failed (exit {run.returncode})')
print('PASS')
