#!/usr/bin/env python3
"""Real host-only LOCAL/MAIN range translation; no guest syscall stub."""
import os
from pathlib import Path
import argparse
import shutil
import signal
import re
import hashlib
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compile-only',action='store_true',help='Cross-link only; no execution acceptance')
args=parser.parse_args()
def run(command,timeout):
    options={'creationflags':subprocess.CREATE_NEW_PROCESS_GROUP} if os.name=='nt' else {'start_new_session':True}
    child=subprocess.Popen([str(x) for x in command],**options)
    try:
        status=child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name=='nt':
            subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],check=False,timeout=15,
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        else:os.killpg(child.pid,signal.SIGKILL)
        child.wait(timeout=15)
        raise RuntimeError(f'Owned located-range fixture/compiler PID{child.pid} exceeded{timeout}s and was stopped')
    if status:raise subprocess.CalledProcessError(status,command)
with tempfile.TemporaryDirectory(prefix='gcm-native-located-') as tmp:
    tmp=Path(tmp);binary=tmp/('test.exe' if os.name=='nt' or args.compile_only else 'test')
    compiler=os.environ.get('CC','clang-cl' if os.name=='nt' else 'cc')
    sources=[root/'tests/test_gcm_native_located_range.c',root/'libs/video/tests/gcm_deferred_fixture_stubs.c','libs/video/rsx_fifo_busy.c']
    isolated=os.name=='nt' or args.compile_only
    if isolated:
        text=(root/'libs/video/cellGcmSys.c').read_text()
        start=text.index('s32 cellGcmNativeReadLocatedRange(')
        at=text.index('{',start);end=at+1;depth=1
        while depth:
            depth+={'{':1,'}':-1}.get(text[end],0);end+=1
        function=text[start:end]
        pages=re.search(r'^#define GCM_OFFSET_TABLE_PAGES\s+(\d+u?)',text,re.M).group(1)
        (tmp/'gcm_native_located_range_source.inc').write_text('#define GCM_OFFSET_TABLE_PAGES '+pages+'\n'+function+'\n')
        print('SOURCE BOUND: unchanged actual host resolver sha256='+hashlib.sha256(function.encode()).hexdigest(),flush=True)
        sources=sources[:1]
    if Path(compiler).stem.lower()=='clang-cl':
        linker=shutil.which('lld-link')
        if not linker:raise SystemExit('LLVM lld-link required for explicit bounded Windows link')
        objects=[]
        for source in sources:
            obj=tmp/(source.stem+'.obj');objects.append(obj)
            run([compiler,'/nologo','/std:c17','/O1','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                '/Gy','/Gw','/DMAG_GCM_NATIVE_RANGE_EXTRACTED=1','/I'+str(tmp),
                '/I'+str(root/'include'),'/c',source,'/Fo'+str(obj)],120)
        run([linker,'/nologo','/machine:x64','/subsystem:console','/incremental:no','/opt:ref',
            '/out:'+str(binary),*objects,'kernel32.lib'],120)
    else:
        linker=['-Wl,-dead_strip'] if sys.platform=='darwin' and not args.compile_only else ['-Wl,--gc-sections','-pthread','-lm']
        run([compiler,'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG','-ffunction-sections',
            '-fdata-sections','-I'+str(root/'include'),*(['-DMAG_GCM_NATIVE_RANGE_EXTRACTED=1','-I'+str(tmp)]if isolated else []),
            *sources,*linker,'-o',binary],60)
    if args.compile_only:print('BUILD ONLY: actual host-range fixture linked; execution pending')
    else:run([binary],10)
