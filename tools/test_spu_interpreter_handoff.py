#!/usr/bin/env python3
"""Exercise real SPU interpretation/drain boundaries without game binaries."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
# Both decode-cache storages: static TLS (default) and the per-thread heap table
# of PS3RECOMP_SPU_DECODE_CACHE_HEAP, which must interpret identically.
for variant, defines in (('static-tls', []), ('heap', ['-DPS3RECOMP_SPU_DECODE_CACHE_HEAP=1'])):
  with tempfile.TemporaryDirectory(prefix='spu-interp-handoff-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', *defines,
                    '-I'+str(root/'include'), '-I'+str(spu),
                    '-I'+str(root/'runtime/platform'),
                    str(spu/'tests/test_spu_interpreter_handoff.c'),
                    *[str(spu / name) for name in sources],
                    str(root/'runtime/platform/win32_compat.c'), str(root/'runtime/diagnostics/job_io_recorder.c'), '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    if defines and sys.platform.startswith('linux'):
        # The table must leave static TLS (every host thread clears it at creation).
        obj = Path(tmp) / 'interp.o'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', *defines, '-I'+str(root/'include'),
                        '-I'+str(spu), '-I'+str(root/'runtime/platform'), '-c', str(spu/'spu_interp.c'),
                        '-o', str(obj)], check=True)
        sections = subprocess.run(['objdump', '-h', str(obj)], capture_output=True, text=True, check=True).stdout
        tls = sum(int(line.split()[2], 16) for line in sections.splitlines()
                  if len(line.split()) > 2 and line.split()[1] in ('.tbss', '.tdata'))
        assert tls < 1 << 20, f'heap decode cache still holds {tls} bytes of static TLS'
        print(f'{variant}: spu_interp.o static TLS {tls} bytes')
    failed = []
    for mode in ['top', 'top-zero', 'nested-return', 'nested-return-unlifted', 'nested-calls', 'stop-zero',
                 'nested-stop', 'stop-code', 'halt', 'smc', 'pure-interpreter', 'stepcap', 'negative-stepcap']:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
        if mode in ('stepcap', 'negative-stepcap'):
            env.update(YDKJ_SPU_TRACE='1', SPU_STEPCAP='2')
        result = subprocess.run([str(binary), mode], capture_output=True,
                                text=True, timeout=5, cwd=tmp, env=env)
        print(variant, result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2500:])
            failed.append(variant + ':' + mode)
    if failed:
        raise SystemExit('FAILED: ' + ', '.join(failed))
