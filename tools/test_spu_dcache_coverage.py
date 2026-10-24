#!/usr/bin/env python3
"""Real cache/resolver preservation; temporary source copies supply observers."""
import argparse,os,pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1];SPU=ROOT/'runtime/spu'
parser=argparse.ArgumentParser();parser.add_argument('--mutants',action='store_true');args=parser.parse_args()
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
original=(SPU/'spu_channels.c').read_text()
with tempfile.TemporaryDirectory(prefix='spu-dcache-coverage-') as td:
    p=pathlib.Path(td)
    fixture=(SPU/'tests/test_spu_mfc_slots.c').read_text()
    fixture=fixture.replace('#include "../spu_channels.c"','#include "coverage_channels.c"')
    fixture=fixture.replace('#include "../spu_lifted_job.h"','#include "'+str(SPU/'spu_lifted_job.h')+'"')
    (p/'coverage_fixture.h').write_text(fixture)
    def run(name,source,sanitize=False,midpublication=False):
        source=source.replace('#include "../','#include "'+str(SPU.parent)+'/')
        needle='static spu_lifted_fn spu_lifted_lookup_full(const spu_context* ctx, uint32_t lsa)\n{'
        assert source.count(needle)==1
        source=source.replace(needle,needle+'\n    extern unsigned coverage_full_calls; ++coverage_full_calls;')
        needle='uint8_t* copy = (uint8_t*)malloc(total);'
        assert source.count(needle)==1
        source=source.replace(needle,'extern void* coverage_malloc(size_t);\n    uint8_t* copy = (uint8_t*)coverage_malloc(total);')
        flags=[]
        if 's_verified_generation' in source:flags+=['-DSPU_DCACHE_GENERATION_TEST']
        if 'spu_dcache_coverage_mask' in source:
            needle='static uint16_t spu_dcache_coverage_mask(uint32_t lsa, unsigned count)\n{'
            assert source.count(needle)==1
            source=source.replace(needle,needle+'\n    extern void coverage_publish_observe(void); coverage_publish_observe();')
            if midpublication:flags+=['-DSPU_DCACHE_MIDPUBLICATION_TEST']
        (p/'coverage_channels.c').write_text(source)
        exe=p/name
        if sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG',*flags,'-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(SPU),'-I'+str(ROOT/'runtime/platform'),str(SPU/'tests/test_spu_dcache_coverage.c'),*[str(SPU/(u+'.c')) for u in units],str(ROOT/'runtime/platform/win32_compat.c'),str(ROOT/'runtime/diagnostics/job_io_recorder.c'),'-pthread','-lm','-o',str(exe)]
        build=subprocess.run(cmd,capture_output=True,text=True,timeout=60);assert not build.returncode,build.stdout+build.stderr
        results=[]
        for mode in (['1','shadow'] if midpublication else ['0','1','shadow']):
            result=subprocess.run([str(exe)],env=dict(env,SPU_DISPATCH_CACHE=mode),capture_output=True,text=True,timeout=10)
            print(name,mode,result.returncode,result.stdout.strip(),flush=True);results.append(result)
        return results
    for name,san in [('o2',False),('ubsan',True)]:
        for result in run(name,original,san):assert not result.returncode,result.stdout+result.stderr
    if 'spu_dcache_coverage_mask' in original:
        for result in run('midpublication',original,midpublication=True):assert not result.returncode,result.stdout+result.stderr
    if args.mutants:
        controls={
            'epochs':('t->vepoch[i]++;','/* bug: omitted code epoch */;'),
            'lifetime':('e->id == id &&','/* bug: lifetime omitted */'),
            'registry':('e->rgen == rgen &&','/* bug: registry omitted */'),
        }
        for name,(old,new) in controls.items():
            assert original.count(old)==1,(name,original.count(old))
            mutant=original.replace(old,new)
            if name=='lifetime':
                # A lifetime-blind cache also hashes independently of the new
                # lifetime id, as a raw-pointer keyed cache would at this address.
                needle='((lsa >> 2) ^ id)'
                assert mutant.count(needle)==1
                mutant=mutant.replace(needle,'((lsa >> 2) ^ 0u)')
            results=run('mutant-'+name,mutant)
            assert any(r.returncode and 'FAIL:' in r.stdout for r in results),(name,results)
            print('PASS rejected bug control:',name,flush=True)
        if 'spu_dcache_coverage_mask' in original:
            controls={
                'overlap-mask':('if(lsa>=s_verified_images[i].lo && lsa<s_verified_images[i].hi)',
                                'if(i==0 && lsa>=s_verified_images[i].lo && lsa<s_verified_images[i].hi)'),
                'odd-publication':('if(vregistration&1u)return spu_lifted_lookup_full(ctx,lsa);','/* bug: accepts incomplete publication */'),
                'mixed-store':('if (fn && __atomic_load_n(&s_verified_generation,__ATOMIC_ACQUIRE)==vregistration) {','if (fn) {'),
            }
            for name,(old,new) in controls.items():
                assert original.count(old)==1,(name,original.count(old))
                results=run('mutant-'+name,original.replace(old,new),midpublication=name=='mixed-store')
                assert any(r.returncode and 'FAIL:' in r.stdout for r in results),(name,results)
                print('PASS rejected bug control:',name,flush=True)
    print('PASS real dispatch coverage preservation',flush=True)
