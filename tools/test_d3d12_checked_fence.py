#!/usr/bin/env python3
"""Deterministic Windows checked-fence contract; no graphics device required."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
if os.name != 'nt':
    print('SKIP: Windows D3D12 headers required')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
compiler=shutil.which(os.environ.get('CC','clang-cl'))
if not compiler: raise SystemExit('A Windows C compiler is required')
with tempfile.TemporaryDirectory(prefix='d3d-checked-') as temp:
    binary=Path(temp)/'test.exe'
    subprocess.run([compiler,'/nologo','/std:c17','/W4','/WX','/MT',
        str(root/'libs/video/tests/test_d3d12_checked_fence.c'),'/Fo'+temp+'\\','/Fe'+str(binary)],check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=10)
