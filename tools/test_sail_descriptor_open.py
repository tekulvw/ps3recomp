#!/usr/bin/env python3
"""Compile/run real Sail and generated NID registration through the PPU HLE bridge."""
import os
import shutil
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
from gen_hle_nids import scan

root=Path(__file__).resolve().parents[1]

def main():
    if os.name=='nt':
        print('SKIP: standalone HLE bridge regression needs a POSIX toolchain')
        return 0
    handlers=dict(scan(root/'libs/codec/cellSail.c'))
    assert handlers.get(0xC044FAB1)=='cellSailDescriptorOpen','DescriptorOpen is not registered'
    with tempfile.TemporaryDirectory(prefix='ps3-sail-descriptor-') as tmp:
        tmp=Path(tmp)
        (tmp/'ppu_recomp.h').write_text('#include "ppu_context.h"\n')
        subprocess.run([sys.executable,str(root/'tools/gen_hle_nids.py'),'cellSail','cellPamf','--out',str(tmp/'nids.cpp')],check=True,timeout=15)
        includes=['-I'+str(p) for p in (root/'include',root/'runtime/ppu',tmp)]
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',*includes,'-c',str(root/'libs/codec/cellSail.c'),'-o',str(tmp/'sail.o')],check=True,timeout=30)
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',*includes,'-c',str(root/'libs/codec/cellPamf.c'),'-o',str(tmp/'pamf.o')],check=True,timeout=30)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O1','-UNDEBUG',*includes,str(root/'tests/test_sail_descriptor_open.cpp'),str(root/'runtime/ppu/ppu_hle.cpp'),str(tmp/'nids.cpp'),str(tmp/'sail.o'),str(tmp/'pamf.o'),'-o',str(tmp/'fixture')],check=True,timeout=45)
        env={k:v for k,v in os.environ.items() if k in ('PATH','HOME','TMPDIR','SYSTEMROOT')}
        media=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else None
        if media is not None: shutil.copyfile(media,tmp/'fixture.pam')
        else:
            b=bytearray(4096);b[:8]=b'PAMF0041'
            for off,v in ((8,1),(12,1),(0x50,0x94),(0x6a,2),(0x70,0x74),(0x82,0x62)):
                struct.pack_into('>I',b,off,v)
            for off,v in ((0x6f,1),(0x81,1),(0x87,2),(0x88,2),(0x8c,0xe0),(0xb8,0xdc),(0xbc,0xbd)):
                b[off]=v
            (tmp/'fixture.pam').write_bytes(b)
        b=bytearray((tmp/'fixture.pam').read_bytes());b[0]=0;(tmp/'bad-magic.pam').write_bytes(b)
        b=bytearray((tmp/'fixture.pam').read_bytes());b[0x87]=255;(tmp/'bad-inner.pam').write_bytes(b)
        (tmp/'truncated.pam').write_bytes(b[:100])
        subprocess.run([str(tmp/'fixture'),'fixture.pam'],cwd=tmp,env=env,check=True,timeout=10)
    return 0
if __name__=='__main__':
    raise SystemExit(main())
