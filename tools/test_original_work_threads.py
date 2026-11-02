#!/usr/bin/env python3
"""Real POSIX factory/cleanup and diagnostic-vs-final drain behavior."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='original-worker-lifecycle-')as temporary:
    binary=Path(temporary)/'fixture'
    command=[os.environ.get('CC','cc'),'-std=c17','-Wall','-Wextra','-Werror',
        '-DPS3RECOMP_ORIGINAL_OBSERVER=1','-I'+str(ROOT/'runtime/diagnostics'),
        str(ROOT/'tests/test_original_work_threads.c'),str(ROOT/'runtime/diagnostics/original_work_threads.c'),
        str(ROOT/'runtime/diagnostics/original_work_observer.c'),'-pthread','-o',str(binary)]
    subprocess.run(command,check=True,timeout=60)
    for mode in ['return','exit']:
        subprocess.run([str(binary),mode],check=True,timeout=10)
