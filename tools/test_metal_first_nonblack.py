#!/usr/bin/env python3
"""Exercise opt-in capture with real Metal textures on macOS."""
import pathlib
import platform
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    if platform.system() != "Darwin":
        print("SKIP: first-nonblack Metal capture requires macOS")
        return
    with tempfile.TemporaryDirectory(prefix="ps3recomp-metal-capture-") as tmp:
        binary = pathlib.Path(tmp) / "test_metal_first_nonblack"
        subprocess.run([
            "clang", "-fobjc-arc", "-I", "include", "-I", "libs/video",
            "libs/video/tests/test_metal_first_nonblack.m",
            "libs/video/rsx_texture_layout.c", "libs/input/pad_macos.m", "-Wl,-dead_strip",
            "-framework", "Metal", "-framework", "Cocoa", "-framework", "QuartzCore",
            "-o", str(binary),
        ], cwd=ROOT, check=True, timeout=120)
        for mode in ("first", "black", "bgra", "windowed", "presentation", "retry", "default", "disabled", "managed-headless", "managed-windowed", "managed-bgra"):
            subprocess.run([str(binary), mode, str(pathlib.Path(tmp) / f"{mode}.ppm")],
                           check=True, timeout=30)


if __name__ == "__main__":
    main()
