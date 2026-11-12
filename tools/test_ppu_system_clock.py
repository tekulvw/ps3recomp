#!/usr/bin/env python3
"""Exercise the actual guest time import and the runtime's shared host clock against monotonic bounds."""
import os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'runtime/ppu/ppu_sysprx.cpp').read_text()
start=source.index('extern "C" uint64_t ppu_system_time_us(') if 'extern "C" uint64_t ppu_system_time_us(' in source else source.index('static void sys_time_get_system_time(')
end=source.index('/* sys_process_is_stack',start)
body=source[start:end]
if 'extern "C" uint64_t ppu_system_time_us(' not in body:
    body+='\nextern "C" uint64_t ppu_system_time_us(void){return 0;}\n'
# The guest clock is the runtime's one host clock (sys_timer.c host_monotonic_ns: mftb, system
# time and timers share it; mach_absolute_time on macOS, CLOCK_MONOTONIC elsewhere). Build the
# real one into the fixture and bound the import by it.
timer=(root/'runtime/syscalls/sys_timer.c').read_text()
hstart=timer.index('uint64_t host_monotonic_ns(void)\n{')
hend=timer.index('\n}\n',hstart)+3
pre='''#include "ppu_context.h"
#include <time.h>
#include <stdint.h>
#include <cassert>
#include <cstdio>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
extern "C" {
'''+timer[hstart:hend]+'''}
static uint64_t monotonic_us(){return host_monotonic_ns()/1000;}
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
