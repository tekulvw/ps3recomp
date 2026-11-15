#!/usr/bin/env python3
"""Deferred reports when a FIFO pass stops inside a primitive, and recovery from a failed queue
(libs/video/tests/test_gcm_reports_midprimitive.c, which compiles cellGcmSys.c in)."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name == "nt":
    print("SKIP: GCM FIFO early release fixture requires POSIX mmap/linker support")
    raise SystemExit(0)

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-reports-midprim-") as tmp:
    out = Path(tmp) / "test"
    cmd = [os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
           "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "include"),
           str(ROOT / "libs/video/tests/test_gcm_reports_midprimitive.c"),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           str(ROOT / "libs/video/tests/gcm_deferred_fixture_stubs.c"),str(ROOT / "libs/video/rsx_fifo_busy.c"),
           "-pthread", "-lm", "-o", str(out)]
    subprocess.run(cmd, check=True, timeout=60)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("PS3RECOMP_", "GCM_", "MAG_"))}
    subprocess.run([str(out)], check=True, timeout=10, env=env)
