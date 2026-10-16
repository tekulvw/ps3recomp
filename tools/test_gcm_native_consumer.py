#!/usr/bin/env python3
"""Verify native-consumer launch configuration and actual event/publication semantics."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
if os.name == "nt":
    print("SKIP: consumer fixture requires POSIX mmap")
    raise SystemExit(0)
sdk = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="gcm-native-consumer-") as tmp:
    out = Path(tmp) / "test"
    flags = ["-fsanitize=" + os.environ["SANITIZERS"], "-fno-sanitize-recover=all"] if os.getenv("SANITIZERS") else []
    command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O3", "-UNDEBUG", *flags,
               "-ffunction-sections", "-fdata-sections", "-I" + str(sdk / "include"),
               str(sdk / "libs/video/tests/test_gcm_native_consumer.c"),
               str(sdk / "libs/video/tests/gcm_deferred_fixture_stubs.c"),
               str(sdk / "runtime/platform/win32_compat.c"),
               "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
               "-pthread", "-lm", "-o", str(out)]
    subprocess.run(command, check=True, timeout=60)
    for mode in ("0", "1"):
        subprocess.run([str(out), mode], check=True, timeout=10)
    print("native consumer launch config and genuine event publication: PASS")
