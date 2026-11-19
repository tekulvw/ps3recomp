#!/usr/bin/env python3
"""The Vulkan backend driven by the register-file engine with and without
PS3RECOMP_RSX_ENCODE_THREAD, per-operation and batched (with pass merging and
descriptor reuse): every run must store the same frame to guest memory, clean
under the Khronos validation layer with synchronization validation. Synthetic
inputs only, no window.

Needs Vulkan/SDL2 development files and glslang (the engine translates its
HLSL with it): --glslang-include/--glslang-lib, or GLSLANG_INCLUDE/GLSLANG_LIB,
default the system paths. Skips (exit 0) without Vulkan."""
from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BATCHED = {"PS3RECOMP_VK_BATCHED_ENGINE": "1", "PS3RECOMP_VK_MERGE_PASSES": "1", "PS3RECOMP_VK_DESCRIPTOR_REUSE": "1"}
RUNS = (("sync", "direct", {}), ("sync", "thread", {}), ("batched", "direct", BATCHED), ("batched", "thread", BATCHED))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-index", default="0", help="PS3RECOMP_VK_DEVICE")
    parser.add_argument("--glslang-include", default=os.environ.get("GLSLANG_INCLUDE"))
    parser.add_argument("--glslang-lib", default=os.environ.get("GLSLANG_LIB"))
    parser.add_argument("--repeat", type=int, default=3, help="threaded runs per configuration")
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
    sources = ["libs/video/tests/test_vulkan_encode_thread.c"] + ["libs/video/" + name + ".c" for name in
        ("rsx_draw_engine", "rsx_fifo_busy", "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact",
         "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler", "rsx_encode_queue")] + [
        "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
        "libs/video/rsx_shader_spirv.cpp"]
    with tempfile.TemporaryDirectory(prefix="vulkan-encode-thread-") as tmp:
        objects = []
        for index, source in enumerate(sources):
            output = Path(tmp) / f"{index}.o"
            cxx = source.endswith(".cpp")
            command = [os.environ.get("CXX", "c++") if cxx else os.environ.get("CC", "cc"),
                       "-std=c++17" if cxx else "-std=gnu17", "-O1", "-g", "-UNDEBUG", "-ffunction-sections",
                       "-fdata-sections", "-Iinclude", "-Ilibs/video", *flags]
            if cxx:
                command += ["-DPS3RECOMP_HAVE_SPIRV_TRANSLATION=1", *glslang_flags]
            subprocess.run(command + ["-c", source, "-o", str(output)], cwd=ROOT, check=True, timeout=120)
            objects.append(str(output))
        binary = Path(tmp) / "fixture"
        subprocess.run([os.environ.get("CXX", "c++"), *objects, "-Wl,--gc-sections", "-pthread", "-ldl", "-lm",
                        *libraries, *glslang_libs, "-o", str(binary)], cwd=ROOT, check=True, timeout=120)
        settings = Path(tmp) / "vk_layer_settings.txt"
        settings.write_text("khronos_validation.validate_core = true\n"
                            "khronos_validation.validate_sync = true\n")
        base = dict(os.environ, PS3RECOMP_VK_DEVICE=args.gpu_index, VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                    VK_LAYER_SETTINGS_PATH=str(settings), PS3RECOMP_RSX_INFLIGHT_LOG="1")
        for key in BATCHED:
            base.pop(key, None)
        loader = subprocess.check_output(["pkg-config", "--variable=libdir", "vulkan"], text=True).strip()
        base["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [loader, base.get("LD_LIBRARY_PATH", "")]))
        reference = None
        for recording, mode, extra in RUNS:
            for attempt in range(args.repeat if mode == "thread" else 1):
                out = Path(tmp) / f"{recording}-{mode}-{attempt}.bin"
                result = subprocess.run([str(binary), mode, str(out)], env=dict(base, **extra),
                                        capture_output=True, text=True, timeout=600)
                text = result.stdout + result.stderr
                print(f"[{recording} {mode} {attempt}] " + " | ".join(
                    l for l in text.splitlines() if "checks=" in l or "ops=" in l or "SKIP" in l))
                if "SKIP:" in result.stderr:
                    print(result.stderr.strip())
                    return
                if result.returncode:
                    raise SystemExit(text[-4000:] + f"\n{recording} {mode} failed")
                if "VUID-" in text or "SYNC-HAZARD" in text or "Validation Error" in text or "THREADING ERROR" in text:
                    raise SystemExit(text[-4000:] + f"\nVulkan validation rejected the {recording} {mode} run")
                frame = out.read_bytes()
                if reference is None:
                    reference = frame
                elif frame != reference:
                    diff = sum(a != b for a, b in zip(reference, frame))
                    raise SystemExit(f"{recording} {mode} run {attempt} stores a different frame ({diff} bytes)")
        print(f"PASS per-operation and batched recording, with and without the encode thread, store the same "
              f"frame ({len(reference)} bytes), clean under synchronization validation")


if __name__ == "__main__":
    main()
