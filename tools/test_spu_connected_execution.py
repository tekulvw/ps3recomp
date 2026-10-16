#!/usr/bin/env python3
"""Run the same neutral program through legacy, connected and interpreter paths."""
import os,pathlib,re,struct,subprocess,sys,tempfile,json
root=pathlib.Path(__file__).resolve().parents[1];spu=root/'runtime/spu'
common=['spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c','spurs_policy_blob_weak.c','spu_interp.c']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
env['SPU_REGION_FRAMED']=os.environ.get('SPU_REGION_FRAMED','0')
with tempfile.TemporaryDirectory(prefix='spu-region-exec-') as td:
 p=pathlib.Path(td);words=[0x40800007,0x40800188,0x1c004387,0x1cffc408,0x217fff08,0x35000000]
 blob=struct.pack('>6I',*words);(p/'program.bin').write_bytes(blob)
 (p/'region_program.h').write_text('static const unsigned char region_program[]={'+','.join(str(x) for x in blob)+'};\n')
 (p/'bounds.json').write_text(json.dumps([{'start':hex(a),'end':hex(b)} for a,b in [(0x10000,0x10008),(0x10008,0x10010),(0x10010,0x10014),(0x10014,0x10018)]]))
 for name,extra in [('legacy',[]),('region',['--connected-regions'])]:
  out=p/name;subprocess.run([sys.executable,str(root/'tools/spu_lifter.py'),str(p/'program.bin'),'--base','0x10000','--functions',str(p/'bounds.json'),'--symbol-prefix',name+'_test_','--verified-code-image','701','--verified-code-spans','--output',str(out),*extra],check=True,capture_output=True)
  for f in (out/'spu_recomp.c',out/'spu_recomp.h'):f.write_text(f.read_text().replace('../../runtime/spu/',''))
 flags=['-std=gnu17','-O2','-UNDEBUG','-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),'-I'+str(p)]
 if os.environ.get('UBSAN')=='1':flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
 build=subprocess.run([os.environ.get('CC','cc'),*flags,str(spu/'tests/test_spu_connected_regions.c'),*[str(spu/c) for c in common],str(root/'runtime/platform/win32_compat.c'),str(p/'legacy/spu_recomp.c'),str(p/'region/spu_recomp.c'),'-pthread','-lm','-o',str(p/'test')],capture_output=True,text=True)
 if build.returncode:print(build.stdout,build.stderr);raise SystemExit(build.returncode)
 for depth in ('0','1'):
  for entry in ('0x10000','0x10004','0x10008','0x1000c','0x10010','0x10014'):
   for irq in ('0','1'):
    outputs=[]
    for mode in ('0','1'):
     r=subprocess.run([str(p/'test'),mode,irq,entry,depth],env=env,capture_output=True,text=True,timeout=15)
     print('mode',mode,'irq',irq,'entry',entry,'depth',depth,r.stdout.strip(),flush=True)
     if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
     m=re.search(r'RESULT pc=(\w+) depth=(\d+) irq=(\d+) regs=(\w+) steps=(\d+) failed=(\d+)',r.stdout);assert m,r
     outputs.append(m.groups())
    assert outputs[0][:4]==outputs[1][:4],outputs
    if entry=='0x10000':assert outputs[0][4]==outputs[1][4],outputs
 print('PASS: legacy/regions match architectural state; original entry boundary-step counts match')
