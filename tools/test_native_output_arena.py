#!/usr/bin/env python3
"""Synthetic native output arena through the actual VM and GCM registries."""
import os,subprocess,sys,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='native-output-arena-') as tmp:
    binary=Path(tmp)/'test'
    linker=['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections','-lm']
    cmd=[os.environ.get('CC','cc'),'-std=gnu17','-D_GNU_SOURCE','-O1','-UNDEBUG','-DCELL_GCM_MAX_IO_MAPPINGS=8','-ffunction-sections','-fdata-sections','-I'+str(root/'include'),str(root/'tests/test_native_output_arena.c'),str(root/'runtime/syscalls/sys_memory.c'),str(root/'libs/video/tests/gcm_deferred_fixture_stubs.c'),str(root/'runtime/platform/win32_compat.c'),'-pthread',*linker,'-o',str(binary)]
    subprocess.run(cmd,check=True,timeout=30)
    env={k:v for k,v in os.environ.items() if not k.startswith('A2O_')}
    env['PS3RECOMP_RSX_DEFERRED_REPORTS']='1'
    subprocess.run([str(binary)],check=True,timeout=15,env=env)
