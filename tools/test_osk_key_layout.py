#!/usr/bin/env python3
"""Verify OSK key-layout configuration through generated registration and real PPU HLE."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]

def main():
    if os.name=='nt':
        print('SKIP: standalone HLE bridge regression needs a POSIX toolchain')
        return 0

    with tempfile.TemporaryDirectory(prefix='ps3-osk-layout-') as tmp:
        tmp=Path(tmp)
        (tmp/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellOskDialog','--out',str(tmp/'nids.cpp')],check=True,timeout=15)
        includes=['-I'+str(p) for p in (root/'include',root/'runtime/ppu',tmp)]
        (tmp/'osk_state.c').write_text('#include "'+str(root/'libs/misc/cellOskDialog.c')+'"\nunsigned fixture_osk_layout(void){return atomic_load(&s_osk_key_layout_option); }\n')
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-DPS3RECOMP_DISABLE_NATIVE_OSK',*includes,'-c',str(tmp/'osk_state.c'),'-o',str(tmp/'osk.o')],check=True,timeout=30)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG',*includes,str(root/'tests/test_osk_key_layout.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'osk.o'),'-o',str(tmp/'fixture')],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        subprocess.run([str(tmp/'fixture')],env=env,check=True,timeout=10)
    return 0
if __name__=='__main__':
    raise SystemExit(main())
