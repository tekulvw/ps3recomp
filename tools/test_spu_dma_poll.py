#!/usr/bin/env python3
"""Verify repeated GET polling using synthetic data and real channels."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: DMA polling fixture currently requires a POSIX C compiler")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / "runtime/spu"
common = ["spu_drain.c", "spu_lockstep.c", "spu_coherency.c", "spu_workload.c",
          "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c", "spu_vm_pagemap.c",
          "spurs_policy_blob_weak.c", "spu_interp.c"]
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}
with tempfile.TemporaryDirectory(prefix="spu-dma-poll-") as tmp:
    binary = Path(tmp) / "fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG",
                    *shlex.split(os.environ.get("CFLAGS", "")), "-pthread",
                    "-I" + str(root / "include"), "-I" + str(spu),
                    "-I" + str(root / "runtime/platform"),
                    str(spu / "tests/test_spu_dma_poll.c"),
                    *[str(spu / name) for name in common],
                    str(root / "runtime/platform/win32_compat.c"), "-lm",
                    "-o", str(binary)], check=True)
    for mode in ("get", "getf"):
        subprocess.run([str(binary), mode], check=True, timeout=10, env=env, cwd=tmp)
        # The obsolete heuristic option must not introduce a guest halt.
        subprocess.run([str(binary), mode], check=True, timeout=10,
                       env=dict(env, SPU_DMA_REPEAT_LIMIT="1"), cwd=tmp)
    # SPU_DMA_REPEAT_LOG counts the back-off: with no yield window every poll
    # past the 256th sleeps (~1 ms each), with a huge one none does.
    def episode(backoff_ms):
        r = subprocess.run([str(binary), "get"], check=True, timeout=30, capture_output=True, text=True,
                           env=dict(env, SPU_DMA_REPEAT_LOG="4", SPU_DMA_REPEAT_BACKOFF_MS=backoff_ms), cwd=tmp)
        lines = [l for l in r.stderr.splitlines() if l.startswith("[dma-repeat] spu=") and " img=" in l]
        assert len(lines) == 1, r.stderr
        fields = dict(f.split("=", 1) for f in lines[0].split()[1:])
        return {k: int(fields[k]) for k in ("repeats", "us", "yields", "sleeps", "slept_us")}, r.stderr
    sleeping, stderr = episode("0")
    assert sleeping["sleeps"] >= 512 and sleeping["yields"] == 0, sleeping
    assert sleeping["slept_us"] >= 900 * sleeping["sleeps"] and sleeping["us"] >= sleeping["slept_us"], sleeping
    assert "backoff since start: sleeps=256 " in stderr, stderr
    yielding, _ = episode("100000000")
    assert yielding["sleeps"] == 0 and yielding["slept_us"] == 0 and yielding["yields"] >= 512, yielding
    print("PASS: back-off counters", sleeping, yielding)
