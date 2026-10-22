#!/usr/bin/env python3
"""PS3_TPROF in-process thread profiler: samples only registered threads; off by default."""
from pathlib import Path
import os,subprocess,tempfile,platform,json
ROOT=Path(__file__).resolve().parents[1]
def main():
 if platform.system()!='Darwin' or platform.machine()!='arm64':print(json.dumps({'status':'SKIP','reason':'macOS arm64 only'}));return
 with tempfile.TemporaryDirectory(prefix='tprof-')as td:
  t=Path(td);exe=t/'tprof'
  q=subprocess.run(['cc','-O1','-g','-pthread','-I'+str(ROOT/'include'),str(ROOT/'runtime/diagnostics/tests/test_ps3_tprof.c'),'-o',str(exe)],capture_output=True,text=True);assert q.returncode==0,q.stderr
  env={k:v for k,v in os.environ.items() if not k.startswith('PS3_TPROF')}
  q=subprocess.run([str(exe)],env=env,capture_output=True,text=True);assert q.returncode==0,q.stdout+q.stderr
  prof=t/'prof.txt';env.update(PS3_TPROF=str(prof),PS3_TPROF_SECONDS='0.3')
  q=subprocess.run([str(exe)],env=env,capture_output=True,text=True);assert q.returncode==0,q.stdout+q.stderr
  q=subprocess.run([str(exe)],env={**env,'PS3_TPROF_PPU':'1','PS3_TPROF':str(t/'ppu.txt')},capture_output=True,text=True);assert q.returncode==0,q.stdout+q.stderr
  syms={}
  for l in subprocess.run(['nm','-n',str(exe)],capture_output=True,text=True).stdout.splitlines():
   p=l.split()
   if len(p)>=3 and p[1] in 'tT':syms[p[2]]=int(p[0],16)
  lo=syms['_tprof_test_spin'];hi=min([v for v in syms.values() if v>lo],default=lo+0x100)
  hits=0;n=0
  for l in prof.read_text().splitlines():
   if not l.startswith('S 7 '):continue
   n+=1;hits+=any(lo<=int(x,16)<hi for x in l.split()[3:4])
  assert n and hits*10>=n*9,(hits,n)
  print(json.dumps({'status':'PASS','spinner_samples':n,'in_spin':hits}))
if __name__=='__main__':main()
