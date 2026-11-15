#!/usr/bin/env python3
"""Metal host-format vertex streams (libs/video/tests/test_metal_host_vertex.m):
the host-format vertex descriptor and the encoding of a draw_host record,
headless (a fake encoder, no Metal device)."""
import pathlib, platform, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    if platform.system() != "Darwin":
        print("SKIP: the Metal backend builds on macOS only")
        return
    with tempfile.TemporaryDirectory(prefix="ps3-metal-host-vertex-") as tmp:
        binary = pathlib.Path(tmp) / "fixture"
        subprocess.run(["clang", "-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK", "-I", "include", "-I", "libs/video",
                        "libs/video/tests/test_metal_host_vertex.m", "libs/video/rsx_texture_layout.c",
                        "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
                        "libs/video/rsx_fifo_busy.c", "libs/input/pad_macos.m", "-Wl,-dead_strip",
                        "-framework", "Metal", "-framework", "Cocoa", "-framework", "QuartzCore", "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=180)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        print(result.stdout.strip())
        if result.returncode:
            print(result.stderr[-2000:])
            raise SystemExit("test_metal_host_vertex failed")


if __name__ == "__main__":
    main()
