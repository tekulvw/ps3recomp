#!/usr/bin/env python3
"""The NP social provider (docs/np-social-provider.md), synthetic.

Builds tests/test_np_social.c against the real NP, NP Basic, NP Lookup and
sysutil sources and runs it twice: with a provider that has every callback
("full") and with one that has none ("minimal"). Both runs first check that
nothing changes before a provider is installed."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: social-provider fixture requires the POSIX toolchain')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC', 'cc')
sources = ['tests/test_np_social.c', 'libs/network/sceNp.c', 'libs/network/np_psnr.c',
           'libs/network/psnr/psnr.c', 'libs/system/cellSysutil.c',
           'libs/system/cellSysutilNpEula.c', 'libs/network/sceNpBasic.c',
           'libs/network/sceNpLookup.c', 'libs/network/np_social.c']
with tempfile.TemporaryDirectory(prefix='ps3-np-social-') as tmp:
    work = Path(tmp)
    objects = []
    for i, source in enumerate(sources):
        obj = work / f'{i}.o'
        objects.append(str(obj))
        subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                        '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                        '-c', str(root / source), '-o', str(obj)], check=True, timeout=300)
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    exe = work / 'fixture'
    subprocess.run([cc, *objects, '-pthread', gc, '-o', str(exe)], check=True, timeout=300)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3_', 'PS3RECOMP_', 'PSNR_'))}
    for mode in ('full', 'minimal'):
        subprocess.run([str(exe), mode], check=True, timeout=60, env=env)
print('PASS: NP social provider')
