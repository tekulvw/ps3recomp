#!/usr/bin/env python3
"""Headless native OSK host fixture (POSIX, no window toolkit)."""
from pathlib import Path
import os,subprocess,sys,tempfile
r=Path(__file__).resolve().parents[1]
if sys.platform in ('win32','darwin'):
    print('SKIP: the headless host is built only where no toolkit dialog exists');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='native-osk-headless-') as t:
    out=Path(t)/'fixture'
    src=[r/'tests/test_native_osk_headless.c',r/'libs/misc/cellOskDialog.c',r/'libs/misc/osk_native.c',
         r/'libs/misc/osk_headless.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c']
    subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
                    '-DPS3RECOMP_NATIVE_OSK_HEADLESS=1','-I'+str(r/'include'),*map(str,src),'-o',str(out),
                    '-pthread','-Wl,--gc-sections'],check=True)
    env=os.environ.copy();env['OSK_NATIVE_CONTROL_FILE']=str(Path(t)/'control');env['PS3RECOMP_OSK_INITIAL_TEXT']='PlayerRetail'
    subprocess.run([str(out)],check=True,timeout=10,env=env)
