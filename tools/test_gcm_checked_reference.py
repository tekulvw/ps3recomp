#!/usr/bin/env python3
"""Compile and run the production GCM FIFO checked-reference regression."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import shutil

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-report-") as tmp:
    out = Path(tmp) / "test"
    compiler=shutil.which(os.environ.get('CC','clang-cl' if os.name=='nt' else 'cc'))
    if not compiler: raise SystemExit('C compiler required')
    if os.name=='nt':
        out=out.with_suffix('.exe')
        cmd=[compiler,'/nologo','/std:c17','/O1','/Gy','/Gw','/MT','/W0','/UNDEBUG',
             '/D_CRT_SECURE_NO_WARNINGS','/D_CRT_NONSTDC_NO_DEPRECATE','/I'+str(ROOT/'include'),
             str(ROOT/'libs/video/tests/test_gcm_checked_reference.c'),
             str(ROOT/'libs/video/tests/gcm_deferred_fixture_stubs.c'),str(ROOT/'libs/video/rsx_fifo_busy.c'),
             '/Fo'+tmp+'\\','/Fe'+str(out),'/link','/OPT:REF']
    else:
        cmd = [compiler, "-std=gnu17", "-O1", "-UNDEBUG",
           "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "include"),
           str(ROOT / "libs/video/tests/test_gcm_checked_reference.c"),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           str(ROOT / "libs/video/tests/gcm_deferred_fixture_stubs.c"),str(ROOT / "libs/video/rsx_fifo_busy.c"),
           "-pthread", "-lm", "-o", str(out)]
    subprocess.run(cmd, check=True, timeout=60)

    for mode in ("live_data_ea","park_patch","park_branch","park_wrap","park_invalid","circling","invalid_jump","held","repeat_wrap","unknown_word","malformed","incomplete","success","failure","rescue","overflow","external_success","external_failure","external_notready","external_report_success","external_report_failure","external_held"):
        subprocess.run([str(out),mode],check=True,timeout=10)
