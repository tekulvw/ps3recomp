#!/usr/bin/env python3
"""Actual GCM target-label observer; synthetic genuine-ready backend contract."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="ring-progress-observer-") as directory:
    binary = Path(directory) / "test"
    command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O2", "-UNDEBUG",
               "-fsanitize=undefined", "-ffunction-sections", "-fdata-sections",
               "-I", str(ROOT / "include"),
               str(ROOT / "libs/video/tests/test_ring_progress_observer.c"),
               str(ROOT / "libs/video/tests/gcm_deferred_fixture_stubs.c"),
               "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
               "-pthread", "-lm", "-o", str(binary)]
    subprocess.run(command, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=10)
