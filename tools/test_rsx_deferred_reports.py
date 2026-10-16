#!/usr/bin/env python3
"""Bounded report ordering and actual VM mutation rejection fixtures."""
import os, subprocess, sys, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='rsx-reports-') as tmp:
 for name in ('test_rsx_deferred_reports','test_vm_release_guard'):
  out=Path(tmp)/name
  cmd=[os.environ.get('CC','cc'),'-std=gnu17','-O2','-UNDEBUG','-fsanitize=undefined','-ffunction-sections','-fdata-sections','-I'+str(root/'include'),str(root/'tests'/f'{name}.c'),'-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections','-pthread','-o',str(out)]
  subprocess.run(cmd,check=True,timeout=60);subprocess.run([str(out)],check=True,timeout=10)
