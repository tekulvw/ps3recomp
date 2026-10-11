#!/usr/bin/env python3
"""Build/run PPU lightweight-lock regression on POSIX; no game or GPU required."""
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def main():
    if os.name == 'nt':
        print('SKIP: regression requires the POSIX PPU test toolchain')
        return
    with tempfile.TemporaryDirectory(prefix='ps3-lwmutex-') as tmp:
        out=Path(tmp)
        subprocess.run([sys.executable, str(ROOT/'tools/make_smoke_elf.py'),
                        '--out', str(out/'smoke.elf'), '--header', str(out/'ppu_recomp.h')], check=True)
        cc=os.environ.get('CC', 'cc'); cxx=os.environ.get('CXX', 'c++')
        common=['-O1', '-ffunction-sections', '-fdata-sections', '-pthread']
        subprocess.run([cc, *common, '-c', str(ROOT/'runtime/platform/win32_compat.c'),
                        '-o', str(out/'compat.o')], check=True)
        linker='-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections'
        subprocess.run([cxx, *common, '-std=c++20', '-UNDEBUG', linker,
                        '-I'+str(out), '-I'+str(ROOT/'include'), '-I'+str(ROOT/'runtime/ppu'),
                        str(ROOT/'runtime/ppu/tests/test_ppu_lwmutex.cpp'), str(out/'compat.o'),
                        '-o', str(out/'test')], check=True)
        subprocess.run([str(out/'test')], check=True, timeout=15)

if __name__=='__main__':
    main()
