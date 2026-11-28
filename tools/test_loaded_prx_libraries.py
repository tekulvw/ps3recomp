#!/usr/bin/env python3
"""Synthetic typed guest library registration; no game files or GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    if os.name=='nt':
        print('SKIP: standalone fixture uses the POSIX compiler driver')
        return
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize',action='store_true')
    args=parser.parse_args()
    sanitizer=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
    cc=os.environ.get('CC','cc')
    with tempfile.TemporaryDirectory(prefix='ps3-loaded-prx-') as tmp:
        tmp=Path(tmp);objects=[]
        for i,source in enumerate(['runtime/prx/prx_loader.c','libs/system/sysPrxForUser.c','runtime/prx/tests/test_loaded_libraries.c']):
            obj=tmp/f'{i}.o';objects.append(str(obj))
            subprocess.run([cc,'-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-pthread',*sanitizer,
                '-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/prx'),'-I'+str(ROOT/'libs/system'),
                *(['-Dmalloc=fixture_malloc','-Drealloc=fixture_realloc'] if source=='runtime/prx/prx_loader.c' else []),
                '-c',str(ROOT/source),'-o',str(obj)],check=True,timeout=30)
        gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
        binary=tmp/'fixture'
        subprocess.run([cc,*objects,'-pthread',*sanitizer,gc,'-o',str(binary)],check=True,timeout=30)
        subprocess.run([str(binary)],check=True,timeout=15)
    sys.path.insert(0,str(ROOT/'tools'))
    import gen_hle_nids
    names=dict((name,nid) for nid,name in gen_hle_nids.scan(str(ROOT/'libs/system/sysPrxForUser.c')))
    assert names['sys_prx_register_library']==0x42b23552
    assert names['sys_prx_unregister_library']==0xd0ea47a7

if __name__=='__main__':main()
