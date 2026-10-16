#!/usr/bin/env python3
"""Generated raw blocks match real interpreter GPR/LS/control state."""
import pathlib,subprocess,tempfile,os,sys,json,struct
from spu_leaf_blocks import emit
r=pathlib.Path(__file__).resolve().parents[1];s=r/'runtime/spu';u=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
e={k:v for k,v in os.environ.items()if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
words=[0x1c000000|(1<<14)|(20<<7)|20,0x34000000|(10<<7)|9,0x24000000|(1<<14)|(10<<7)|9,0xe0000000|(33<<21)|(31<<14)|(30<<7)|32,0xb0000000|(34<<21)|(11<<14)|(9<<7)|13,0x1c000000|(0x3ff<<14)|(8<<7)|8,0x21000000|(((-6)&0xffff)<<7)|8,0x35000000|(6<<7),0x40200000]
blob=struct.pack('>9I',*words);bounds=[(0x10000,0x1001c),(0x1001c,0x10020)]
with tempfile.TemporaryDirectory(prefix='spu-leaf-exec-')as td:
 p=pathlib.Path(td);(p/'program.bin').write_bytes(blob);(p/'bounds.json').write_text(json.dumps([{'start':hex(a),'end':hex(b)}for a,b in bounds]));(p/'leaf_program.h').write_text('static const uint8_t leaf_program[]={'+','.join(str(v)for v in blob)+'};\n')
 z=subprocess.run([sys.executable,str(r/'tools/spu_lifter.py'),str(p/'program.bin'),'--base','0x10000','--functions',str(p/'bounds.json'),'--symbol-prefix','legacy_test_','--verified-code-image','701','--verified-code-spans','--output',str(p/'legacy')],capture_output=True,text=True);assert not z.returncode,z.stderr
 for f in (p/'legacy/spu_recomp.c',p/'legacy/spu_recomp.h'):f.write_text(f.read_text().replace('../../runtime/spu/',''))
 h,c,m=emit(blob,0x10000,bounds,701,'raw_test_');assert len(m['units'])==2,m
 (p/'raw').mkdir();(p/'raw/spu_recomp.h').write_text(h);(p/'raw/spu_recomp.c').write_text('#define spu_verified_leaf_enter observed_leaf_enter\n'+c)
 for san in (False,True):
  exe=p/('ubsan'if san else'o2');cmd=['cc','-std=gnu17','-O2','-UNDEBUG','-ffp-contract=off','-I'+str(r/'include'),'-I'+str(s),'-I'+str(r/'runtime/platform'),'-I'+str(p),str(s/'tests/test_spu_leaf_execution.c'),*[str(s/(x+'.c'))for x in u],str(r/'runtime/platform/win32_compat.c'),str(p/'raw/spu_recomp.c'),str(p/'legacy/spu_recomp.c'),'-pthread','-lm','-o',str(exe)]
  if san:cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
  z=subprocess.run(cmd,capture_output=True,text=True,timeout=45);assert not z.returncode,z.stderr
  for mode in ('0','1'):
   z=subprocess.run([str(exe),mode],env=dict(e,SPU_VERIFIED_LEAF_BLOCKS=mode,SPU_REGION_FRAMED="1"),capture_output=True,text=True,timeout=10);print(('UBSan'if san else'O2'),mode,z.stdout.strip(),flush=True);assert not z.returncode,z.stdout+z.stderr
