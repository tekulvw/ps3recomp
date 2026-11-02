#!/usr/bin/env python3
"""Execute actual authored SPU bodies on all original execution routes."""
import argparse,hashlib,json,os
from pathlib import Path
import struct,subprocess,sys,tempfile
from spu_leaf_blocks import emit
ROOT=Path(__file__).resolve().parents[1];SPU=ROOT/'runtime/spu'
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compile-only',action='store_true',help='Explicit cross-build check; never claims execution')
negative=parser.add_mutually_exclusive_group()
negative.add_argument('--negative-interpreter-control',action='store_true',help='Omit actual interpreter hook in isolated source copy; require witness failure')
negative.add_argument('--negative-halt-control',action='store_true',help='Invent skipped halt-PC witness in isolated generated source; require rejection')
args=parser.parse_args()
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job',
       'spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
environment={k:v for k,v in os.environ.items()if not k.startswith(('SPU_','SPURS_','PS3_SPU_','PS3_TPROF','YDKJ_','LBP_','GH3_'))}
program=struct.pack('>6I',0x40800087,0x33000180,0x1c004387,0x35000300,0x1c008387,0x35000000)
halt_program=struct.pack('>3I',0x32000100,0x40200000,0x327fff00)
bounds=[(0x10000,0x10010),(0x10010,0x10018)]
with tempfile.TemporaryDirectory(prefix='original-routes-') as temporary:
    tmp=Path(temporary);(tmp/'program.bin').write_bytes(program)
    (tmp/'bounds.json').write_text(json.dumps([{'start':hex(a),'end':hex(b)}for a,b in bounds]))
    (tmp/'observer_program.h').write_text('static const uint8_t observer_program[]={'+','.join(map(str,program))+'};\nstatic const uint8_t observer_program_sha256[]={'+','.join(map(str,hashlib.sha256(program).digest()))+'};\n')
    with (tmp/'observer_program.h').open('a')as header:
        header.write('static const uint8_t observer_halt_program[]={'+','.join(map(str,halt_program))+'};\nstatic const uint8_t observer_halt_sha256[]={'+','.join(map(str,hashlib.sha256(halt_program).digest()))+'};\n')
    (tmp/'halt.bin').write_bytes(halt_program)
    (tmp/'halt_bounds.json').write_text('[{"start":"0x10000","end":"0x1000c"}]')
    subprocess.run([sys.executable,str(ROOT/'tools/spu_lifter.py'),str(tmp/'halt.bin'),'--base','0x10000','--functions',str(tmp/'halt_bounds.json'),'--symbol-prefix','halt_observer_','--observe-original-work','--output',str(tmp/'halt')],check=True,capture_output=True,text=True,timeout=30)
    for file in ['spu_recomp.h','spu_recomp.c']:
        p=tmp/'halt'/file;p.write_text(p.read_text().replace('../../runtime/spu/',''))
    if args.negative_halt_control:
        file=tmp/'halt/spu_recomp.c';source=file.read_text();old='spu_halt(ctx); return;'
        assert source.count(old)==1
        file.write_text(source.replace(old,'PS3_ORIGINAL_STEP(ctx,0x10004u,PS3_ORIGINAL_LIFTED); '+old))
    for name,prefix,extra in [('legacy','observer_',[]),('region','region_observer_',['--connected-regions'])]:
        command=[sys.executable,ROOT/'tools/spu_lifter.py',tmp/'program.bin','--base','0x10000',
            '--functions',tmp/'bounds.json','--symbol-prefix',prefix,'--verified-code-image','701',
            '--verified-code-spans','--observe-original-work','--output',tmp/name,*extra]
        subprocess.run(list(map(str,command)),check=True,capture_output=True,text=True,timeout=30)
        for file in ['spu_recomp.h','spu_recomp.c']:
            p=tmp/name/file;p.write_text(p.read_text().replace('../../runtime/spu/',''))
    header,source,_=emit(program,0x10000,bounds,701,'leaf_observer_',observe_original_work=True)
    (tmp/'leaf').mkdir();(tmp/'leaf/spu_recomp.h').write_text(header);(tmp/'leaf/spu_recomp.c').write_text(source)
    binary=tmp/'routes'
    interp=SPU/'spu_interp.c'
    if args.negative_interpreter_control:
        interp=tmp/'spu_interp_without_witness.c';source=(SPU/'spu_interp.c').read_text()
        old='PS3_ORIGINAL_STEP(ctx,ctx->pc,PS3_ORIGINAL_INTERPRETER);';assert source.count(old)==1
        interp.write_text(source.replace(old,'((void)0);'))
    command=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-DPS3RECOMP_ORIGINAL_OBSERVER=1',
        *['-I'+str(path)for path in [ROOT/'include',SPU,ROOT/'runtime/platform',ROOT/'runtime/diagnostics',tmp]],
        str(SPU/'tests/test_original_work_routes.c'),*[str(interp if unit=='spu_interp' else SPU/(unit+'.c'))for unit in units],
        str(ROOT/'runtime/platform/win32_compat.c'),str(ROOT/'runtime/diagnostics/original_work_observer.c'),
        str(ROOT/'runtime/diagnostics/job_io_recorder.c'),str(ROOT/'runtime/diagnostics/critical_trace.c'),
        *[str(tmp/name/'spu_recomp.c')for name in ['legacy','region','leaf','halt']],'-pthread','-lm','-o',str(binary)]
    compiled=subprocess.run(command,capture_output=True,text=True,timeout=90)
    if compiled.returncode:raise RuntimeError(compiled.stdout+compiled.stderr)
    if args.compile_only:
        print('BUILD ONLY: original runtime/generated fixture linked; execution pending',flush=True)
        sys.exit(0)
    # The new hook must disappear completely from a normal interpreter build.
    default_object=tmp/'default-interpreter.o'
    default_command=[os.environ.get('CC','cc'),'-std=gnu17','-O2',
        *['-I'+str(path)for path in [ROOT/'include',SPU,ROOT/'runtime/platform']],
        '-c',str(SPU/'spu_interp.c'),'-o',str(default_object)]
    checked=subprocess.run(default_command,capture_output=True,text=True,timeout=60)
    if checked.returncode:raise RuntimeError(checked.stdout+checked.stderr)
    symbols=subprocess.run([os.environ.get('NM','nm'),'-u',str(default_object)],capture_output=True,text=True,timeout=10)
    if symbols.returncode:raise RuntimeError(symbols.stdout+symbols.stderr)
    assert 'ps3_original_step' not in symbols.stdout
    routes=['interp']if args.negative_interpreter_control else ['halt']if args.negative_halt_control else ['direct','interp','micro','region','leaf','halt']
    for route in routes:
        run=subprocess.run([str(binary),route],env=dict(environment,SPU_REGION_FRAMED='1',SPU_VERIFIED_LEAF_BLOCKS='1'),capture_output=True,text=True,timeout=15)
        print(run.stdout+run.stderr,flush=True)
        if args.negative_interpreter_control or args.negative_halt_control:
            assert run.returncode==42 and 'ORIGINAL_WITNESS_MISMATCH' in run.stderr
            print('NEGATIVE CONTROL rejected missing/fabricated witness after exact guest result',flush=True)
            continue
        if run.returncode:raise SystemExit(run.returncode)
