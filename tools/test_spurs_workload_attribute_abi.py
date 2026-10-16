#!/usr/bin/env python3
"""Exercise the real PPU dispatcher and SPURS initializer with a ninth ABI argument."""
import os,platform,subprocess,tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spurs-workload-abi-') as tmp:
 t=Path(tmp);(t/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
 inc=['-I'+str(p) for p in (r,r/'include',r/'runtime/ppu',r/'runtime/spu',t)]
 common=['-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',*inc]
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu17',*common,'-c',str(r/'libs/spurs/cellSpurs.c'),'-o',str(t/'spurs.o')],check=True)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17',*common,str(r/'tests/test_spurs_workload_attribute_abi.cpp'),str(r/'runtime/ppu/ppu_hle.cpp'),str(t/'spurs.o'),'-Wl,-dead_strip' if platform.system()=='Darwin' else '-Wl,--gc-sections','-pthread','-o',str(t/'fixture')],check=True)
 env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
 for mode in ('off','on'):subprocess.run([str(t/'fixture'),mode],env=env,check=True,timeout=10)
