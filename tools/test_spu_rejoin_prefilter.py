#!/usr/bin/env python3
"""Real registry/interpreter negative-prefilter regression, no title data."""
import os,subprocess
from pathlib import Path
r=Path(__file__).resolve().parents[1];spu=r/'runtime/spu'
import tempfile
_tmp=tempfile.TemporaryDirectory(prefix='spu-rejoin-prefilter-');out=Path(_tmp.name)
cc=os.environ.get('CC','cc');flags=['-std=gnu17','-O2','-UNDEBUG','-I'+str(r/'include'),'-I'+str(spu),'-I'+str(r/'runtime/platform')]
if os.environ.get('PREFILTER_UBSAN'): flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
subprocess.run([cc,*flags,'-Dspu_lifted_lookup=spu_lifted_lookup_real','-c',str(spu/'spu_channels.c'),'-o',str(out/'channels.o')],check=True)
sources=['spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c']
subprocess.run([cc,*flags,str(spu/'tests/test_spu_rejoin_prefilter.c'),str(out/'channels.o'),*[str(spu/x) for x in sources],str(r/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(out/'test')],check=True)
for flag in ['0','1']:
 for mode in ['prefilter','top','top-zero','nested-return','nested-return-unlifted','nested-calls','stop-zero','nested-stop','stop-code','halt','smc','pure-interpreter','stepcap','negative-stepcap']:
  env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))};env['SPU_INTERP_REJOIN_PREFILTER']=flag
  if mode in ('stepcap','negative-stepcap'):env.update(YDKJ_SPU_TRACE='1',SPU_STEPCAP='2')
  p=subprocess.run([str(out/'test'),mode],env=env,capture_output=True,text=True,timeout=5)
  print(flag,mode,p.returncode,p.stdout.strip(),flush=True)
  if p.returncode: print(p.stderr[-3000:]);raise SystemExit(p.returncode)
