#!/usr/bin/env python3
"""Bounded source-qualified observer control/worker fixture; no game execution."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]

def run(command,timeout,stage):
    print(f'OBSERVER_STAGE {stage}: {command[0]}',flush=True)
    options={'creationflags':subprocess.CREATE_NEW_PROCESS_GROUP} if os.name=='nt' else {'start_new_session':True}
    started=time.monotonic()
    process=subprocess.Popen([str(arg)for arg in command],**options)
    try:
        result=process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        # This exact Popen PID owns only our compiler/linker/fixture tree.
        # Kill descendants before deleting their owned output directory.
        if os.name=='nt':
            subprocess.run(['taskkill','/PID',str(process.pid),'/T','/F'],check=False,
                           stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
        else:
            os.killpg(process.pid,signal.SIGKILL)
        process.wait(timeout=15)
        raise RuntimeError(f'{stage} timed out after {timeout}s; owned PID {process.pid} stopped')
    if result:raise subprocess.CalledProcessError(result,command)
    print(f'OBSERVER_STAGE {stage} passed in {time.monotonic()-started:.3f}s',flush=True)

tmp=Path(tempfile.mkdtemp(prefix='original-work-observer-'))
try:
    compiler=shutil.which(os.environ.get('CC','clang-cl' if os.name=='nt' else 'cc'))
    if not compiler:raise SystemExit('C compiler required')
    binary=tmp/('observer.exe' if os.name=='nt' else 'observer')
    sources=[ROOT/'tests/test_original_work_observer.c',ROOT/'runtime/diagnostics/original_work_observer.c']
    if os.name=='nt':
        linker=shutil.which('lld-link')
        if not linker:raise SystemExit('LLVM lld-link required for explicit bounded Windows link')
        objects=[]
        for source in sources:
            obj=tmp/(source.stem+'.obj');objects.append(obj)
            run([compiler,'/nologo','/std:c17','/W4','/WX','/DPS3RECOMP_ORIGINAL_OBSERVER=1',
                 '/I'+str(ROOT/'runtime/diagnostics'),'/c',source,'/Fo'+str(obj)],120,'compile '+source.name)
        # Direct LLD invocation isolates link diagnostics from driver children
        # and avoids leaving a separately owned MSVC linker holding observer.exe.
        run([linker,'/nologo','/machine:x64','/subsystem:console','/incremental:no',
             '/out:'+str(binary),*objects,'kernel32.lib'],120,'link')
    else:
        run([compiler,'-std=c17','-Wall','-Wextra','-Werror','-DPS3RECOMP_ORIGINAL_OBSERVER=1',
             '-I'+str(ROOT/'runtime/diagnostics'),*sources,'-pthread','-o',binary],60,'compile/link')
    for mode in ['good','wrong_source','missing_worker','overflow','loss','route','unregistered','after_retire','late_arm']+['arm_race']*8:
        run([binary,mode],10,'execute '+mode)
finally:
    try:shutil.rmtree(tmp)
    except OSError as error:
        # Preserve the original failed stage instead of masking it with cleanup.
        print(f'Owned observer artifacts retained at {tmp}: {error}',file=sys.stderr)
