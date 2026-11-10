#!/usr/bin/env python3
"""bind_vs_constant_rows on a real Metal device, headless (no window): a
transform program reading one constant stages only that row and the
viewport transform's, and draws the right colour. Synthetic inputs only."""
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def translation_flags():
    """glslang and SPIRV-Cross (brew install glslang spirv-cross), which the
    Metal backend needs to turn guest programs into MSL; None without them."""
    prefix = os.environ.get("MSL_TRANSLATION_PREFIX")
    if not prefix and shutil.which("brew"):
        prefix = subprocess.run(["brew", "--prefix"], capture_output=True, text=True).stdout.strip()
    if not prefix:
        return None
    inc, lib = pathlib.Path(prefix) / "include", pathlib.Path(prefix) / "lib"
    if not (inc / "glslang").is_dir() or not (inc / "spirv_cross" / "spirv_cross_c.h").is_file():
        return None
    parts = [f"-lspirv-cross-{p}" for p in ("c", "msl", "glsl", "hlsl", "cpp", "reflect", "util", "core")
             if (lib / f"libspirv-cross-{p}.a").is_file()]
    extra = ["-lglslang-default-resource-limits"] if any(lib.glob("libglslang-default-resource-limits*")) else []
    return ["-DPS3RECOMP_HAVE_MSL_TRANSLATION=1", f"-I{inc}", f"-I{inc / 'spirv_cross'}", f"-L{lib}",
            "-lglslang"] + extra + parts


def main():
    if platform.system() != "Darwin":
        print("SKIP: Metal requires macOS")
        return
    flags = translation_flags()
    if flags is None:
        print("SKIP: glslang and spirv-cross not found (brew install glslang spirv-cross)")
        return
    with tempfile.TemporaryDirectory(prefix="metal-constant-rows-") as tmp:
        binary = pathlib.Path(tmp) / "test"
        subprocess.run(["clang", "-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK", "-I", "include",
                        "-I", "libs/video", "libs/video/tests/test_metal_constant_rows.m",
                        "libs/video/rsx_texture_layout.c", "libs/video/rsx_dispatch.c",
                        "libs/video/rsx_vertex_compact.c", "libs/video/rsx_vp_decompiler.c",
                        "libs/video/rsx_fp_decompiler.c", "libs/video/rsx_shader_msl.cpp",
                        "runtime/diagnostics/critical_trace.c",
                        "runtime/diagnostics/render_ablation.c", "libs/video/rsx_fifo_busy.c",
                        "libs/video/tests/gcm_deferred_fixture_stubs.c", "libs/input/pad_macos.m", "-lc++",
                        "-Wl,-dead_strip", "-framework", "Metal", "-framework", "Cocoa",
                        "-framework", "QuartzCore", *flags, "-o", str(binary)],
                       cwd=ROOT, check=True, timeout=180)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
        print(result.stdout.strip())
        if result.returncode:
            raise SystemExit(result.stderr[-4000:] + "\ntest_metal_constant_rows FAILED")


if __name__ == "__main__":
    main()
