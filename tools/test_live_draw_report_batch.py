#!/usr/bin/env python3
"""Synthetic Windows LIVE checked report-batch adapter and upload pressure."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    if os.name != 'nt':
        print('SKIP: Windows D3D12 fixture'); return 0
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime-library',type=Path,required=True)
    p.add_argument('--scenario',default='all')
    p.add_argument('--vertex-mode',choices=('L','H','M','C'))
    a=p.parse_args();root=Path(__file__).resolve().parents[1]
    compiler=shutil.which(os.environ.get('CC','clang-cl'))
    if not compiler or not a.runtime_library.is_file():p.error('Native compiler and /MT runtime library required')
    with tempfile.TemporaryDirectory(prefix='live-report-batch-') as temp:
        work=Path(temp);objects=[]
        stub=work/'stub.exe'
        subprocess.run([compiler,'/nologo','/std:c17','/O1','/MT','/W4','/WX','/I'+str(root/'include'),
            str(root/'libs/video/tests/test_live_draw_report_batch_stub.c'),'/Fe'+str(stub),'/Fo'+str(work/'stub.obj')],check=True,timeout=60)
        subprocess.run([str(stub)],check=True,timeout=10)
        print('PASS non-Windows API stub (no GPU)',flush=True)
        for i,source in enumerate((root/'libs/video/tests/test_live_draw_report_batch.c',root/'runtime/host/host_stubs.c')):
            obj=work/(str(i)+'.obj')
            subprocess.run([compiler,'/nologo','/std:c17','/O1','/Gy','/Gw','/W0','/MT','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'include'),'/I'+str(root/'libs/video'),'/I'+str(root/'runtime'),'/c',str(source),'/Fo'+str(obj)],check=True,timeout=90)
            objects.append(str(obj))
        exe=work/'fixture.exe'
        libs=('d3d12','dxgi','d3dcompiler','dxguid','gdi32','user32','ole32','ws2_32','xinput','bcrypt','winmm','shlwapi','synchronization','psapi','dbghelp')
        subprocess.run([compiler,'/nologo',*objects,str(a.runtime_library.resolve()),'/Fe'+str(exe),'/link','/OPT:REF',*(x+'.lib' for x in libs)],check=True,timeout=90)
        scenarios=('mip-pressure','cube-pressure','oversize','draw-pressure','multi-texture-pressure','pressure-failure','adapter','lease','status-failures',
            'failure-close','failure-signal','failure-fence','failure-wait','failure-device','failure-reset','failure-list-reset') if a.scenario=='all' else (
            ('draw-pressure','multi-texture-pressure','pressure-failure') if a.scenario=='preparation' else (a.scenario,))
        for mode in scenarios:
            print('SCENARIO',mode,flush=True)
            env=dict(os.environ)
            if a.vertex_mode:env['RSX_VERTEX_MODE']=a.vertex_mode
            subprocess.run([str(exe),mode],cwd=work,env=env,check=True,timeout=60)
    return 0
if __name__=='__main__':raise SystemExit(main())
