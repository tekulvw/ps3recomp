"""Finite request lifecycle and actual Metal completed-surface diagnostic tests."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="frame-capture-") as directory:
    directory = Path(directory)
    gate = directory / "gate"
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-Ilibs/video", "tests/rsx_frame_capture_gate.c", "-o", str(gate)], cwd=root, check=True)
    subprocess.run([str(gate)], check=True)
    lifecycle = directory / "lifecycle"
    subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                    "-Ilibs/video", "tests/rsx_frame_capture_lifecycle.c", "-o", str(lifecycle)], cwd=root, check=True)
    subprocess.run([str(lifecycle), "off", str(directory)], check=True)
    subprocess.run([str(lifecycle), "on", str(directory)], check=True)
    gpu = directory / "gpu"
    subprocess.run(["clang", "-O2", "-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK", "-Iinclude", "-Ilibs/video",
                    "libs/video/tests/test_metal_frame_capture.m", "libs/video/rsx_texture_layout.c",
                    "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
                    "libs/input/pad_macos.m", "-Wl,-dead_strip", "-framework", "Metal", "-framework", "Cocoa",
                    "-framework", "QuartzCore", "-o", str(gpu)], cwd=root, check=True, timeout=60)
    subprocess.run([str(gpu), str(directory / "surface.rgba")], check=True, timeout=30)
