#!/usr/bin/env python3
"""cellGameUpdate: NID registration and the "no update" answer through the
real cellNetCtl, cellSysutil pump and generated HLE table (synthetic)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: game-update fixture requires the POSIX toolchain')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
cc = os.environ.get('CC', 'cc')
cxx = 'clang++' if 'clang' in cc else ('g++' if 'gcc' in cc else 'c++')
with tempfile.TemporaryDirectory(prefix='ps3-gameupdate-') as tmp:
    work = Path(tmp)
    gen = work / 'nids.cpp'
    subprocess.run([sys.executable, str(root / 'tools/gen_hle_nids.py'), '--out', str(gen),
                    'cellNetCtl'], check=True, timeout=300)
    objects = []
    for i, source in enumerate([root / 'tests/test_game_update.c',
                                root / 'libs/network/cellNetCtl.c',
                                root / 'libs/system/cellSysutil.c']):
        obj = work / f'{i}.o'
        objects.append(str(obj))
        subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-Wall', '-Wextra',
                        '-ffunction-sections', '-fdata-sections',
                        '-I' + str(root / 'include'), '-I' + str(root / 'libs/system'),
                        '-c', str(source), '-o', str(obj)], check=True, timeout=300)
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    exe = work / 'fixture'
    subprocess.run([cxx, str(gen), *objects, '-pthread', gc, '-o', str(exe)],
                   check=True, timeout=300)
    subprocess.run([str(exe)], check=True, timeout=10)
