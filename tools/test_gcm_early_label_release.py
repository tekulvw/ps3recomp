#!/usr/bin/env python3
"""Compile and run the real GCM FIFO early label release regression."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name == "nt":
    print("SKIP: GCM FIFO early release fixture requires POSIX mmap/linker support")
    raise SystemExit(0)

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-early-release-") as tmp:
    out = Path(tmp) / "test"
    cmd = [os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
           "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "include"),
           str(ROOT / "libs/video/tests/test_gcm_early_label_release.c"),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           str(ROOT / "libs/video/tests/gcm_deferred_fixture_stubs.c"),
           "-pthread", "-lm", "-o", str(out)]
    subprocess.run(cmd, check=True, timeout=60)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("PS3RECOMP_", "GCM_", "MAG_"))}
    subprocess.run([str(out)], check=True, timeout=10, env=env)
