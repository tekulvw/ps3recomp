#!/usr/bin/env python3
"""Bounded actual-drain ownership checks, no guest assets/application."""
import os,pathlib,subprocess,tempfile
r=pathlib.Path(__file__).resolve().parents[1];spu=r/'runtime/spu'
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items()if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-region-scope-')as td:
 for sanitize in (False,True):
  exe=pathlib.Path(td)/('ubsan'if sanitize else'o2')
  cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-I'+str(r/'include'),'-I'+str(spu),'-I'+str(r/'runtime/platform'),str(spu/'tests/test_spu_region_scope.c'),*[str(spu/(u+'.c'))for u in units],str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(exe)]
  if sanitize:cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
  p=subprocess.run(cmd,capture_output=True,text=True,timeout=45);assert not p.returncode,p.stdout+p.stderr
  for mode in ('0','1'):
   p=subprocess.run([str(exe),mode],env=dict(env,SPU_REGION_FRAMED=mode),capture_output=True,text=True,timeout=10);print(p.stdout.strip());assert not p.returncode,p.stdout+p.stderr
  p=subprocess.run([str(exe),'1'],env=dict(env,SPU_REGION_FRAMED='0'),capture_output=True,text=True,timeout=10)
  assert p.returncode==1,p.stdout+p.stderr
  print('PASS positive-control fixture rejects missing framed admission',flush=True)
