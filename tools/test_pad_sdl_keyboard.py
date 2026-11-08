#!/usr/bin/env python3
"""SDL game-window keyboard on pad port 0 (Linux and other non-Apple POSIX hosts).

Builds libs/input/tests/test_pad_sdl_keyboard.c against the real cellPad and
pad_sdl_keyboard sources and runs its layout, opt-out and real-SDL-window modes.
No game files, controller or display needed (the SDL mode uses the dummy driver).
"""
from pathlib import Path
import os, shlex, shutil, subprocess, sys, tempfile

r = Path(__file__).resolve().parents[1]
if sys.platform in ('win32', 'darwin'):
    print('SKIP: the SDL keyboard pad is built only on non-Apple, non-Windows hosts'); raise SystemExit(0)
tool = ['sdl2-config', '--cflags', '--libs'] if shutil.which('sdl2-config') else ['pkg-config', '--cflags', '--libs', 'sdl2']
if not shutil.which(tool[0]):
    print('SKIP: SDL2 development files not found'); raise SystemExit(0)
flags = shlex.split(subprocess.run(tool, check=True, capture_output=True, text=True).stdout)
cc = os.environ.get('CC', 'cc')
with tempfile.TemporaryDirectory(prefix='pad-sdl-keyboard-') as t:
    out = Path(t) / 'fixture'
    subprocess.run([cc, '-std=gnu17', '-O1', '-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                    '-DPS3RECOMP_PAD_SDL_KEYBOARD=1', '-I' + str(r / 'include'),
                    str(r / 'libs/input/tests/test_pad_sdl_keyboard.c'), str(r / 'libs/input/pad_sdl_keyboard.c'),
                    '-o', str(out), *flags, '-lm', '-pthread', '-Wl,--gc-sections'], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PAD_', 'YDKJ_'))}
    env['SDL_VIDEODRIVER'] = 'dummy'
    failed = 0
    for mode in ('layout', 'off', 'sdl'):
        p = subprocess.run([str(out), mode], env=env, capture_output=True, text=True, timeout=60)
        print(f'== {mode}: exit {p.returncode}')
        for line in p.stdout.splitlines():
            if not line.startswith('[cellPad]'): print('  ' + line)
        if p.returncode: failed += 1; print(p.stderr[-4000:])
    raise SystemExit(1 if failed else 0)
