#!/usr/bin/env python3
"""Build and run the SPU interpreter attribution sink regression (off and on)."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: attribution sink requires POSIX threads')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-interp-attribution-') as temporary:
    work = Path(temporary)
    binary = work / 'test'
    args = [os.environ.get('CC', 'cc'), '-std=gnu17', '-O2', '-UNDEBUG', '-pthread',
            '-I' + str(root / 'include'), '-I' + str(spu), '-I' + str(root / 'runtime/platform'),
            str(spu / 'tests/test_spu_interp_attribution.c'), '-o', str(binary)]
    if os.environ.get('UBSAN') == '1':
        args[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    subprocess.run(args, check=True)
    report = work / 'report.tsv'
    for mode in ('off', 'on'):
        current = env.copy()
        if mode == 'on':
            current['SPU_INTERP_ATTRIBUTION'] = str(report)
        run = subprocess.run([str(binary), mode, str(report)], env=current, cwd=work,
                             capture_output=True, text=True, timeout=60)
        print(run.stdout.strip(), run.stderr.strip(), flush=True)
        if run.returncode:
            raise SystemExit(run.returncode)
