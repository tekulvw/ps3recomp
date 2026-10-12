#!/usr/bin/env python3
"""Compile and run the real GCM FIFO guest-report regression."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name == "nt":
    print("SKIP: GCM FIFO publication fixture requires POSIX mmap/linker support")
    raise SystemExit(0)

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-report-") as tmp:
    out = Path(tmp) / "test"
    cmd = [os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
           "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "include"),
           str(ROOT / "libs/video/tests/test_gcm_checked_reference.c"),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           "-pthread", "-lm", "-o", str(out)]
    subprocess.run(cmd, check=True, timeout=60)
    
    for mode in ("park_patch","park_branch","park_wrap","held","repeat_wrap","malformed","incomplete","success","failure","rescue","overflow"):
        subprocess.run([str(out),mode],check=True,timeout=10)
