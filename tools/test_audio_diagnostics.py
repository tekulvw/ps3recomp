#!/usr/bin/env python3
"""Opt-in audio observations from controlled BE PCM through the real mixer."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
def main():
    if os.name == 'nt':
        print('SKIP: standalone fixture uses POSIX and SDL2')
        return
    darwin = sys.platform == 'darwin'
    include = os.environ.get('PS3RECOMP_TEST_SDL_INCLUDE', '/opt/homebrew/include' if darwin else '')
    libs = os.environ.get('PS3RECOMP_TEST_SDL_LIB_DIR', '/opt/homebrew/lib' if darwin else '')
    includes = ['-I'+str(root/'include')] + (['-I'+include] if include else [])
    linker = (['-L'+libs] if libs else []) + ['-lSDL2']
    if not include and not libs:
        config = shutil.which('sdl2-config')
        if config:
            includes += shlex.split(subprocess.check_output([config,'--cflags'],text=True))
            linker = shlex.split(subprocess.check_output([config,'--libs'],text=True))
    sections = ['-Wl,-dead_strip'] if darwin else ['-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    with tempfile.TemporaryDirectory(prefix='audio-diagnostics-') as tmp:
        exe = str(Path(tmp) / 'fixture')
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-pthread',*includes,
            str(root/'libs/audio/tests/test_audio_diagnostics.c'),*linker,*sections,'-o',exe],check=True,timeout=30)
        env = {k:v for k,v in os.environ.items() if k != 'AUDIO_DIAGNOSTICS'}
        for value in (None,'0','true','1'):
            runenv = dict(env)
            if value is not None: runenv['AUDIO_DIAGNOSTICS']=value
            result=subprocess.run([exe],env=runenv,text=True,capture_output=True,check=True,timeout=5)
            assert 'PASS real mixer' in result.stdout
            if value != '1':
                assert not result.stderr, result.stderr
                continue
            lines=result.stderr.splitlines()
            assert len(lines)==4, lines
            assert 'channels=8' in lines[0] and 'blocks=1 nonzero=1' in lines[0], lines
            assert 'peaks=[0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8]' in lines[0], lines
            assert 'mix=[0.5721,0.7279]' in lines[1], lines
            assert 'submit=2 errors=1' in lines[1] and 'queue_frames=256' in lines[1], lines
            assert 'controlled queue failure' in lines[1], lines
            assert 'blocks=1 nonzero=0' in lines[2] and 'peaks=[0,0,0,0,0,0,0,0]' in lines[2], lines
            assert 'mix=[0,0]' in lines[3], lines
        print('PASS: BE channels, downmix, ring clearing, SDL errors, cadence/reset, explicit opt-in')
if __name__ == '__main__': main()
