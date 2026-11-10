#!/usr/bin/env python3
"""Native actual-cellGcm report/label batching and failure boundaries."""
import os,shutil,subprocess,tempfile,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='gcm-live-report-')as tmp:
    cc=shutil.which(os.environ.get('CC','clang-cl'if os.name=='nt'else'cc'))
    if not cc:raise SystemExit('C compiler required')
    out=Path(tmp)/('fixture.exe'if os.name=='nt'else'fixture')
    sources=[str(ROOT/'libs/video/tests/test_gcm_live_report_batch.c'),str(ROOT/'libs/video/tests/gcm_deferred_fixture_stubs.c'),str(ROOT/'libs/video/rsx_fifo_busy.c'),str(ROOT/'libs/video/rsx_tiling.c')]
    if os.name=='nt':cmd=[cc,'/nologo','/std:c17','/O1','/Gy','/Gw','/MT','/W0','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS','/D_CRT_NONSTDC_NO_DEPRECATE','/I'+str(ROOT/'include'),*sources,'/Fo'+tmp+'\\','/Fe'+str(out),'/link','/OPT:REF']
    else:
        sources.append(str(ROOT/'runtime/platform/win32_compat.c'))
        dead_strip='-Wl,-dead_strip'if sys.platform=='darwin'else'-Wl,--gc-sections'
        cmd=[cc,'-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-I',str(ROOT/'include'),*sources,dead_strip,'-pthread','-lm','-o',str(out)]
    subprocess.run(cmd,check=True,timeout=60)
    for mode in (sys.argv[1:]or('off','legacy','batch1','batch32','fifo-mixed','idle','read','capacity','reentrant','held','snapshot','map-changed','swap','pump','frozen-config','partial','stale','issue-failure','status-failure','life-during-issue','effect-ref','effect-flip','effect-user','effect-failure','effect-notify','host-label','release-get','inline','vm','timestamp','pointer-read')):
        env=os.environ.copy();env['PS3RECOMP_RSX_DEFERRED_REPORTS']='0'if mode=='off'else'1';env['PS3RECOMP_GCM_LIVE_REPORT_BATCH']=env['PS3RECOMP_RSX_DEFERRED_REPORTS']
        env['PS3RECOMP_RSX_DEFERRED_REPORT_BATCH']='1'if mode=='batch1'else'32'
        if mode=='legacy':env['PS3RECOMP_GCM_LIVE_REPORT_BATCH']='0'
        print('MODE',mode,flush=True);subprocess.run([str(out),mode],check=True,timeout=10,env=env)
