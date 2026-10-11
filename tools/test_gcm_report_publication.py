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
           str(ROOT / "libs/video/tests/test_gcm_report_publication.c"),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           "-pthread", "-lm", "-o", str(out)]
    subprocess.run(cmd, check=True, timeout=60)
    subprocess.run([str(out)], check=True, timeout=10)
