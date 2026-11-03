#!/usr/bin/env python3
"""GPU Z24S8 store, its alias texture and the GPU-only depth view on a real Metal device."""
import pathlib
import platform
import subprocess
import tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
if platform.system() != "Darwin":
    print("SKIP: Metal requires macOS")
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="metal-depth-z24-") as tmp:
    binary = pathlib.Path(tmp) / "test"
    subprocess.run(["clang", "-fobjc-arc", "-Iinclude", "-Ilibs/video",
                    "libs/video/tests/test_metal_depth_z24.m",
                    "libs/video/rsx_texture_layout.c", "libs/video/rsx_shader_msl.cpp",
                    "libs/video/tests/gcm_deferred_fixture_stubs.c",
                    "runtime/diagnostics/render_ablation.c", "-lc++", "-Wl,-dead_strip",
                    "-framework", "Metal", "-framework", "Cocoa",
                    "-framework", "QuartzCore", "-o", str(binary)],
                   cwd=ROOT, check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=120)
