#!/usr/bin/env python3
"""Actual inline FIFO mutation between immutable LIVE GPU draw snapshots."""
import argparse,os,shutil,subprocess,tempfile
from pathlib import Path
def main():
    if os.name!='nt':print('SKIP Windows D3D12 fixture');return 0
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--runtime-library',type=Path,required=True)
    p.add_argument('--scenario',choices=('all','constants','fp','vertex','index','pending','off','batch-off','partial','failure','pressure','reference'),default='all')
    p.add_argument('--fp-mode',choices=('buffered','literal'),default='buffered')
    a=p.parse_args();root=Path(__file__).resolve().parents[1]
    cc=shutil.which(os.environ.get('CC','clang-cl'))
    if not cc or not a.runtime_library.is_file():p.error('native compiler and /MT runtime required')
    with tempfile.TemporaryDirectory(prefix='gcm-live-inline-')as tmp:
        work=Path(tmp);objects=[]
        for i,source in enumerate((root/'libs/video/tests/test_gcm_live_inline_gpu.c',root/'runtime/host/host_stubs.c')):
            obj=work/f'{i}.obj';defines=['/Dps3_ww_report_inline=fixture_inert_write_watch']if i else[]
            subprocess.run([cc,'/nologo','/std:c17','/O1','/Gy','/Gw','/W0','/MT','/D_CRT_SECURE_NO_WARNINGS',*defines,
                '/I'+str(root/'include'),'/I'+str(root/'libs/video'),'/I'+str(root/'runtime'),'/c',str(source),'/Fo'+str(obj)],check=True,timeout=90);objects.append(str(obj))
        exe=work/'fixture.exe';libs=('d3d12','dxgi','d3dcompiler','dxguid','gdi32','user32','ole32','ws2_32','xinput','bcrypt','winmm','shlwapi','synchronization','psapi','dbghelp')
        subprocess.run([cc,'/nologo',*objects,str(a.runtime_library.resolve()),'/Fe'+str(exe),'/link','/OPT:REF',*(x+'.lib'for x in libs)],check=True,timeout=90)
        modes=('fp','vertex','index','pending','off','batch-off','partial','failure','pressure','reference')if a.scenario=='all'else(
            ('fp','pending','pressure','reference')if a.scenario=='constants'else(a.scenario,))
        for mode in modes:
            print('SCENARIO',mode,'FP',a.fp_mode,flush=True)
            subprocess.run([str(exe),mode],cwd=work,env=dict(os.environ,RSX_FP_CONSTANT_MODE=a.fp_mode),check=True,timeout=60)
    return 0
if __name__=='__main__':raise SystemExit(main())
