#!/usr/bin/env python3
"""Checkpoint LS mutation must precede the full destination body guard.

Only a temporary copy of the real drain's task-hook definition is renamed;
the observing fixture delegates unchanged and then changes neutral guest bytes.
Persistent runtime/emitter/shared fixtures are not modified.
"""
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[1]
SPU=ROOT/'runtime/spu'
COMMON=['spu_lockstep.c','spu_coherency.c','spu_workload.c','spurs_policy.c',
        'spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c',
        'spurs_policy_blob_weak.c','spu_interp.c']

def run():
    env={k:v for k,v in os.environ.items()
         if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
    with tempfile.TemporaryDirectory(prefix='spu-checkpoint-mutation-') as td:
        p=pathlib.Path(td)
        words=[0x1c000000|(1<<14)|(20<<7)|20,0x32000080,
               0x04000000|(7<<14)|(22<<7)|21,0x35000000]
        blob=struct.pack('>4I',*words)
        (p/'program.bin').write_bytes(blob)
        (p/'mutation_program.h').write_text('static const unsigned char mutation_program[]={'+','.join(map(str,blob))+'};\n')
        (p/'bounds.json').write_text(json.dumps([
            {'start':'0x10000','end':'0x10008'},
            {'start':'0x10008','end':'0x10010'}]))
        for name,extra in [('legacy',[]),('region',['--connected-regions'])]:
            out=p/name
            result=subprocess.run([sys.executable,str(ROOT/'tools/spu_lifter.py'),
                str(p/'program.bin'),'--base','0x10000','--functions',str(p/'bounds.json'),
                '--symbol-prefix',f'mutation_{name}_','--verified-code-image','701',
                '--verified-code-spans','--output',str(out),*extra],capture_output=True,text=True)
            assert result.returncode==0,result.stdout+result.stderr
            for f in (out/'spu_recomp.c',out/'spu_recomp.h'):
                f.write_text(f.read_text().replace('../../runtime/spu/',''))
            if name=='region':
                assert 'spu_region_local_step(ctx, mutation_region_spu_func_00010008)' in (out/'spu_recomp.c').read_text()
        drain=(SPU/'spu_drain.c').read_text()
        drain,count=re.subn(r'\bvoid spu_task_launch_check\(',
                            'void mutation_real_task_launch_check(',drain)
        assert count==1,count
        (p/'observed_drain.c').write_text(drain)
        flags=['-std=gnu17','-O2','-UNDEBUG','-I'+str(ROOT/'include'),
               '-I'+str(SPU),'-I'+str(ROOT/'runtime/platform'),'-I'+str(p)]
        if os.environ.get('UBSAN')=='1':
            flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        build=subprocess.run([os.environ.get('CC','cc'),*flags,
            str(SPU/'tests/test_spu_region_checkpoint_mutation.c'),str(p/'observed_drain.c'),
            *[str(SPU/c) for c in COMMON],str(ROOT/'runtime/platform/win32_compat.c'),
            str(p/'legacy/spu_recomp.c'),str(p/'region/spu_recomp.c'),
            '-pthread','-lm','-o',str(p/'test')],capture_output=True,text=True)
        assert build.returncode==0,build.stdout+build.stderr
        signatures=[]
        for mode in (0,1):
            result=subprocess.run([str(p/'test'),str(mode)],env=env,
                capture_output=True,text=True,timeout=15)
            print(result.stdout.strip(),flush=True)
            assert result.returncode==0,result.stdout+result.stderr
            signature=re.search(r'pc=(\w+) counters=([\d,]+) regs=(\w+)',result.stdout)
            assert signature,result.stdout
            signatures.append(signature.groups())
        assert signatures[0]==signatures[1],signatures
        print('PASS: real checkpoint mutation rejects original destination; real interpreter executes changed code; sensitive helper entries deopt before hooks')

if __name__=='__main__':run()
