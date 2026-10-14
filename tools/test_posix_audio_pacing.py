#!/usr/bin/env python3
"""Real POSIX mixer timing against inert SDL/VM and a deterministic clock."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='posix-audio-pacing-') as tmp:
        exe=str(Path(tmp)/'fixture')
        flags=['-fsanitize=undefined','-fno-sanitize-recover=all'] if args.sanitize else []
        subprocess.run([os.environ.get('CC','clang'),'-std=gnu17','-O2','-pthread',*flags,
            '-I'+str(ROOT/'include'),'-I/opt/homebrew/include',
            str(ROOT/'libs/audio/tests/test_posix_audio_pacing.c'),'-Wl,-dead_strip','-o',exe],check=True)
        env={k:v for k,v in os.environ.items() if not k.startswith(('AUDIO_','SPU_DMA_DISPOSITION'))}
        failed=[]
        for mode in range(9):
            r=subprocess.run([exe,str(mode)],env=env,text=True,capture_output=True,timeout=10)
            print(r.stdout,end='');print(r.stderr,end='')
            if r.returncode:failed.append(mode)
        if failed:raise SystemExit('FAIL modes: '+str(failed))
        print('PASS: normal, no-device, frozen/full, drain recovery, long suspension, port stop/start, rejecting sink, ten-second rate, device loss/return')
if __name__=='__main__':main()
