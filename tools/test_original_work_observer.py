#!/usr/bin/env python3
"""Bounded source-qualified observer control/worker fixture; no game execution."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='original-work-observer-') as temporary:
    tmp=Path(temporary)
    compiler=shutil.which(os.environ.get('CC','clang-cl' if os.name=='nt' else 'cc'))
    if not compiler:
        raise SystemExit('C compiler required')
    binary=tmp/('observer.exe' if os.name=='nt' else 'observer')
    sources=[ROOT/'tests/test_original_work_observer.c',ROOT/'runtime/diagnostics/original_work_observer.c']
    if os.name=='nt':
        command=[compiler,'/nologo','/std:c17','/W4','/WX','/DPS3RECOMP_ORIGINAL_OBSERVER=1',
                 '/I'+str(ROOT/'runtime/diagnostics'),*map(str,sources),'/Fo'+str(tmp)+'\\','/Fe'+str(binary)]
    else:
        command=[compiler,'-std=c17','-Wall','-Wextra','-Werror','-DPS3RECOMP_ORIGINAL_OBSERVER=1',
                 '-I'+str(ROOT/'runtime/diagnostics'),*map(str,sources),'-pthread','-o',str(binary)]
    subprocess.run(command,check=True,timeout=60)
    for mode in ['good','wrong_source','missing_worker','overflow','loss','route','unregistered','after_retire','late_arm']+['arm_race']*8:
        subprocess.run([str(binary),mode],check=True,timeout=10)
