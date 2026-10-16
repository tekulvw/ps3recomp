#!/usr/bin/env python3
"""Real-interpreter diagnostic counts and opt-out parity on a neutral loop."""
import os,pathlib,subprocess,tempfile,re
r=pathlib.Path(__file__).resolve().parents[1];s=r/'runtime/spu'
u=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
e={k:v for k,v in os.environ.items()if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-hot-check-')as td:
 for san in (False,True):
  exe=pathlib.Path(td)/('ubsan'if san else'o2')
  cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-I'+str(r/'include'),'-I'+str(s),'-I'+str(r/'runtime/platform'),str(s/'tests/test_spu_interp_hot_blocks.c'),*[str(s/(x+'.c'))for x in u],str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(exe)]
  if san:cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
  p=subprocess.run(cmd,capture_output=True,text=True,timeout=45);assert not p.returncode,p.stderr
  outputs=[]
  for mode in ('0','1'):
   p=subprocess.run([str(exe)],env=dict(e,SPU_INTERP_HOT_BLOCKS=mode),capture_output=True,text=True,timeout=10);assert not p.returncode,p.stdout+p.stderr
   lines=[x for x in p.stderr.splitlines()if x.startswith('[spu-hot-block]')]
   assert len(lines)==int(mode),p.stderr
   if lines:
    assert 'total=1048576 blocks=10000:1048576' in lines[0],lines
    assert 'ai:524288' in lines[0] and 'brnz:524288' in lines[0],lines
   outputs.append(p.stdout);print(('UBSan'if san else'O2'),mode,p.stdout.strip(),flush=True)
  assert outputs[0]==outputs[1],outputs
 print('PASS exact hot-block/opcode counts and diagnostic opt-out architectural parity',flush=True)
