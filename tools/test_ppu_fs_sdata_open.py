#!/usr/bin/env python3
"""Plain opens of SDATA, cellFsSdataOpen, and durable cellFsWrite (ppu_fs.cpp + edat.c).

On hardware a plain cellFsOpen of a .sdat returns the container's own bytes,
the same ones cellFsStat sizes; only cellFsSdataOpen decrypts. The runtime used
to decrypt on every open, so a title copying its .sdat (MAG installs
game.psarc.sdat this way) got a short final read and a truncated copy, and the
decrypted copy was written beside the source as <file>.dec. This builds
runtime/ppu/tests/test_fs_sdata_open.cpp (ppu_fs.cpp and the real edat.c) and
runs it on a synthetic SDATA it makes itself.

It also runs the same program as `write-and-die`: cellFsWrite 512 bytes, then
_Exit with nothing flushed, the way a killed run ends. The bytes must already be
in the file.

It needs a host C++ compiler (under CI=1 a missing one fails instead of
skipping). No game file, key or GPU is used.

Usage:
    python3 tools/test_ppu_fs_sdata_open.py [--exe-out PATH]
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FIXTURE = os.path.join(ROOT, "runtime", "ppu", "tests", "test_fs_sdata_open.cpp")
INCLUDE_DIRS = ["include", "runtime/ppu", "runtime/memory", "runtime", "libs", "libs/filesystem", "."]


def _cxx():
    return (shutil.which(os.environ.get("CXX", "c++")) or shutil.which("g++")
            or shutil.which("clang++"))


def _stub_header():
    spec = importlib.util.spec_from_file_location("ppu_lifter", os.path.join(TOOLS, "ppu_lifter.py"))
    lifter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(lifter)
    return lifter.HEADER_PREAMBLE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe-out", help="keep the built test program at this path")
    args = ap.parse_args()
    cxx = _cxx()
    if cxx is None:
        if os.environ.get("CI"):
            print("FAIL: no C++ compiler found")
            return 1
        print("skip test_ppu_fs_sdata_open: no C++ compiler")
        return 0
    with tempfile.TemporaryDirectory() as td:
        with open(os.path.join(td, "ppu_recomp.h"), "w") as fh:
            fh.write(_stub_header())
        exe = args.exe_out or os.path.join(td, "test_fs_sdata_open")
        cmd = [cxx, "-std=gnu++17", "-w", "-I" + td]
        cmd += ["-I" + os.path.join(ROOT, d) for d in INCLUDE_DIRS]
        cmd += ["-o", exe, FIXTURE]
        if "mingw" in os.path.basename(cxx):
            cmd += ["-static"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL: fixture did not build")
            print((r.stdout + r.stderr)[-4000:])
            return 1
        scratch = os.path.join(td, "scratch")
        os.mkdir(scratch)
        r = subprocess.run([exe, scratch], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode:
            sys.stderr.write(r.stderr[-4000:])
            return r.returncode

        durable = os.path.join(td, "durable")
        os.makedirs(durable)
        r = subprocess.run([exe, "write-and-die", durable], capture_output=True, text=True)
        path = os.path.join(durable, "ps3recomp_fs_durable.bin")
        data = open(path, "rb").read() if os.path.exists(path) else b""
        want = bytes((0xA5 ^ i) & 0xFF for i in range(512))
        ok = r.returncode == 0 and data == want
        print(("[PASS] " if ok else "[FAIL] ") +
              f"cellFsWrite bytes survive an unflushed process end ({len(data)} of 512 bytes, exit {r.returncode})")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
