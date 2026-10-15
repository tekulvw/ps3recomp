#!/usr/bin/env python3
"""Synthetic WPOS sample-grid and real shared-engine cache regression."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    if os.name == "nt":
        print("SKIP: neutral fixture requires a POSIX C toolchain")
        return
    with tempfile.TemporaryDirectory(prefix="rsx-wpos-") as directory:
        binary = Path(directory) / "test"
        sources = ["tests/test_rsx_wpos_scale.c", "rsx_draw_engine.c", "rsx_dispatch.c",
                   "rsx_vertex_compact.c", "rsx_texture_layout.c", "rsx_vp_decompiler.c", "rsx_fp_decompiler.c"]
        subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=gnu17", "-Iinclude", "-Ilibs/video",
                        "-o", str(binary), *["libs/video/" + s for s in sources]], cwd=ROOT, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
