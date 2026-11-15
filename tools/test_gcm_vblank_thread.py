#!/usr/bin/env python3
"""PS3RECOMP_GCM_VBLANK_THREAD: the 60 Hz beat on its own thread, host ticks ignored, executed flips completed within a period."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
if os.name == "nt":
    print("SKIP: consumer fixture requires POSIX mmap")
    raise SystemExit(0)
sdk = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-vblank-thread-") as tmp:
    out = Path(tmp) / "test"
    flags = ["-fsanitize=" + os.environ["SANITIZERS"], "-fno-sanitize-recover=all"] if os.getenv("SANITIZERS") else []
    command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O3", "-UNDEBUG", *flags,
               "-ffunction-sections", "-fdata-sections", "-I" + str(sdk / "include"),
               str(sdk / "libs/video/tests/test_gcm_vblank_thread.c"),
               str(sdk / "libs/video/tests/gcm_deferred_fixture_stubs.c"), str(sdk / "libs/video/rsx_fifo_busy.c"),
               str(sdk / "runtime/platform/win32_compat.c"),
               "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
               "-pthread", "-lm", "-o", str(out)]
    subprocess.run(command, check=True, timeout=60)
    subprocess.run([str(out)], check=True, timeout=30)
