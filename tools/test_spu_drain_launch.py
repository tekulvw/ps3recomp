#!/usr/bin/env python3
"""Bounded real-runtime launch preservation checks, with optional bug controls.

--mutants proves that skipped history, steps, reset, IRQ and PM-flow checks
are detected. All mutants live in temporary files; production files stay intact.
"""
import argparse
import os
import pathlib
import subprocess
import tempfile

ROOT=pathlib.Path(__file__).resolve().parents[1]
SPU=ROOT/'runtime/spu'
UNITS=['spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job',
       'spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
parser=argparse.ArgumentParser()
parser.add_argument('--mutants',action='store_true')
args=parser.parse_args()
env={k:v for k,v in os.environ.items()
     if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
source=(SPU/'spu_drain.c').read_text()
with tempfile.TemporaryDirectory(prefix='spu-drain-launch-') as td:
    tmp=pathlib.Path(td)
    def run(name,drain,sanitize=False,diagnostics=False):
        path=tmp/(name+'.c');path.write_text(drain)
        exe=tmp/name
        flags=['-std=gnu17','-O2','-UNDEBUG','-I'+str(ROOT/'include'),
               '-I'+str(SPU),'-I'+str(ROOT/'runtime/platform')]
        if sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        command=[os.environ.get('CC','cc'),*flags,
                 str(SPU/'tests/test_spu_drain_launch.c'),str(path),
                 *[str(SPU/(u+'.c')) for u in UNITS],
                 str(ROOT/'runtime/platform/win32_compat.c'),
                 str(ROOT/'runtime/diagnostics/job_io_recorder.c'),'-pthread','-lm','-o',str(exe)]
        built=subprocess.run(command,capture_output=True,text=True,timeout=60)
        assert built.returncode==0,built.stdout+built.stderr
        testenv=dict(env,SPU_REGION_FRAMED='1')
        if diagnostics:testenv.update(SPU_STEPTRACE='906',SPU_EXITTRACE='906')
        result=subprocess.run([str(exe)],env=testenv,capture_output=True,text=True,timeout=10)
        print(name,result.returncode,result.stdout.strip(),flush=True)
        if name=='o2':
            depth=subprocess.run([str(exe),'depth'],env=dict(testenv,SPU_HOST_DEPTH_MAX='8'),
                                 capture_output=True,text=True,timeout=10)
            assert depth.returncode==0,depth.stdout+depth.stderr
            assert '[spu-depth] recent drain sites (oldest first): 0A010 0A014 0A01C 0A020' in depth.stderr,depth.stderr
            print('PASS half-depth diagnostic ring',flush=True)
        return result
    for name,sanitize,diagnostics in [('o2',False,False),('ubsan',True,False),('diagnostics',False,True)]:
        result=run(name,source,sanitize,diagnostics)
        assert result.returncode==0,result.stdout+result.stderr
        if diagnostics:assert '[step]' in result.stderr and '[spu-exit]' in result.stderr,result.stderr
    if args.mutants:
        history_line=next(line for line in source.splitlines() if ('.pc[' in line or '->pc[' in line) and 'SPU_LS_MASK' in line and '++' in line)
        # The helper may name its stable history pointer differently; only the
        # write statement is omitted, leaving real lookup/dispatch unchanged.
        controls={
            'history':(history_line,''),
            'steps':('ctx->steps++;','/* bug: missing hot step increment */'),
            'reset':('spu_check_stack_reset(ctx, (void (*)(spu_context*))fn);','/* bug: skipped reset */ (void)fn;'),
            'irq':('if (ctx->int_enable && (ctx->event_status & ctx->event_mask)) {','if (0) {'),
            'flow':('g_pm_flow_buf[g_pm_flow_n++] = ctx->pc;','/* bug: missing PM-flow checkpoint */;'),
        }
        for name,(old,new) in controls.items():
            assert source.count(old)==1,(name,source.count(old))
            result=run('mutant-'+name,source.replace(old,new))
            assert result.returncode!=0 and 'FAIL:' in result.stdout,(name,result.stdout,result.stderr)
            print('PASS rejected bug control:',name,flush=True)
    print('PASS real drain launch preservation',flush=True)
