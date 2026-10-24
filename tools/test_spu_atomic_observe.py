#!/usr/bin/env python3
"""Bounded real-runtime observer fixture, never starts native/game services."""
import argparse,json,os,pathlib,subprocess,tempfile,shutil
p=argparse.ArgumentParser();p.add_argument("--ubsan",action="store_true");p.add_argument("--artifacts",type=pathlib.Path);args=p.parse_args()
ROOT=pathlib.Path(__file__).resolve().parents[1]; SPU=ROOT/'runtime/spu'
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','PS3_SPU_','PS3_TPROF','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-atomic-observe-') as td:
    tmp=pathlib.Path(td);exe=tmp/'test'
    cmd=['cc','-std=gnu17','-O2','-UNDEBUG','-I'+str(ROOT/'include'),'-I'+str(SPU),'-I'+str(ROOT/'runtime/platform'),str(SPU/'tests/test_spu_atomic_observe.c'),*[str(SPU/(u+'.c')) for u in units],str(ROOT/'runtime/platform/win32_compat.c'),str(ROOT/'runtime/diagnostics/job_io_recorder.c'),str(ROOT/'runtime/diagnostics/critical_trace.c'),'-pthread','-lm','-o',str(exe)]
    if args.ubsan:cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
    r=subprocess.run(cmd,capture_output=True,text=True,timeout=60);assert not r.returncode,r.stdout+r.stderr
    results=[]
    for mode in [0,1]:
        output=tmp/f'observe-{mode}.json';e=dict(env)
        if mode:e['SPU_ATOMIC_OBSERVE']='1'
        r=subprocess.run([str(exe),str(output),str(mode)],env=e,capture_output=True,text=True,timeout=10)
        print(mode,r.returncode,r.stdout.strip(),r.stderr.strip(),flush=True)
        assert not r.returncode,r.stdout+r.stderr
        results.append(next(x for x in r.stdout.splitlines() if x.startswith('OBSERVE_GUEST')))
        if mode:
            data=json.loads(output.read_text());print(json.dumps(data,sort_keys=True),flush=True)
            assert data['commands']==6 and data['completed_after_stop']==0,data
            rows=data['reports'][0]['rows']
            assert {(x['ctx_image_id_snapshot'],x['ctx_pc_snapshot'],x['cmd'],x['outcome']) for x in rows}=={(209,0xC77C,0xD0,4),(2,0x2488,0xD0,4),(209,0xC77C,0xB4,0),(2,0x2488,0xB4,1),(209,0xC77C,0xB0,2),(209,0xC77C,0xB8,2)},rows
            assert all(x['ea']==0x20000 and x['current_executing_image']=='unknown' and x['n']==1 for x in rows),rows
            assert output.stat().st_mode&0o777==0o600
        else:assert not output.exists()
    assert results[0]==results[1],results
    for name in ['overflow','budget','bounds','late','lateack','sample','cleanup','control']:
        output=tmp/(name+'.json')
        r=subprocess.run([str(exe),str(output),'1',name],env=dict(env,SPU_ATOMIC_OBSERVE='1'),capture_output=True,text=True,timeout=10)
        print(name,r.returncode,r.stdout.strip(),r.stderr.strip(),flush=True)
        assert not r.returncode,r.stdout+r.stderr
        data=json.loads(output.read_text())
        if name=='overflow':
            p=data['reports'][0];assert data['commands']==160 and p['key_overflow']>0
            assert sum(x['n'] for x in p['rows'])+p['key_overflow']==160
        if name=='budget':assert data['commands']==2 and data['reports'][0]['budget_exhausted']
        if name=='control':
            assert data['commands']==16,data
            old=json.loads(pathlib.Path(str(output)+'.critical').read_text());assert old['capture_id']==2
        if name=='cleanup':
            assert data['reported_threads']==0 and data['missing_unknown']==32 and data['thread_capacity_overflow']==1,data
        if name=='sample':
            r=data['reports'][0]['rows'][0];assert data['commands']==2048 and r['locks']==2048
            assert 1<=r['timed_locks']<32 and r['sample_hold_ns']>0 and r['sample_wait_ns']>0,r
            assert r['sample_watch_callbacks']==r['timed_locks'],r
        if name=='lateack':assert data['commands']==1 and data['completed_after_stop']==1,data
        if name=='late':
            assert data['commands']==0 and data['missing_unknown']==1,data
            nxt=json.loads(pathlib.Path(str(output)+'.next').read_text())
            assert nxt['commands']==1 and nxt['reports'][0]['rows'][0]['ea']==0x20100,nxt
    if args.artifacts:
        args.artifacts.mkdir(parents=True,exist_ok=True)
        for item in tmp.iterdir():
            if item.is_file():shutil.copy2(item,args.artifacts/item.name)
        (args.artifacts/'build.private.json').write_text(json.dumps({'argv':cmd,'cwd':str(ROOT),'ubsan':args.ubsan,'same_executable_as_run':True},indent=2)+'\n')
    print('PASS observer real atomic on/off equivalence and publication boundaries',flush=True)
