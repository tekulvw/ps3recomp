#!/usr/bin/env python3
"""Run real network-dialog HLE with isolated offline/online process settings."""
import os
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ps3-netctl-') as tmp:
    binary=Path(tmp)/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-UNDEBUG',
                    '-I'+str(root/'include'),'-I'+str(root/'libs/system'),
                    str(root/'tests/test_netctl_dialog.c'),
                    str(root/'libs/network/cellNetCtl.c'),'-o',str(binary)],check=True)
    env=dict(os.environ);env.pop('PS3_NET_ONLINE',None)
    subprocess.run([str(binary)],env=env,check=True,timeout=10)
    env['PS3_NET_ONLINE']='1'
    subprocess.run([str(binary),'online'],env=env,check=True,timeout=10)
