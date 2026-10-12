#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys,tempfile
r=Path(__file__).resolve().parents[1]
if sys.platform=='win32':
 print('SKIP: NP EULA fixture requires POSIX C toolchain');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='np-eula-') as d:
 p=Path(d);gen=p/'nids.cpp'
 subprocess.run([sys.executable,str(r/'tools/gen_hle_nids.py'),'--out',str(gen),'cellSysutilNpEula'],check=True)
 objects=[]
 for i,s in enumerate([r/'tests/test_np_eula.c',r/'libs/system/cellSysutilNpEula.c',r/'libs/system/cellSysutil.c']):
  o=p/f'{i}.o';objects.append(str(o))
  subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I'+str(r/'include'),'-c',str(s),'-o',str(o)],check=True)
 gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
 exe=p/'fixture';subprocess.run(['c++',str(gen),*objects,'-pthread',gc,'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
