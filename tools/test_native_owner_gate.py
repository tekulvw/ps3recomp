#!/usr/bin/env python3
"""Real host-owner synchronization fixture; no graphics device or window."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='native-owner-gate-') as tmp:
    cc = shutil.which(os.environ.get('CC', 'clang-cl' if os.name == 'nt' else 'cc'))
    if not cc:
        raise SystemExit('C compiler required for native owner gate fixture')
    binary = Path(tmp) / ('fixture.exe' if os.name == 'nt' else 'fixture')
    source = str(ROOT / 'libs/video/tests/test_native_owner_gate.c')
    if os.name == 'nt':
        command = [cc, '/nologo', '/std:c17', '/W4', '/WX', '/UNDEBUG',
                   '/D_CRT_SECURE_NO_WARNINGS', '/I'+str(ROOT/'include'), source,
                   '/Fo'+tmp+'\\', '/Fe'+str(binary)]
    else:
        command = [cc, '-std=gnu17', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                   '-I'+str(ROOT/'include'), '-ffunction-sections', '-fdata-sections',
                   source, str(ROOT/'runtime/platform/win32_compat.c'),
                   '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                   '-pthread', '-o', str(binary)]
    subprocess.run(command, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20)
