#!/usr/bin/env python3
"""CPU-only regression for native publication and descriptor reuse."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sources = ["libs/video/tests/test_texture_native_invalidation.c"] + [
        "libs/video/" + name + ".c" for name in (
            "rsx_fifo_busy", "rsx_commands", "rsx_dispatch", "rsx_texture_layout",
            "rsx_vertex_compact", "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler")]
    with tempfile.TemporaryDirectory(prefix="rsx-texture-native-") as temp:
        binary = str(Path(temp) / "test")
        cmd = shlex.split(os.environ.get("CC", "clang")) + [
            "-std=gnu17", "-O1", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
            "-Iinclude", "-Ilibs/video", *sources,
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
            "-pthread", "-lm", "-o", binary]
        if args.sanitize:
            cmd += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(cmd, cwd=root, check=True, timeout=90)
        subprocess.run([binary], cwd=root, check=True, timeout=90)


if __name__ == "__main__":
    main()
