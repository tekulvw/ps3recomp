#!/usr/bin/env python3
"""Real legacy/region direct-call -> local tail -> indirect fallback test.

All instructions are neutral encodings. No guest files, native application,
dispatcher mocks, or modified runtime objects are used. An observing wrapper
records fallback stop PCs and delegates to the real interpreter unchanged.
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
COMMON=['spu_drain.c','spu_lockstep.c','spu_coherency.c','spu_workload.c',
        'spurs_policy.c','spurs_job.c','spu_tsp_weak.c','spu_vm_pagemap.c',
        'spurs_policy_blob_weak.c','spu_interp.c']

def ai(reg):
    return 0x1c000000|(1<<14)|(reg<<7)|reg

def run():
    env={k:v for k,v in os.environ.items()
         if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
    with tempfile.TemporaryDirectory(prefix='spu-nested-fallback-') as td:
        p=pathlib.Path(td)
        words=[0x40200000]*13  # excluded gap words never become lifted entries
        program={0x10000:0x31000000|((0x10020>>2)<<7)|6,
                 0x10004:ai(20),0x10008:0x35000000,
                 0x10020:ai(21),0x10024:0x32000080,
                 0x10028:0x35200000|(11<<7)|7,
                 0x1002c:ai(22),0x10030:0x35000000|(6<<7)}
        for pc,word in program.items():words[(pc-0x10000)//4]=word
        blob=struct.pack('>13I',*words)
        (p/'program.bin').write_bytes(blob)
        (p/'nested_program.h').write_text('static const unsigned char nested_program[]={'+','.join(map(str,blob))+'};\n')
        bounds=[(0x10000,0x1000c),(0x10020,0x10028),(0x10028,0x10034)]
        (p/'bounds.json').write_text(json.dumps([{'start':hex(a),'end':hex(b)} for a,b in bounds]))
        for name,extra in [('legacy',[]),('region',['--connected-regions'])]:
            out=p/name
            result=subprocess.run([sys.executable,str(ROOT/'tools/spu_lifter.py'),
                str(p/'program.bin'),'--base','0x10000','--functions',str(p/'bounds.json'),
                '--symbol-prefix',f'nested_{name}_','--verified-code-image','701',
                '--verified-code-spans','--output',str(out),*extra],capture_output=True,text=True)
            assert result.returncode==0,result.stdout+result.stderr
            for f in (out/'spu_recomp.c',out/'spu_recomp.h'):
                f.write_text(f.read_text().replace('../../runtime/spu/',''))
            source=(out/'spu_recomp.c').read_text()
            assert 'SPU_RET_REG(ctx, 6)' in source,source
            assert 'spu_drain_call(ctx, 0x1002C)' in source,source
            if name=='region':
                assert 'spu_region_local_step(ctx, nested_region_spu_func_00010028)' in source,source
        flags=['-std=gnu17','-O2','-UNDEBUG','-I'+str(ROOT/'include'),
               '-I'+str(SPU),'-I'+str(ROOT/'runtime/platform'),'-I'+str(p)]
        if os.environ.get('UBSAN')=='1':
            flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        build=subprocess.run([os.environ.get('CC','cc'),*flags,
            str(SPU/'tests/test_spu_region_nested_fallback.c'),
            *[str(SPU/c) for c in COMMON],str(ROOT/'runtime/platform/win32_compat.c'),
            str(p/'legacy/spu_recomp.c'),str(p/'region/spu_recomp.c'),
            '-pthread','-lm','-o',str(p/'test')],capture_output=True,text=True)
        assert build.returncode==0,build.stdout+build.stderr
        failures=[]
        for irq in (0,1):
            signatures=[]
            stop_pcs=[]
            for mode in (0,1):
                result=subprocess.run([str(p/'test'),str(mode),str(irq)],
                    env=env,capture_output=True,text=True,timeout=15)
                print(result.stdout.strip(),flush=True)
                if result.returncode:
                    print(result.stderr,flush=True)
                    failures.append((irq,mode,result.returncode))
                signature=re.search(r'pc=(\w+).*counters=([\d,]+) regs=(\w+)',result.stdout)
                assert signature,result.stdout+result.stderr
                signatures.append(signature.groups())
                stops=re.search(r'first_stop=(\w+) last_stop=(\w+)',result.stdout)
                assert stops,result.stdout
                stop_pcs.append(stops.groups())
            assert signatures[0]==signatures[1],signatures
            # This fixture starts with a live translated frame. Conservative
            # region admission must retain its original owning call drain.
            assert stop_pcs[0]==stop_pcs[1]==('10004','10004'),stop_pcs
        assert not failures,failures
        print('PASS: both emitters execute all four continuations once through real indirect fallback')

if __name__=='__main__':run()
