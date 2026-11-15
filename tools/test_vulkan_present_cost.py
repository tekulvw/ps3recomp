#!/usr/bin/env python3
"""Vulkan engine presents: a direct present (the default) and a read-back
present (PS3RECOMP_VK_PRESENT_READBACK=1) of the same synthetic frames must
leave identical bytes, both clean under the Khronos validation layer (with
synchronization validation); so must a run with the opt-in present thread
(PS3RECOMP_RSX_PRESENT_THREAD=1), whose shows run beside the recording thread. A periodic frame
dump (PS3RECOMP_VK_DUMP) taken with the present thread, from its staging image, must equal the
same dump taken by a direct run. --timing also reports the present call's CPU
cost in both modes without the layer; --window presents to an SDL window,
--batched records the engine in batches.
Synthetic inputs only.

Needs Vulkan/SDL2 development files and glslang: --glslang-include/
--glslang-lib, or GLSLANG_INCLUDE/GLSLANG_LIB, default the system paths.
Skips (exit 0) without Vulkan."""
from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODES = {"direct": {}, "readback": {"PS3RECOMP_VK_PRESENT_READBACK": "1"},
         "thread": {"PS3RECOMP_RSX_PRESENT_THREAD": "1"}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu-index", default="0", help="PS3RECOMP_VK_DEVICE")
    parser.add_argument("--frames", default="60")
    parser.add_argument("--draws", default="40", help="blended full-screen quads per frame")
    parser.add_argument("--timing", action="store_true", help="also time both modes without the validation layer")
    parser.add_argument("--timing-frames", default="600")
    parser.add_argument("--window", action="store_true", help="present to an SDL window (PS3RECOMP_VK_WINDOW=1)")
    parser.add_argument("--frame-ms", default="0", help="pace each timed frame to at least this long")
    parser.add_argument("--batched", action="store_true", help="batched engine recording (PS3RECOMP_VK_BATCHED_ENGINE=1)")
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
    sources = ["libs/video/tests/test_vulkan_present_cost.c"] + ["libs/video/" + name + ".c" for name in
        ("rsx_draw_engine", "rsx_fifo_busy", "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact",
         "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler")] + [
        "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
        "libs/video/rsx_shader_spirv.cpp"]
    with tempfile.TemporaryDirectory(prefix="vulkan-present-cost-") as tmp:
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
        environment = dict(os.environ, PS3RECOMP_VK_DEVICE=args.gpu_index, VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                           VK_LAYER_SETTINGS_PATH=str(settings))
        loader = subprocess.check_output(["pkg-config", "--variable=libdir", "vulkan"], text=True).strip()
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [loader, environment.get("LD_LIBRARY_PATH", "")]))
        if args.window:
            environment["PS3RECOMP_VK_WINDOW"] = "1"
        if args.batched:
            environment["PS3RECOMP_VK_BATCHED_ENGINE"] = "1"
        outputs = {}
        for mode, extra in MODES.items():
            out = Path(tmp) / f"{mode}.bin"
            result = subprocess.run([str(binary), mode, str(out), args.frames, args.draws], env=dict(environment, **extra),
                                    capture_output=True, text=True, timeout=1800)
            print(result.stdout, end="")
            print("\n".join(line for line in result.stderr.splitlines() if not line.startswith("[RSX vulkan] device")))
            result.check_returncode()
            text = result.stdout + result.stderr
            if "VUID-" in text or "SYNC-HAZARD" in text or "Validation Error" in text or "THREADING ERROR" in text:
                raise SystemExit(f"Vulkan validation rejected the {mode} run")
            outputs[mode] = out.read_bytes()
        if outputs["direct"] != outputs["readback"]:
            raise SystemExit("a direct present left different bytes from a read-back present")
        if outputs["direct"] != outputs["thread"]:
            raise SystemExit("a present-thread run left different bytes from a direct present")
        # A periodic dump: with the present thread it is copied from the staging
        # image of the dump_every-th staged frame; it must equal a direct run's.
        frames = int(args.frames)
        every = str(next(n for n in range(max(1, frames // 3), frames + 1) if frames % n == 0))  # the last frame is dumped
        dumps = {}
        for mode, extra in (("direct", {}), ("thread", MODES["thread"])):
            out = Path(tmp) / f"{mode}-dumped.bin"
            ppm = Path(tmp) / f"{mode}.ppm"
            result = subprocess.run([str(binary), mode, str(out), args.frames, args.draws],
                                    env=dict(environment, **extra, PS3RECOMP_VK_DUMP=str(ppm), PS3RECOMP_VK_DUMP_EVERY=every),
                                    capture_output=True, text=True, timeout=1800)
            print("\n".join(line for line in result.stderr.splitlines() if not line.startswith("[RSX vulkan] device")))
            result.check_returncode()
            text = result.stdout + result.stderr
            if "VUID-" in text or "SYNC-HAZARD" in text or "Validation Error" in text or "THREADING ERROR" in text:
                raise SystemExit(f"Vulkan validation rejected the {mode} run with a frame dump")
            if mode == "thread" and "present thread unsupported" in text:
                raise SystemExit("the present thread refused a run with a frame dump")
            if not ppm.exists() or ppm.stat().st_size < 16:
                raise SystemExit(f"the {mode} run with PS3RECOMP_VK_DUMP wrote no frame dump")
            dumps[mode] = ppm.read_bytes()
            if out.read_bytes() != outputs["direct"]:
                raise SystemExit(f"a {mode} run with a frame dump left different bytes from a direct present")
        if dumps["direct"] != dumps["thread"]:
            raise SystemExit("the present thread's frame dump differs from a direct run's")
        if args.timing:
            plain = {k: v for k, v in environment.items() if k not in ("VK_INSTANCE_LAYERS", "VK_LAYER_SETTINGS_PATH")}
            for mode, extra in MODES.items():
                out = Path(tmp) / f"{mode}-timing.bin"
                result = subprocess.run([str(binary), mode, str(out), args.timing_frames, args.draws, args.frame_ms],
                                        env=dict(plain, **extra), capture_output=True, text=True, timeout=1800)
                result.check_returncode()
                print("timing, no validation: " + " | ".join(l for l in result.stdout.splitlines() if "present CPU" in l))
        print(f"PASS direct and read-back presents leave identical frames ({len(outputs['direct'])} bytes)")


if __name__ == "__main__":
    main()
