#!/usr/bin/env python3
"""Actual common CPU ingress ownership test; no GPU or game execution."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sources=['libs/video/tests/test_original_owner_ingress.c']+[
    'libs/video/'+name+'.c' for name in ('rsx_draw_engine','rsx_fifo_busy','rsx_tiling','rsx_commands','rsx_dispatch',
    'rsx_texture_layout','rsx_vertex_compact','rsx_vertex_fetch','rsx_vp_decompiler','rsx_fp_decompiler')]
with tempfile.TemporaryDirectory(prefix='original-owner-ingress-') as tmp:
    cc=shutil.which(os.environ.get('CC','clang-cl' if os.name=='nt' else 'cc'))
    if not cc:raise SystemExit('C compiler required for original owner ingress test')
    binary=Path(tmp)/('fixture.exe' if os.name=='nt' else 'fixture')
    if os.name=='nt':
        command=[cc,'/nologo','/std:c17','/O1','/Gy','/Gw','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                 '/Iinclude','/Ilibs/video',*sources,'/Fo'+tmp+'\\','/Fe'+str(binary),'/link','/OPT:REF']
    else:
        command=[cc,'-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections',
                 '-Iinclude','-Ilibs/video',*sources,
                 '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
                 '-pthread','-lm','-o',str(binary)]
    subprocess.run(command,cwd=ROOT,check=True,timeout=60)
    subprocess.run([str(binary)],check=True,timeout=20)
