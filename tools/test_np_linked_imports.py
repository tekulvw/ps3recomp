#!/usr/bin/env python3
"""NP/signaling/HTTP imports a linked title calls after its update check:
registration in the generated HLE table and their answers (synthetic)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: linked-import fixture requires the POSIX toolchain')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC', 'cc')
cxx = 'clang++' if 'clang' in cc else ('g++' if 'gcc' in cc else 'c++')
sources = ['tests/test_np_linked_imports.c', 'libs/network/sceNp.c', 'libs/network/np_psnr.c',
           'libs/network/psnr/psnr.c', 'libs/system/cellSysutil.c',
           'libs/system/cellSysutilNpEula.c', 'libs/network/sceNpBasic.c',
           'libs/network/sceNpSignaling.c', 'libs/network/cellHttp.c']
with tempfile.TemporaryDirectory(prefix='ps3-np-linked-') as tmp:
    work = Path(tmp)
    gen = work / 'nids.cpp'
    subprocess.run([sys.executable, str(root / 'tools/gen_hle_nids.py'), '--out', str(gen),
                    'sceNp', 'sceNpBasic', 'sceNpSignaling', 'cellHttp'], check=True, timeout=300)
    objects = []
    for i, source in enumerate(sources):
        obj = work / f'{i}.o'
        objects.append(str(obj))
        subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                        '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                        '-c', str(root / source), '-o', str(obj)], check=True, timeout=300)
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    exe = work / 'fixture'
    subprocess.run([cxx, str(gen), *objects, '-pthread', gc, '-o', str(exe)], check=True, timeout=300)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PS3_', 'PS3RECOMP_', 'PSNR_'))}
    subprocess.run([str(exe)], check=True, timeout=10, env=env)
