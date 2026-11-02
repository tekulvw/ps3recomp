#!/usr/bin/env python3
"""Real native factory/cleanup and diagnostic-vs-final drain behavior."""
from pathlib import Path
import argparse
import os
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--windows', action='store_true', help='Select actual Windows factory fixture for a cross-build')
parser.add_argument('--compile-only', action='store_true', help='Link only; no execution acceptance')
args = parser.parse_args()
windows = os.name == 'nt' or args.windows
if args.windows and os.name != 'nt' and not args.compile_only:
    parser.error('Cross-built Windows fixtures require --compile-only')

def run(command, timeout):
    options = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
    child = subprocess.Popen([str(arg) for arg in command], **options)
    try:
        result = child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], check=False,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        else:
            os.killpg(child.pid, signal.SIGKILL)
        child.wait(timeout=15)
        raise RuntimeError(f'Owned worker fixture/compiler PID {child.pid} exceeded {timeout}s and was stopped')
    if result:
        raise subprocess.CalledProcessError(result, command)

tmp = Path(tempfile.mkdtemp(prefix='original-worker-lifecycle-'))
try:
    binary = tmp / ('fixture.exe' if windows else 'fixture')
    suffix = '_windows' if windows else ''
    sources = [ROOT / f'tests/test_original_work_threads{suffix}.c',
               ROOT / f'runtime/diagnostics/original_work_threads{suffix}.c',
               ROOT / 'runtime/diagnostics/original_work_observer.c']
    compiler = os.environ.get('CC', 'clang-cl' if os.name == 'nt' else 'cc')
    if Path(compiler).stem.lower() in ('clang-cl', 'cl'):
        linker = shutil.which('lld-link')
        if not linker:
            raise SystemExit('LLVM lld-link required for explicit bounded Windows link')
        objects = []
        for source in sources:
            obj = tmp / (source.stem + '.obj')
            objects.append(obj)
            run([compiler, '/nologo', '/std:c17', '/W4', '/WX', '/DPS3RECOMP_ORIGINAL_OBSERVER=1',
                 '/I' + str(ROOT / 'runtime/diagnostics'), '/c', source, '/Fo' + str(obj)], 120)
        run([linker, '/nologo', '/machine:x64', '/subsystem:console', '/incremental:no',
             '/out:' + str(binary), *objects, 'kernel32.lib'], 120)
    else:
        run([compiler, '-std=c17', '-Wall', '-Wextra', '-Werror',
             '-DPS3RECOMP_ORIGINAL_OBSERVER=1', '-I' + str(ROOT / 'runtime/diagnostics'),
             *sources, *([] if windows else ['-pthread']), '-o', binary], 60)
    if args.compile_only:
        print('BUILD ONLY: native worker fixture linked; execution remains pending', flush=True)
    else:
        modes = ['windows_return', 'windows_exit', 'crt_return', 'crt_exit'] if windows else ['return', 'exit']
        for mode in modes:
            run([binary, mode], 10)
finally:
    try:
        shutil.rmtree(tmp)
    except OSError as error:
        print(f'Owned worker fixture artifacts retained at {tmp}: {error}')
