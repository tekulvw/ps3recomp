#!/usr/bin/env python3
"""Real ELF loading and guest main-thread priority query; no game/GPU required."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from check_ppu_scaffold import stub_header

ROOT = Path(__file__).resolve().parents[1]
# Exact 32-byte process parameter header from retail MAG v1.0, ELF PT60000001.
MAG_PARAM = bytes.fromhex('0000002013bcc5f60000900000300001000003e90005a0000010000000000000')

def image(param=None, advertised=None, offset_override=None):
    count = 1 + (param is not None)
    out = bytearray(0x1000)
    out[:16] = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>HHIQQQIHHHHHH', out, 16, 2, 21, 1, 0x10000, 64, 0, 0, 64, 56, count, 0, 0, 0)
    struct.pack_into('>IIQQQQQQ', out, 64, 1, 6, len(out), 0x10000, 0x10000, 8, 0x1000, 0x1000)
    out.extend(struct.pack('>II', 0x10000, 0x10000))
    if param is not None:
        size = len(param) if advertised is None else advertised
        offset = len(out) if offset_override is None else offset_override
        struct.pack_into('>IIQQQQQQ', out, 120, 0x60000001, 0, offset, 0x11000, 0x11000, size, size, 8)
        out.extend(param)
    return out

def header(priority):
    data=bytearray(MAG_PARAM); struct.pack_into('>i',data,16,priority); return data

def main():
    if os.name == 'nt':
        print('SKIP: standalone fixture uses the POSIX mmap/toolchain'); return
    with tempfile.TemporaryDirectory(prefix='ppu-primary-priority-') as tmp:
        tmp=Path(tmp); (tmp/'ppu_recomp.h').write_text(stub_header())
        flags=['-O1','-ffunction-sections','-fdata-sections','-pthread']
        subprocess.run([os.environ.get('CC','cc'),*flags,'-c',str(ROOT/'runtime/syscalls/sys_ppu_thread.c'),'-o',str(tmp/'threads.o')],check=True)
        linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-ldl']
        subprocess.run([os.environ.get('CXX','c++'),*flags,'-std=c++17','-I'+str(tmp),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/platform'),str(ROOT/'runtime/ppu/ppu_loader.cpp'),str(ROOT/'runtime/ppu/tests/test_ppu_primary_priority.cpp'),str(tmp/'threads.o'),*linker,'-o',str(tmp/'test')],check=True)
        bad_magic=bytearray(MAG_PARAM); bad_magic[4:8]=bytes(4)
        short_declared=bytearray(MAG_PARAM); short_declared[:4]=struct.pack('>I',16)
        cases=[('retail',image(MAG_PARAM),1001),('different',image(header(700)),700),
               ('extended',image(struct.pack('>I',36)+MAG_PARAM[4:]+bytes(4)),1001),
               ('highest',image(header(0)),0),('lowest',image(header(3071)),3071),
               ('absent',image(),1001),('bad-magic',image(bad_magic),1001),
               ('invalid-negative',image(header(-32768)),1001),('invalid-high',image(header(3072)),1001),
               ('short-declared',image(short_declared),1001),('truncated',image(MAG_PARAM[:16]),1001),
               ('past-eof',image(MAG_PARAM,advertised=0x1000),1001),
               ('overflow-offset',image(MAG_PARAM,offset_override=0xfffffffffffffff0),1001)]
        args=[]
        for label,data,expected in cases:
            path=tmp/(label+'.elf'); path.write_bytes(data); args += [str(path),str(expected)]
        env={k:v for k,v in os.environ.items() if not k.startswith(('PPU_','PS3_','PS3RECOMP_','SPU_'))}
        subprocess.run([str(tmp/'test'),*args],env=env,check=True,timeout=15)
        print('PASS: retail/custom primary priorities, missing/invalid parameters and repeated-load reset')
if __name__=='__main__': main()
