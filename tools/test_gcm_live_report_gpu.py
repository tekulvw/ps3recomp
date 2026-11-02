#!/usr/bin/env python3
"""Actual GCM-library -> LIVE checked-fence publication and GPU pixels."""
import argparse,os,shutil,subprocess,tempfile
from pathlib import Path
def main():
    if os.name!='nt':print('SKIP: Windows D3D12 fixture');return 0
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--runtime-library',type=Path,required=True);p.add_argument('--scenario',choices=('all','batch','dependency','partial'),default='all');a=p.parse_args()
    root=Path(__file__).resolve().parents[1];cc=shutil.which(os.environ.get('CC','clang-cl'))
    if not cc or not a.runtime_library.is_file():p.error('native compiler and /MT library required')
    with tempfile.TemporaryDirectory(prefix='gcm-live-gpu-')as tmp:
        work=Path(tmp);objects=[]
        for i,src in enumerate((root/'libs/video/tests/test_gcm_live_report_gpu.c',root/'runtime/host/host_stubs.c')):
            defines=['/Dps3_ww_report_inline=fixture_inert_write_watch']if i==1 else[]
            obj=work/f'{i}.obj';subprocess.run([cc,'/nologo','/std:c17','/O1','/Gy','/Gw','/W0','/MT','/D_CRT_SECURE_NO_WARNINGS',*defines,'/I'+str(root/'include'),'/I'+str(root/'libs/video'),'/I'+str(root/'runtime'),'/c',str(src),'/Fo'+str(obj)],check=True,timeout=90);objects.append(str(obj))
        exe=work/'fixture.exe';libs=('d3d12','dxgi','d3dcompiler','dxguid','gdi32','user32','ole32','ws2_32','xinput','bcrypt','winmm','shlwapi','synchronization','psapi','dbghelp')
        subprocess.run([cc,'/nologo',*objects,str(a.runtime_library.resolve()),'/Fe'+str(exe),'/link','/OPT:REF',*(x+'.lib'for x in libs)],check=True,timeout=90)
        for mode in (('batch','dependency','partial')if a.scenario=='all'else(a.scenario,)):
            print('SCENARIO',mode,flush=True);r=subprocess.run([str(exe),mode],cwd=work,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60);print(r.stdout,flush=True)
            r.check_returncode();validated=False;writes=0
            for line in r.stdout.splitlines():
                if line.startswith('REAL_STATUS positive'):validated=True
                if line.startswith('[ww-hle]'):
                    if not validated:raise AssertionError('guest label write preceded real fence proof')
                    writes+=1
            if mode!='partial'and writes!=(16 if mode=='batch' else 3):raise AssertionError('actual publication witness count')
    return 0
if __name__=='__main__':raise SystemExit(main())
