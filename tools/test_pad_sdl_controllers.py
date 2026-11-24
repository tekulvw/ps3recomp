#!/usr/bin/env python3
"""cellPad's SDL2 controller backend with synthetic controllers (SDL virtual joysticks).

Builds libs/input/tests/test_pad_sdl_controllers.c against the real cellPad source
and real SDL2 and runs it: a synthetic DualSense, DualShock 4 and Xbox pad, no
physical controller, window or game file.

POSIX hosts take SDL2 from sdl2-config (or pkg-config). Windows needs clang-cl in a
Visual Studio developer environment and SDL2_DIR naming an extracted official SDL2
Windows development package (SDL2-devel-<version>-VC.zip): its root or its cmake
folder. Without SDL2 the test skips, unless CI=1, where a skip would hide the gap.
"""
from pathlib import Path
import os, shlex, shutil, subprocess, sys, tempfile

r = Path(__file__).resolve().parents[1]
source = r / 'libs/input/tests/test_pad_sdl_controllers.c'
ci = os.environ.get('CI') == '1'


def skip(reason):
    if ci:
        print('FAIL: ' + reason); raise SystemExit(1)
    print('SKIP: ' + reason); raise SystemExit(0)


def build_posix(out):
    tool = ['sdl2-config', '--cflags', '--libs'] if shutil.which('sdl2-config') else ['pkg-config', '--cflags', '--libs', 'sdl2']
    if not shutil.which(tool[0]):
        skip('SDL2 development files not found')
    flags = shlex.split(subprocess.run(tool, check=True, capture_output=True, text=True).stdout)
    cc = os.environ.get('CC', 'cc')
    gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    subprocess.run([cc, '-std=gnu17', '-O1', '-Wall', '-Wno-unused-function', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(r / 'include'), str(source), '-o', str(out), *flags, '-lm', '-pthread', gc], check=True)


def build_windows(work):
    compiler = shutil.which(os.environ.get('CC', 'clang-cl'))
    if not compiler:
        skip('clang-cl not found; run in a Visual Studio developer environment')
    sdl = os.environ.get('SDL2_DIR')
    if not sdl:
        skip('SDL2_DIR is not set (an extracted SDL2-devel-<version>-VC.zip)')
    root = Path(sdl)
    if not (root / 'include/SDL.h').is_file():
        root = root.parent
    lib = root / 'lib/x64'
    if not (root / 'include/SDL.h').is_file() or not (lib / 'SDL2.lib').is_file():
        skip(f'no SDL2 Windows development package at {sdl}')
    out = work / 'fixture.exe'
    subprocess.run([compiler, '/nologo', '/std:c17', '/O1', '/W3', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
                    '/I' + str(r / 'include'), '/I' + str(root / 'include'), str(source),
                    '/Fo' + str(work / 'fixture.obj'), '/Fe' + str(out),
                    '/link', str(lib / 'SDL2.lib'), 'user32.lib'], cwd=work, check=True, timeout=300)
    shutil.copy2(lib / 'SDL2.dll', work / 'SDL2.dll')
    return out


with tempfile.TemporaryDirectory(prefix='pad-sdl-controllers-') as t:
    work = Path(t)
    if os.name == 'nt':
        binary = build_windows(work)
    else:
        binary = work / 'fixture'
        build_posix(binary)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PAD_', 'YDKJ_', 'SDL_'))}
    p = subprocess.run([str(binary)], cwd=work, env=env, capture_output=True, text=True, timeout=60)
    print(p.stdout, end='')
    if p.returncode:
        print(p.stderr[-4000:], file=sys.stderr)
    raise SystemExit(p.returncode)
