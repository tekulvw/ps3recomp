#!/usr/bin/env python3
"""SPU writes store each 128-byte line's first word last (docs/SPU_PUT_LINE_ORDER.md)."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: fixture currently requires a POSIX C compiler")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items() if not k.startswith(("SPU_", "SPURS_", "PS3RECOMP_"))}
with tempfile.TemporaryDirectory(prefix="spu-put-line-order-") as tmp:
    binary = Path(tmp) / "fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG", "-pthread",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    "-I" + str(root / "include"), "-I" + str(root / "runtime/spu"),
                    str(root / "runtime/spu/tests/test_spu_put_line_order.c"),
                    "-o", str(binary)], check=True, timeout=300)
    result = subprocess.run([str(binary)], env=env, cwd=tmp, capture_output=True, text=True, timeout=60)
    print(result.stdout, end="")
    print(result.stderr, end="")
    raise SystemExit(result.returncode)
