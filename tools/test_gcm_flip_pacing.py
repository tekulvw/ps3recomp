#!/usr/bin/env python3
"""Flip pacing, fixed and adaptive (libs/video/tests/test_gcm_flip_pacing.c,
which compiles cellGcmSys.c in): one run per PS3RECOMP_GCM_FLIP_PACING mode."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
if os.name == 'nt':
    print('SKIP: the GCM flip fixture uses POSIX linking')
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='gcm-flip-pacing-') as tmp:
    binary = Path(tmp) / 'test'
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections',
                    '-I' + str(root / 'include'), '-I' + str(root / 'libs/video'),
                    str(root / 'libs/video/tests/test_gcm_flip_pacing.c'),
                    str(root / 'libs/video/tests/gcm_deferred_fixture_stubs.c'), *linker,
                    '-o', str(binary)], check=True)
    for mode in ('fixed', 'adaptive'):
        result = subprocess.run([str(binary), mode], timeout=30, capture_output=True, text=True)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit('test_gcm_flip_pacing failed: ' + mode)
