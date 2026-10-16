#!/usr/bin/env python3
"""Exercise the production FIFO transfer path with synthetic guest resources."""
import os,subprocess,sys,tempfile
from pathlib import Path
if os.name=='nt':
 print('SKIP: FIFO fixture requires POSIX mmap/linker support');raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
import ppu_lifter
with tempfile.TemporaryDirectory(prefix='gcm-m2mf-') as tmp:
 p=Path(tmp);out=p/'test';l=ppu_lifter.PPULifter()
 (p/'ppu_recomp.h').write_text(l.emit_header());(p/'lifted.cpp').write_text(l.emit_source())
 common=['-O1','-ffunction-sections','-fdata-sections']
 for src,name in [(root/'libs/video/tests/gcm_deferred_fixture_stubs.c','seams'),(root/'libs/video/tests/test_gcm_memory_to_memory.c','fifo'),(root/'runtime/spu/spu_coherency.c','coh'),(root/'runtime/platform/win32_compat.c','compat')]:
  subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-UNDEBUG',*common,'-I',str(root/'include'),'-c',str(src),'-o',str(p/(name+'.o'))],check=True,timeout=60)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-UNDEBUG','-I'+str(root/'runtime/ppu'),*common,'-I'+str(p),'-I'+str(root/'include'),str(root/'runtime/ppu/ppu_loader.cpp'),str(p/'lifted.cpp'),str(p/'fifo.o'),str(p/'seams.o'),str(p/'coh.o'),str(p/'compat.o'),'-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections','-pthread','-lm','-o',str(out)],check=True,timeout=60)
 modes=sys.argv[1:] or ['retail','3d','rebind','rows','negative','overlap_down','overlap_up','unmapped','range','incomplete','format','notify','chunks','io_pages','reservation','bad_ea','main_end','page_zero','unknown_dma','budget','main_source','zero']
 for mode in modes:subprocess.run([str(out),mode],check=True,timeout=10)
