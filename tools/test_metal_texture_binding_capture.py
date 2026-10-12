"""Bound texture capture on a real Metal queue, including compressed/remapped sampling."""
import os
from pathlib import Path
import platform
import subprocess
import tempfile

if platform.system() != "Darwin":
    print("SKIP: Metal capture requires macOS")
else:
    root=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="rsx-metal-binding-") as directory:
        directory=Path(directory); binary=directory/"test"
        subprocess.run(["clang","-fobjc-arc","-DPS3RECOMP_DISABLE_NATIVE_OSK","-Iinclude","-Ilibs/video",
            "libs/video/tests/test_metal_texture_binding_capture.m","libs/video/rsx_texture_layout.c",
            "libs/input/pad_macos.m","-Wl,-dead_strip","-framework","Metal","-framework","Cocoa",
            "-framework","QuartzCore","-o",str(binary)],cwd=root,check=True,timeout=120)
        subprocess.run([str(binary),str(directory/"texture.rgba")],check=True,timeout=30)
