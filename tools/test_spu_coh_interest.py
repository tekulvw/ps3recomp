#!/usr/bin/env python3
"""Both coherence schemes publish the bitmap used by inline PPU stores."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name=='nt':
    print('SKIP: standalone coherence fixture uses the POSIX compiler')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='spu-coh-interest-') as tmp:
    binary=Path(tmp)/'fixture'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG',
                    '-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),
                    str(root/'runtime/spu/spu_coherency.c'),
                    str(root/'runtime/spu/tests/test_spu_coh_interest.c'),
                    '-pthread','-o',str(binary)],check=True,timeout=60)
    for scheme in ['0','1']:
        subprocess.run([str(binary),scheme],env=dict(os.environ,PS3RECOMP_COH_VERSIONS=scheme),
                       check=True,timeout=10)
