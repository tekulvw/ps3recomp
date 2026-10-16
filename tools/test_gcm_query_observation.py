#!/usr/bin/env python3
import os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='gcm-query-observe-') as tmp:
 out=Path(tmp)/'test'
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I'+str(root/'include'),str(root/'libs/video/tests/test_gcm_query_observation.c'),str(root/'libs/video/tests/gcm_deferred_fixture_stubs.c'),'-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections','-pthread','-lm','-o',str(out)],check=True,timeout=60)
 subprocess.run([str(out)],check=True,timeout=10)
