#!/usr/bin/env python3
"""Lift neutral instructions, then test guarded transfers in the real runtime."""
import json,os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spu=root/'runtime/spu'
cc=os.environ.get('CC','cc')
span_mode=os.environ.get('SPU_VERIFIED_SPANS')=='1'
span_args=['--verified-code-spans'] if span_mode else []
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
# STQD r10,0(r11); BRSL r6,callee; AI r7,r7,1; BI r0.
# Callee: IL r7,22; BI r6. Gaps are deliberately not executable segments.
words=[0x24000000|(11<<7)|10,0x33000000|(7<<7)|6,
       0x1C000000|(1<<14)|(7<<7)|7,0x35000000,0,0,0,0,
       0x40800000|(22<<7)|7,0x35000000|(6<<7)]
with tempfile.TemporaryDirectory(prefix='spu-verified-overlay-') as td:
 work=Path(td);raw=b''.join(w.to_bytes(4,'big') for w in words)
 (work/'program.bin').write_bytes(raw)
 (work/'bounds.json').write_text(json.dumps([{'start':'0x10000','end':'0x10010'},{'start':'0x10020','end':'0x10028'}]))
 command=[sys.executable,str(root/'tools/spu_lifter.py'),str(work/'program.bin'),'--base','0x10000','--functions',str(work/'bounds.json'),'--symbol-prefix','verified_test_','--output',str(work)]
 lift=subprocess.run(command+['--verified-code-image','701',*span_args],cwd=root,env=env,capture_output=True,text=True)
 if lift.returncode:print(lift.stdout,lift.stderr);raise SystemExit(lift.returncode)
 # Disabled/default output is byte identical; invalid ids create no output.
 for mode,extra in [('default',[]),('zero',['--verified-code-image','0'])]:
  where=work/mode;subprocess.run(command+['--output',str(where),*extra],check=True,cwd=root,env=env,capture_output=True)
 assert (work/'default/spu_recomp.c').read_bytes()==(work/'zero/spu_recomp.c').read_bytes()
 for value in ['-1','2147483648']:
  where=work/('invalid'+value)
  r=subprocess.run(command+['--output',str(where),'--verified-code-image',value],cwd=root,env=env,capture_output=True)
  assert r.returncode!=0 and not where.exists()
 invalid_span=work/'invalid-span'
 invalid=subprocess.run(command+['--verified-code-spans','--output',str(invalid_span)],cwd=root,env=env,capture_output=True)
 assert invalid.returncode!=0 and not invalid_span.exists()
 h=work/'spu_recomp.h';h.write_text(h.read_text().replace('../../runtime/spu/','').replace('../runtime/spu/',''))
 # A real generated guarded function deliberately outside its registered text.
 # Legacy ownership must not repeatedly serve it after its guard rejects.
 (work/'outside.bin').write_bytes((0x40800000|(66<<7)|7).to_bytes(4,'big')+(0x35000000).to_bytes(4,'big'))
 (work/'outside-bounds.json').write_text('[{"start":"0x20000","end":"0x20008"}]')
 subprocess.run([sys.executable,str(root/'tools/spu_lifter.py'),str(work/'outside.bin'),'--base','0x20000','--functions',str(work/'outside-bounds.json'),'--symbol-prefix','outside_test_','--header-name','outside_recomp.h','--source-name','outside_recomp.c','--output',str(work),'--verified-code-image','701',*span_args],check=True,cwd=root,env=env,capture_output=True)
 oh=work/'outside_recomp.h';oh.write_text(oh.read_text().replace('../../runtime/spu/','').replace('../runtime/spu/',''))
 if span_mode:
  tail_words=[0x24000000|(11<<7)|10,0x32000000|(7<<7),0,0,0,0,0,0,0x40800000|(22<<7)|7,0x35000000]
  tail_raw=b''.join(w.to_bytes(4,'big') for w in tail_words)
  (work/'tail.bin').write_bytes(tail_raw)
  (work/'tail-bounds.json').write_text('[{"start":"0x22000","end":"0x22008"},{"start":"0x22020","end":"0x22028"}]')
  subprocess.run([sys.executable,str(root/'tools/spu_lifter.py'),str(work/'tail.bin'),'--base','0x22000','--functions',str(work/'tail-bounds.json'),'--symbol-prefix','tail_test_','--header-name','tail_recomp.h','--source-name','tail_recomp.c','--output',str(work),'--verified-code-image','705',*span_args],check=True,cwd=root,env=env,capture_output=True)
  th=work/'tail_recomp.h';th.write_text(th.read_text().replace('../../runtime/spu/','').replace('../runtime/spu/',''))
  (work/'tail_program.h').write_text('static const unsigned char tail_program[] = {'+','.join(str(b) for b in tail_raw)+'};\n')
 # Store original neutral byte image for independent actual-LS expectations.
 (work/'verified_program.h').write_text('static const unsigned char verified_program[] = {'+','.join(str(b) for b in raw)+'};\n')
 runtime=(spu/'spu_channels.c').read_bytes()
 needle=b'memcmp(ctx->ls + s->lsa, s->bytes, s->size)'
 assert runtime.count(needle)==1
 instrumented=work/'instrumented-channels.c'
 instrumented.write_bytes(b'unsigned test_verified_compares; unsigned long long test_verified_bytes;\n'+runtime.replace(needle,b'(++test_verified_compares, test_verified_bytes += s->size, memcmp(ctx->ls + s->lsa, s->bytes, s->size))').replace(b'memcmp(ctx->ls + entry, s->bytes + offset, end - entry)',b'(++test_verified_compares, test_verified_bytes += end - entry, memcmp(ctx->ls + entry, s->bytes + offset, end - entry))'))
 sources=['runtime/spu/tests/test_spu_lifted_start.c','runtime/spu/spu_channels.c','runtime/spu/spu_drain.c','runtime/spu/spu_lockstep.c','runtime/spu/spu_coherency.c','runtime/spu/spu_workload.c','runtime/spu/spurs_policy.c','runtime/spu/spurs_job.c','runtime/spu/spu_tsp_weak.c','runtime/spu/spu_vm_pagemap.c','runtime/spu/spurs_policy_blob_weak.c','runtime/spu/spu_interp.c','runtime/spu/spu_lifted_thread.c','runtime/syscalls/spu_fallback.c','runtime/syscalls/sdk_version_weak.c','runtime/platform/win32_compat.c']
 flags=['-std=gnu17','-O2','-UNDEBUG','-DSPU_VERIFIED_OVERLAY_TEST','-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),'-I'+str(work)]
 if span_mode:flags+=['-DSPU_VERIFIED_SPANS_TEST']
 if os.environ.get('UBSAN')=='1':flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
 def run(source,label):
  binary=work/label
  build=subprocess.run([cc,*flags,*[(str(instrumented) if s=='runtime/spu/spu_channels.c' else str(root/s)) for s in sources],str(source),str(work/'outside_recomp.c'),*([str(work/'tail_recomp.c')] if span_mode else []),'-lpthread','-o',str(binary)],capture_output=True,text=True)
  if build.returncode:print(build.stdout,build.stderr);raise SystemExit(build.returncode)
  r=subprocess.run([str(binary)],capture_output=True,text=True,timeout=20,env=env)
  summary=[line for line in r.stdout.splitlines() if 'passed' in line or 'FAIL:' in line]
  print(label,'\n'.join(summary),flush=True)
  if r.returncode and 'FAIL:' not in r.stdout:print(r.stdout,r.stderr)
  return r
 original=work/'spu_recomp.c';r=run(original,'guarded');assert r.returncode==0
 if os.environ.get('MUTANT')=='1':
  # Preserve the new registry but reproduce pre-feature direct function bodies.
  src=original.read_text();lines=src.splitlines();removed=[l for l in lines if 'spu_verified_code_enter' in l and '(ctx,' in l];assert len(removed)==2
  mutant=work/'unguarded.c';mutant.write_text('\n'.join(l for l in lines if l not in removed)+'\n')
  r=run(mutant,'unguarded-negative-control');assert r.returncode!=0 and 'FAIL:' in r.stdout
 print('PASS: generated direct-call guard and production interpreter fallback')
