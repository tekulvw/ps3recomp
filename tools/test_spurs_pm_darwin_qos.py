#!/usr/bin/env python3
"""Real Apple pthread QoS through actual PM worker entry; no game process."""
import argparse,json,os,pathlib,platform,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output-dir',type=pathlib.Path)
p.add_argument('--case',choices=['default','invalid','off','on','set-error','get-error','id-error','cap']);a=p.parse_args()
if platform.system()!='Darwin':print('SKIP real Apple pthread QoS fixture');raise SystemExit(0)
def run(out):
    units=['spu_channels','spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
    exe=out/'fixture'
    cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-ffunction-sections','-fdata-sections','-Wl,-dead_strip','-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu'),'-I'+str(ROOT/'runtime/platform'),str(ROOT/'tests/test_spurs_pm_darwin_qos.c'),str(ROOT/'libs/spurs/spurs_taskset.c'),*[str(ROOT/'runtime/spu'/(u+'.c')) for u in units],str(ROOT/'runtime/platform/win32_compat.c'),str(ROOT/'runtime/diagnostics/job_io_recorder.c'),'-pthread','-lm','-o',str(exe)]
    (out/'build-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
    b=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
    (out/'build.stdout').write_text(b.stdout);(out/'build.stderr').write_text(b.stderr)
    assert not b.returncode,b.stdout+b.stderr
    env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','PS3_TPROF','PS3_FTRACE'))}
    success=True
    for mode in ([a.case] if a.case else ('default','invalid','off','on','set-error','get-error','id-error','cap')):
        r=subprocess.run([str(exe),mode],capture_output=True,text=True,env=env,timeout=10)
        (out/(mode+'.stdout')).write_text(r.stdout);(out/(mode+'.stderr')).write_text(r.stderr)
        print(mode,r.returncode,r.stdout.strip(),flush=True)
        success=success and not r.returncode
        if mode in ('default','invalid'):assert '[spurs-pm-qos]' not in r.stderr
        elif not r.returncode:
            if mode=='cap':
                assert r.stderr.count('[spurs-pm-qos]')==129
                assert r.stderr.count('scope=pm-worker-start')==128
                assert r.stderr.count('telemetry_limit=128 further_startups_unreported=1')==1
                continue
            assert r.stderr.count('[spurs-pm-qos]')==1
            assert 'scope=pm-worker-start' in r.stderr and 'spu=3 ' in r.stderr
            if mode=='off':assert 'set_attempted=0 ' in r.stderr
            if mode=='set-error':assert 'set_rc=1 ' in r.stderr
            if mode=='get-error':assert 'before_rc=22 ' in r.stderr and 'after_rc=22 ' in r.stderr
            if mode=='id-error':assert 'host_id=0 id_rc=22 ' in r.stderr
    if not success:raise SystemExit(1)
if a.output_dir:
    a.output_dir.mkdir(parents=True,exist_ok=True);run(a.output_dir.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='spurs-pm-qos-') as d:run(pathlib.Path(d))
