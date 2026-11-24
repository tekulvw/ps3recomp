#!/usr/bin/env python3
from pathlib import Path
import os, subprocess, sys, tempfile
r=Path(__file__).resolve().parents[1]
if sys.platform=='win32':
 print('SKIP: NP ticket refill fixture needs POSIX C toolchain');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='np-ticket-refill-') as d:
 p=Path(d);gen=p/'nids.cpp'
 subprocess.run([sys.executable,str(r/'tools/gen_hle_nids.py'),'--out',str(gen),'sceNp'],check=True,timeout=300)
 objects=[]
 for i,s in enumerate([r/'tests/test_np_ticket_refill.c',r/'libs/network/sceNp.c',r/'libs/network/np_psnr.c',r/'libs/network/psnr/psnr.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c']):
  o=p/f'{i}.o';objects.append(str(o))
  subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I'+str(r/'include'),'-c',str(s),'-o',str(o)],check=True,timeout=300)
 gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
 exe=p/'fixture';subprocess.run(['c++',str(gen),*objects,'-pthread',gc,'-o',str(exe)],check=True,timeout=300)
 env={k:v for k,v in os.environ.items() if not k.startswith(('PS3_','PS3RECOMP_','PSNR_'))}
 files=p/'tickets';files.mkdir()
 subprocess.run([str(exe),str(files)],check=True,timeout=60,env=env)
