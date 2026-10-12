#!/usr/bin/env python3
"""B8 format crossbar and real Metal glyph-alpha sampling regression."""
import pathlib
import platform
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="ps3recomp-b8-remap-") as tmp:
        binary = pathlib.Path(tmp) / "layout"
        subprocess.run(["clang", "-Iinclude", "-Ilibs/video",
                        "libs/video/tests/test_texture_layout.c",
                        "libs/video/rsx_texture_layout.c", "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30)
        if platform.system() != "Darwin":
            print("SKIP: real Metal B8 sampling requires macOS")
            return
        binary = pathlib.Path(tmp) / "metal"
        subprocess.run(["clang", "-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK",
                        "-Iinclude", "-Ilibs/video",
                        "libs/video/tests/test_metal_b8_remap.m",
                        "libs/video/rsx_texture_layout.c", "libs/input/pad_macos.m",
                        "-Wl,-dead_strip", "-framework", "Metal", "-framework", "Cocoa",
                        "-framework", "QuartzCore", "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
