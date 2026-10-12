#!/usr/bin/env python3
import pathlib,subprocess,tempfile,sys,os
if os.name=='nt':
    print('SKIP standalone engine fixture requires a POSIX compiler');sys.exit(0)
r=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='picture-engine-') as tmp:
    out=pathlib.Path(tmp)/'test'
    files=['libs/video/tests/test_picture_identity_engine.c']+['libs/video/'+x+'.c' for x in ('rsx_draw_engine','rsx_dispatch','rsx_texture_layout','rsx_vertex_compact','rsx_vertex_fetch','rsx_vp_decompiler','rsx_fp_decompiler')]
    subprocess.run(['cc','-std=gnu17','-O1','-ffunction-sections','-fdata-sections','-Iinclude','-Ilibs/video',*files,'-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections','-lm','-o',str(out)],cwd=r,check=True,timeout=60)
    for mode in ("happy","superseded","partial"):subprocess.run([str(out),mode],check=True,timeout=20)
