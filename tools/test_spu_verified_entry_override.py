#!/usr/bin/env python3
"""Production resolver regression for exact native entries with nonexclusive LS spans."""
import os
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
spu=root/'runtime/spu'
units=['spu_drain','spu_lockstep','spu_coherency','spu_workload','spurs_policy','spurs_job','spu_tsp_weak','spu_vm_pagemap','spurs_policy_blob_weak','spu_interp']
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-entry-override-') as td:
    for sanitize in (False,True):
        binary=Path(td)/('ubsan' if sanitize else 'o2')
        command=['cc','-std=gnu17','-O2','-UNDEBUG','-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform'),str(spu/'tests/test_spu_verified_entry_override.c'),*[str(spu/(unit+'.c')) for unit in units],str(root/'runtime/platform/win32_compat.c'),'-pthread','-lm','-o',str(binary)]
        if sanitize: command[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
        if os.environ.get('RED')=='1':command[1:1]=['-DSPU_OVERRIDE_RED']
        result=subprocess.run(command,capture_output=True,text=True,timeout=45)
        assert result.returncode==0,result.stderr
        for prefilter in ('0','1'):
            result=subprocess.run([str(binary)],env=dict(env,SPU_VERIFIED_ENTRY_PREFILTER=prefilter),capture_output=True,text=True,timeout=10)
            print(('UBSan' if sanitize else 'O2'),prefilter,result.stdout,flush=True)
            assert result.returncode==0,result.stdout+result.stderr
