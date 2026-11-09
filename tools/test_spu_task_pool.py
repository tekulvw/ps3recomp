#!/usr/bin/env python3
"""Async SPU task host threads, pool off and on, on Windows and POSIX.

Builds runtime/spu/tests/test_spu_task_pool.c (which compiles in
runtime/spu/spu_task_pool.inc), runs it with PS3RECOMP_SPU_TASK_POOL unset and
=1, and requires the same task results from both.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/spu/tests/test_spu_task_pool.c'
windows = os.name == 'nt'
compiler = os.environ.get('CC', 'clang-cl' if windows else 'cc')
msvc_style = Path(compiler).stem.lower() in ('clang-cl', 'cl')
if not shutil.which(compiler):
    raise SystemExit(f'compiler {compiler!r} not found')

with tempfile.TemporaryDirectory(prefix='spu-task-pool-') as tmp:
    binary = Path(tmp) / ('test.exe' if windows else 'test')
    if msvc_style:
        command = [compiler, '/nologo', '/std:c17', '/O2', '/W3', '/D_CRT_SECURE_NO_WARNINGS',
                   '/I' + str(ROOT / 'include'), str(SOURCE), '/Fe' + str(binary)]
    else:
        command = [compiler, '-std=gnu17', '-O2', '-Wall', '-Wextra', '-UNDEBUG',
                   '-I' + str(ROOT / 'include'), str(SOURCE),
                   *([] if windows else ['-pthread']), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=120, cwd=tmp)
    results = {}
    for mode in (None, '1'):
        env = {k: v for k, v in os.environ.items() if k != 'PS3RECOMP_SPU_TASK_POOL'}
        if mode is not None:
            env['PS3RECOMP_SPU_TASK_POOL'] = mode
        run = subprocess.run([str(binary)], env=env, cwd=tmp, timeout=120,
                             capture_output=True, text=True)
        label = 'pool on' if mode else 'pool off'
        print(f'--- {label}', flush=True)
        print(run.stdout.strip(), flush=True)
        if run.returncode:
            print(run.stderr[-4000:], flush=True)
            raise SystemExit(f'{label}: exit {run.returncode}')
        results[label] = next(line for line in run.stdout.splitlines() if line.startswith('RESULT '))
    assert results['pool off'] == results['pool on'], results
    print('PASS async SPU task threads: pool off and on agree', flush=True)
