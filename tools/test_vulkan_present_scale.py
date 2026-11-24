#!/usr/bin/env python3
"""Vulkan backend: rsx_vulkan_backend_set_present_scale (the display image at
a percentage of the display size, for a host that renders its colour targets
larger) for the engine path's direct, read-back and present-thread presents,
and its refusal on the fixed-function path, at 100, 150, 200 and 300 percent,
every run clean under the Khronos validation layer. Synthetic inputs only.

Needs Vulkan/SDL2 development files and glslang, found as
tools/test_vulkan_batched_engine.py finds them. Skips (exit 0) without Vulkan."""
from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNS = [(p, m) for p in ("100", "150", "200", "300") for m in ("direct", "readback", "thread")] + [("200", "fixed")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-index", default="0", help="PS3RECOMP_VK_DEVICE")
    parser.add_argument("--glslang-include", default=os.environ.get("GLSLANG_INCLUDE"))
    parser.add_argument("--glslang-lib", default=os.environ.get("GLSLANG_LIB"))
    args = parser.parse_args()
    if not shutil.which("pkg-config") or subprocess.run(["pkg-config", "--exists", "vulkan", "sdl2"]).returncode:
        print("SKIP: Vulkan/SDL2 development files not found")
        return
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "vulkan", "sdl2"], text=True))
    libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", "sdl2"], text=True))
    glslang_flags = ["-I" + args.glslang_include] if args.glslang_include else []
    glslang_libs = (["-L" + args.glslang_lib, "-Wl,-rpath," + args.glslang_lib] if args.glslang_lib else []) + [
        "-lglslang", "-lglslang-default-resource-limits"]
    if args.glslang_lib and (Path(args.glslang_lib) / "libSPIRV.so").exists():
        glslang_libs.append("-lSPIRV")
    sources = ["libs/video/tests/test_vulkan_present_scale.c"] + ["libs/video/" + name + ".c" for name in
        ("rsx_draw_engine", "rsx_fifo_busy", "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact",
         "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler")] + [
        "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
        "libs/video/rsx_shader_spirv.cpp"]
    with tempfile.TemporaryDirectory(prefix="vulkan-present-scale-") as tmp:
        objects = []
        for index, source in enumerate(sources):
            output = Path(tmp) / f"{index}.o"
            cxx = source.endswith(".cpp")
            command = [os.environ.get("CXX", "c++") if cxx else os.environ.get("CC", "cc"),
                       "-std=c++17" if cxx else "-std=gnu17", "-O1", "-g", "-UNDEBUG", "-ffunction-sections",
                       "-fdata-sections", "-Iinclude", "-Ilibs/video", *flags]
            if cxx:
                command += ["-DPS3RECOMP_HAVE_SPIRV_TRANSLATION=1", *glslang_flags]
            subprocess.run(command + ["-c", source, "-o", str(output)], cwd=ROOT, check=True, timeout=300)
            objects.append(str(output))
        binary = Path(tmp) / "fixture"
        link = [os.environ.get("CXX", "c++"), *objects, "-Wl,--gc-sections", "-pthread", "-ldl", "-lm",
                *libraries, *glslang_libs, "-o", str(binary)]
        linked = subprocess.run(link, cwd=ROOT, capture_output=True, text=True, timeout=300)
        if linked.returncode:
            # A static glslang (Ubuntu's glslang-dev) needs SPIRV-Tools and the
            # SPIRV library spelled out after it; a shared one does not.
            extra = ["-lSPIRV", "-lSPIRV-Tools-opt", "-lSPIRV-Tools"]
            retried = subprocess.run(link[:-2] + extra + link[-2:], cwd=ROOT, capture_output=True, text=True, timeout=300)
            if retried.returncode:
                sys.stderr.write(linked.stderr + retried.stderr)
                raise SystemExit("linking the fixture failed")
        settings = Path(tmp) / "vk_layer_settings.txt"
        settings.write_text("khronos_validation.validate_core = true\n"
                            "khronos_validation.validate_sync = true\n")
        environment = dict(os.environ, PS3RECOMP_VK_DEVICE=args.gpu_index, VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                           VK_LAYER_SETTINGS_PATH=str(settings))
        loader = subprocess.check_output(["pkg-config", "--variable=libdir", "vulkan"], text=True).strip()
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [loader, environment.get("LD_LIBRARY_PATH", "")]))
        for percent, mode in RUNS:
            result = subprocess.run([str(binary), percent, mode], env=environment,
                                    capture_output=True, text=True, timeout=600)
            print(result.stdout, end="")
            text = result.stdout + result.stderr
            log = [line for line in result.stderr.splitlines() if "present scale " in line or "FAIL" in line]
            print("\n".join(log))
            if result.returncode:
                sys.stderr.write(result.stderr)
                raise SystemExit(f"{percent}% {mode}: exit {result.returncode}")
            if "VUID-" in text or "SYNC-HAZARD" in text or "Validation Error" in text or "THREADING ERROR" in text:
                sys.stderr.write(result.stderr)
                raise SystemExit(f"Vulkan validation rejected the {percent}% {mode} run")
            if percent != "100":
                want = "needs the register-file draw engine" if mode == "fixed" else "present scale " + percent + "%: display image"
                if want not in result.stderr:
                    raise SystemExit(f"{percent}% {mode}: the backend did not report {want!r}")
            elif "present scale " in result.stderr:
                raise SystemExit("100%: the backend reported a present scale it should not have applied")
        print(f"PASS present scale: {len(RUNS)} runs, display image extents and contents as requested")


if __name__ == "__main__":
    main()
