#!/usr/bin/env python3
"""Real registry, exact changed-byte, compare-volume and owning-return checks."""
import pathlib,subprocess,tempfile,os
r=pathlib.Path(__file__).resolve().parents[1];s=r/'runtime/spu'
u=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
e={k:v for k,v in os.environ.items()if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-leaf-check-')as td:
 for san in (False,True):
  exe=pathlib.Path(td)/('ubsan'if san else'o2');cmd=['cc','-std=gnu17','-O2','-UNDEBUG','-I'+str(r/'include'),'-I'+str(s),'-I'+str(r/'runtime/platform'),str(s/'tests/test_spu_verified_leaf.c'),*[str(s/(x+'.c'))for x in u],str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(exe)]
  if san:cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
  p=subprocess.run(cmd,capture_output=True,text=True,timeout=45);assert not p.returncode,p.stderr
  for mode in ('0','1'):
   p=subprocess.run([str(exe),mode],env=dict(e,SPU_VERIFIED_LEAF_BLOCKS=mode,SPU_REGION_FRAMED="1"),capture_output=True,text=True,timeout=10);print(('UBSan'if san else'O2'),mode,p.stdout.strip(),flush=True);assert not p.returncode,p.stdout+p.stderr
