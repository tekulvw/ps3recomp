#!/usr/bin/env python3
"""Actual generated PPU mid-function original witness and default equivalence."""
from pathlib import Path
import argparse,os,struct,subprocess,tempfile
from ppu_disasm import disassemble_bytes
from ppu_lifter import PPULifter
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compile-only',action='store_true',help='Explicit cross-build check; execution remains pending')
args=parser.parse_args()
program=struct.pack('>6I',0x38600001,0x38630002,0x2c030003,0x40820008,0x38630004,0x4e800020)
with tempfile.TemporaryDirectory(prefix='ppu-original-observer-')as temporary:
    tmp=Path(temporary);results=[]
    for observed in [False,True]:
        lifter=PPULifter(prefix='ppu_observer_',observe_original_work=observed)
        lifter.lift_function(disassemble_bytes(program,0x1000,True),0x1000,0x1018)
        directory=tmp/str(int(observed));directory.mkdir()
        (directory/'ppu_recomp.h').write_text(lifter.emit_header())
        source=lifter.emit_source();(directory/'ppu_recomp.c').write_text(source)
        assert ('PS3_ORIGINAL_STEP' in source)==observed
        binary=directory/'fixture'
        flags=['-O2','-UNDEBUG','-I'+str(directory),'-I'+str(ROOT/'runtime/diagnostics'),'-I'+str(ROOT/'runtime/ppu')]
        if observed:flags.append('-DPS3RECOMP_ORIGINAL_OBSERVER=1')
        objects=[]
        for name,input_source in [('fixture',ROOT/'tests/test_ppu_original_work.c'),('observer',ROOT/'runtime/diagnostics/original_work_observer.c')]:
            obj=directory/(name+'.o');objects.append(str(obj))
            compiler=[os.environ.get('CXX','c++'),'-std=gnu++20','-x','c++'] if name=='fixture' else [os.environ.get('CC','cc'),'-std=gnu17']
            built=subprocess.run([*compiler,*flags,'-c',str(input_source),'-o',str(obj)],capture_output=True,text=True,timeout=60)
            if built.returncode:raise RuntimeError(built.stdout+built.stderr)
        command=[os.environ.get('CXX','c++'),'-std=gnu++20',*flags,'-x','c++',str(directory/'ppu_recomp.c'),'-x','none',*objects,'-lm','-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True,timeout=60)
        if built.returncode:raise RuntimeError(built.stdout+built.stderr)
        if args.compile_only:
            print(f'PPU observed={observed} BUILD ONLY; execution pending',flush=True)
            continue
        result=subprocess.run([str(binary),str(int(observed))],capture_output=True,text=True,timeout=10)
        if result.returncode:raise RuntimeError(result.stdout+result.stderr)
        results.append(result.stdout);print(result.stdout.strip(),flush=True)
    if not args.compile_only:assert results[0]==results[1]
