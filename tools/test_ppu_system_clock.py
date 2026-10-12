#!/usr/bin/env python3
"""Exercise the actual guest time import and shared clock against monotonic bounds."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'runtime/ppu/ppu_sysprx.cpp').read_text()
start=source.index('extern "C" uint64_t ppu_system_time_us(') if 'extern "C" uint64_t ppu_system_time_us(' in source else source.index('static void sys_time_get_system_time(')
end=source.index('/* sys_process_is_stack',start)
body=source[start:end]
if 'extern "C" uint64_t ppu_system_time_us(' not in body:
    body+='\nextern "C" uint64_t ppu_system_time_us(void){return 0;}\n'
pre='''#include "ppu_context.h"
#include <time.h>
#include <stdint.h>
#include <cassert>
#include <cstdio>
static uint64_t monotonic_us(){timespec t{};assert(!clock_gettime(CLOCK_MONOTONIC,&t));return uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
'''
test='''int main(){ppu_context c{};uint64_t previous=0;
for(unsigned i=0;i<1000;++i){auto before=monotonic_us();auto a=ppu_system_time_us();sys_time_get_system_time(&c);auto b=ppu_system_time_us();auto after=monotonic_us();assert(before<=a && a<=c.gpr[3] && c.gpr[3]<=b && b<=after && a>=previous);previous=b;}
auto before=ppu_system_time_us();timespec delay{0,20000000};assert(!nanosleep(&delay,nullptr));auto elapsed=ppu_system_time_us()-before;assert(elapsed>=20000 && elapsed<2000000);puts("PASS1000 same-domain helper/import bounds, monotonic order and microsecond units");}
'''
def main():
    if os.name=='nt': print('SKIP POSIX clock fixture; Windows preserves existing QPC domain');return
    with tempfile.TemporaryDirectory(prefix='ppu-clock-') as tmp:
        p=Path(tmp);(p/'test.cpp').write_text(pre+body+test)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-UNDEBUG','-I'+str(root/'runtime/ppu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True,timeout=30)
        subprocess.run([str(p/'test')],check=True,timeout=5)
if __name__=='__main__': main()
