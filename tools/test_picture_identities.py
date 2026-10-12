#!/usr/bin/env python3
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def main():
    if os.name=='nt':print('SKIP standalone POSIX compiler fixture');return
    with tempfile.TemporaryDirectory(prefix='picture-identities-') as p:
        exe=Path(p)/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c17','-Wall','-Wextra','-Werror','-UNDEBUG','-pthread','-I'+str(root/'include'),str(root/'libs/video/tests/test_picture_identities.c'),'-o',str(exe)],check=True,timeout=30)
        for mode in ("happy","threaded","capacity","aliases","commands"):subprocess.run([str(exe),mode],check=True,timeout=15)
if __name__=='__main__':main()
