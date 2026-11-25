#!/usr/bin/env python3
"""The optional file overlay over the VFS root (PS3_VFS_OVERLAY, docs/VFS_OVERLAY.md).

Builds runtime/ppu/tests/test_vfs_overlay.cpp (the PPU cellFs HLE in ppu_fs.cpp
with the real edat.c) and links the real libs/filesystem/cellFs.c and
runtime/syscalls/sys_fs.c, then runs it in separate processes on a synthetic
game tree and overlay this script makes:

  on      only the overlaid file differs, for open/read/write/stat/fstat and a
          directory listing, through all three translators
  off     without PS3_VFS_OVERLAY nothing changes
  inert   PS3_VFS_OVERLAY without PS3_VFS_ROOT, or naming no directory: nothing
          changes, and stderr says why

No game file, key or GPU is used. It needs a POSIX C/C++ compiler and a
dead-section linker (under CI=1 a missing one fails instead of skipping).
"""
import importlib.util
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
FIXTURE = ROOT / 'runtime/ppu/tests/test_vfs_overlay.cpp'
INCLUDE_DIRS = ['include', 'runtime/ppu', 'runtime/memory', 'runtime', 'libs', 'libs/filesystem', '.']


def _stub_header():
    spec = importlib.util.spec_from_file_location('ppu_lifter', TOOLS / 'ppu_lifter.py')
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def _tree(base):
    root = base / 'game'
    overlay = base / 'server-overlay-with-a-longer-name'
    run = root / 'PS3_GAME/USRDIR/run'
    (run / 'sub').mkdir(parents=True)
    (run / 'settings.bin').write_bytes(b'original-settings')
    (run / 'settings.bin.bak').write_bytes(b'backup')
    (run / 'other.bin').write_bytes(b'other-data')
    (run / 'sub/inner.bin').write_bytes(b'inner')
    (root / 'PS3_GAME/PARAM.SFO').write_bytes(b'sfo')
    sibling = base / 'game-sibling/PS3_GAME/USRDIR/run'
    sibling.mkdir(parents=True)
    (sibling / 'settings.bin').write_bytes(b'sibling')
    orun = overlay / 'PS3_GAME/USRDIR/run'
    (orun / 'sub').mkdir(parents=True)
    (orun / 'settings.bin').write_bytes(b'OVERLAY-settings-and-more')
    (orun / 'sub/overlay-only.bin').write_bytes(b'not listed')
    (base / 'hdd0/game/X/USRDIR/run').mkdir(parents=True)
    return root, overlay


def main():
    if os.name == 'nt':
        print('SKIP: fixture requires a POSIX C/C++ compiler and dead-section linker')
        return 0
    cc = shutil.which(os.environ.get('CC', 'cc'))
    cxx = shutil.which(os.environ.get('CXX', 'c++'))
    if not cc or not cxx:
        if os.environ.get('CI'):
            print('FAIL: no C/C++ compiler found')
            return 1
        print('SKIP: no C/C++ compiler')
        return 0
    gc = '-Wl,-dead_strip' if platform.system() == 'Darwin' else '-Wl,--gc-sections'
    with tempfile.TemporaryDirectory(prefix='vfs-overlay-') as temporary:
        tmp = Path(temporary)
        (tmp / 'ppu_recomp.h').write_text(_stub_header())
        objects = []
        for source in ('libs/filesystem/cellFs.c', 'runtime/syscalls/sys_fs.c'):
            obj = tmp / (Path(source).stem + '.o')
            subprocess.run([cc, '-std=gnu17', '-D_GNU_SOURCE', '-O1', '-w', '-ffunction-sections', '-fdata-sections',
                            '-I' + str(ROOT / 'include'), '-c', str(ROOT / source), '-o', str(obj)],
                           check=True, timeout=300)
            objects.append(str(obj))
        exe = tmp / 'test_vfs_overlay'
        cmd = [cxx, '-std=gnu++17', '-O1', '-w', '-ffunction-sections', '-fdata-sections', gc, '-I' + str(tmp)]
        cmd += ['-I' + str(ROOT / d) for d in INCLUDE_DIRS]
        cmd += [str(FIXTURE), *objects, '-o', str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if r.returncode:
            print('FAIL: fixture did not build')
            print((r.stdout + r.stderr)[-4000:])
            return 1

        base_env = {k: v for k, v in os.environ.items() if k in ('PATH', 'HOME', 'TMPDIR', 'SYSTEMROOT')}
        failures = 0
        cases = [
            ('on', 'on', {'PS3_VFS_ROOT': None, 'PS3_VFS_OVERLAY': None}, 'overlay: files in'),
            ('on, trailing separators', 'on', {'PS3_VFS_ROOT': '/', 'PS3_VFS_OVERLAY': '/'}, 'overlay: files in'),
            ('off', 'off', {'PS3_VFS_ROOT': None}, None),
            ('off, empty switch', 'off', {'PS3_VFS_ROOT': None, 'PS3_VFS_OVERLAY': ''}, None),
            ('inert, no root', 'inert', {'PS3_VFS_OVERLAY': None}, 'without PS3_VFS_ROOT'),
            ('inert, no overlay directory', 'inert', {'PS3_VFS_ROOT': None, 'PS3_VFS_OVERLAY': 'missing'},
             'is not a directory'),
        ]
        for index, (label, mode, variables, message) in enumerate(cases):
            case = tmp / f'case{index}'
            case.mkdir()
            root, overlay = _tree(case)
            env = dict(base_env, PS3_HDD0_ROOT=str(case / 'hdd0'))
            for name, value in variables.items():
                if value is None:
                    env[name] = str(root if name == 'PS3_VFS_ROOT' else overlay)
                elif value == '/':
                    env[name] = str(root if name == 'PS3_VFS_ROOT' else overlay) + '/'
                elif value == 'missing':
                    env[name] = str(case / 'missing')
                else:
                    env[name] = value
            r = subprocess.run([str(exe), mode, str(root), str(overlay)], cwd=case, env=env,
                               capture_output=True, text=True, timeout=30)
            sys.stdout.write(f'--- {label}\n' + r.stdout)
            ok = r.returncode == 0
            if message is None:
                said = '[vfs]' not in r.stderr
                print(('[PASS] ' if said else '[FAIL] ') + 'stderr says nothing about an overlay')
            else:
                said = message in r.stderr
                print(('[PASS] ' if said else '[FAIL] ') + f'stderr says "{message}"')
            if mode == 'on':
                named = '[vfs] overlay file: PS3_GAME/USRDIR/run/settings.bin' in r.stderr
                print(('[PASS] ' if named else '[FAIL] ') + 'stderr names the redirected file')
                said = said and named
            if not (ok and said):
                sys.stderr.write(r.stderr[-4000:])
                failures += 1
        print('PASS' if not failures else f'FAIL: {failures} case(s)')
        return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
