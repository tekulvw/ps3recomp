"""Build and run the bounded synthetic Metal frontend contract fixture."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-root", type=Path, required=True,
                        help="generated copy_transform and compact directories with manifests")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="frontend-metal-") as temporary:
        binary = Path(temporary) / "fixture"
        command = ["clang", "-O1", "-g", "-UNDEBUG", "-fobjc-arc",
                   "-DPS3RECOMP_DISABLE_NATIVE_OSK", "-Iinclude", "-Ilibs/video",
                   "libs/video/tests/test_frontend_metal.m", "libs/video/rsx_texture_layout.c",
                   "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
                   "libs/input/pad_macos.m", "-Wl,-dead_strip", "-framework", "Metal",
                   "-framework", "Cocoa", "-framework", "QuartzCore", "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=90)
        shaders = args.shader_root.resolve()
        subprocess.run([str(binary), str(shaders / "copy_transform"), str(shaders / "compact")], cwd=ROOT, check=True, timeout=20)

if __name__ == "__main__":
    main()
