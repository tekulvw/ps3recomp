#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile,sys
r=Path(__file__).resolve().parents[1]
if sys.platform=='win32':
 print('SKIP: native OSK fixture requires a POSIX C toolchain');raise SystemExit(0)
gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
with tempfile.TemporaryDirectory(prefix='native-osk-') as t:
 out=Path(t)/'fixture'
 src=[r/'tests/test_native_osk.c',r/'libs/misc/cellOskDialog.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c']
 extra=r/'libs/misc/osk_native.c'
 if extra.exists():src.append(extra)
 subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-DPS3RECOMP_TEST_NATIVE_OSK','-I'+str(r/'include'),*map(str,src),'-o',str(out),'-pthread',gc],check=True)
 subprocess.run([str(out)],check=True,timeout=10)

 if sys.platform=='darwin':
  objects=[]
  for i,p in enumerate((r/'libs/misc/cellOskDialog.c',r/'libs/misc/osk_native.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c')):
   o=Path(t)/f'ui-{i}.o';objects.append(str(o))
   subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-I'+str(r/'include'),'-c',str(p),'-o',str(o)],check=True)
  ui=Path(t)/'ui'
  subprocess.run(['clang','-fobjc-arc','-O1','-UNDEBUG','-I'+str(r/'include'),str(r/'tests/test_native_osk_macos.m'),*objects,str(r/'libs/input/pad_macos.m'),str(r/'libs/video/rsx_texture_layout.c'),'-framework','Metal','-framework','QuartzCore','-framework','AppKit','-pthread','-Wl,-dead_strip','-o',str(ui)],check=True)
  subprocess.run([str(ui)],check=True,timeout=15)
