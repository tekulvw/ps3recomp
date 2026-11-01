#!/usr/bin/env python3
"""Compile real Windows cellPad focus tests; no desktop, game, or controller."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    if os.name != 'nt':
        print('SKIP: native Windows cellPad fixture')
        return 0
    compiler = shutil.which(os.environ.get('CC', 'clang-cl'))
    if not compiler:
        raise RuntimeError('Run in a Visual Studio developer environment with clang-cl')
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='pad-focus-') as temp:
        work = Path(temp)
        binary = work / 'fixture.exe'
        subprocess.run([compiler, '/nologo', '/std:c17', '/O1', '/W4', '/WX', '/MT', '/UNDEBUG',
                        '/D_CRT_SECURE_NO_WARNINGS', '/DPS3RECOMP_NATIVE_OSK_WINDOWS=1','/I'+str(root/'include'),
                        str(root/'libs/input/tests/test_pad_file_focus_windows.c'),
                        '/Fo'+str(work/'fixture.obj'), '/Fe'+str(binary), '/link', 'user32.lib', 'xinput.lib'],
                       cwd=work, check=True, timeout=60)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('PAD_', 'YDKJ_'))}
        for mode in ('analog-reparse','reparse','analog','analog-no-controller','analog-focus','analog-osk','analog-invalid','analog-locked','analog-mixed','analog-off','analog-no-focus',
                     'osk-owner','late-unfocused', 'locked-admission', 'focused', 'no-controller', 'blocked-discard', 'long-path', 'legacy', 'zero', 'no-file'):
            subprocess.run([str(binary), mode, str(work/(mode+'.txt'))],
                           cwd=work, env=env, check=True, timeout=10)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
