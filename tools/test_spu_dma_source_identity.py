#!/usr/bin/env python3
"""Verify same-record caller identity and sequence with independent synthetic PUTs."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: fixture currently requires a POSIX C compiler")
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items()
       if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}
flag = {"SPU_DMA_SOURCE_F32BE": "1", "SPU_DMA_RANGE": "12000-12120"}
with tempfile.TemporaryDirectory(prefix="spu-dma-identity-") as tmp:
    binary = Path(tmp)/"fixture"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG", "-pthread",
                    *shlex.split(os.environ.get("CFLAGS", "")),
                    "-I"+str(root/"include"), "-I"+str(root/"runtime/spu"),
                    str(root/"runtime/spu/tests/test_spu_dma_source_identity.c"),
                    "-o", str(binary)], check=True)
    def run(extra, mode=None):
        result = subprocess.run([str(binary)] + ([mode] if mode else []), env=env | extra,
                                cwd=tmp, capture_output=True, text=True, timeout=5, check=True)
        contexts = {int(line.split()[1].split("=")[1]): line.split()[2].split("=")[1]
                    for line in result.stdout.splitlines() if line.startswith("fixture-context")}
        records = [dict(field.split("=", 1) for field in line.split()[1:])
                   for line in result.stderr.splitlines() if line.startswith("[dma-source-f32be]")]
        return contexts, records
    for extra in ({}, {"SPU_DMA_RANGE": "12000-12120"},
                  flag | {"SPU_DMA_SOURCE_F32BE": "0"},
                  flag | {"SPU_DMA_SOURCE_F32BE": "true"},
                  {"SPU_DMA_SOURCE_F32BE": "1"},
                  flag | {"SPU_DMA_RANGE": "invalid"}):
        assert run(extra, "disabled")[1] == [], "disabled poison access/emission"
    contexts, records = run(flag)
    assert len(records) == 5, records
    assert contexts[0] != contexts[1], contexts
    expected = [(0, 1), (1, 2), (0, 3), (1, 4), (0, 5)]
    def check(record, index, sequence):
        assert record["ctx"] == contexts[index], record
        assert record["seq"] == str(sequence), record
        assert record["r0"] == ("0xA1234567", "0xB1234567")[index], record
        assert record["r1"] == ("0xC7654321", "0xD7654321")[index], record
        assert record["img"] == "7" and record["pc"] == "0x02340", record
        assert record["ea"] == ("0x00012000", "0x00012100")[index], record
        assert record["cmd"] == "0x20" and record["words"] == "8", record
        assert record["nonzero"] == ("3", "1")[index], record
        assert record["peak"] == ("2.5", "4")[index], record
    for record, (index, sequence) in zip(records, expected):
        check(record, index, sequence)
    # Threads contend across two contexts, with three separately admitted windows.
    contexts, records = run(flag, "threads")
    assert len(records) == 3, records
    for sequence, record in enumerate(records, 1):
        index = 0 if record["ctx"] == contexts[0] else 1
        check(record, index, sequence)
print("PASS: two-context direct/list PUT caller identity, raw preferred lane, shared trace sequence, disabled poison safety, concurrent global rate limit, and unchanged DMA bytes")
