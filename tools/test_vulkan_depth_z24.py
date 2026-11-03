#!/usr/bin/env python3
"""Vulkan GPU Z24S8 depth store, alias texture, GPU-only depth view and
completion tickets on a real device, headless, under the Khronos validation
layer (any VUID fails the run). Synthetic inputs only.

Skips (exit 0) where pkg-config cannot find Vulkan; --require-hardware makes the
named GPU mandatory."""
from pathlib import Path
import argparse
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# The test's own texel reader: texelFetch through the image view (so its
# component mapping applies) into floats.
FETCH_GLSL = """#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(std430, set = 0, binding = 1) buffer Out { vec4 o[]; };
layout(push_constant) uniform PC { uint w; uint h; } pc;
void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    if (p.x >= pc.w || p.y >= pc.h) return;
    o[p.y * pc.w + p.x] = texelFetch(tex, ivec2(p), 0);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--require-hardware", action="store_true")
    parser.add_argument("--gpu-index", default="0")
    parser.add_argument("--vendor-id", default="0")
    parser.add_argument("--device-id", default="0")
    parser.add_argument("--exhaustive", action="store_true", help="every float bit pattern in [0, 1]")
    args = parser.parse_args()
    if not shutil.which("pkg-config") or subprocess.run(["pkg-config", "--exists", "vulkan", "sdl2"]).returncode:
        print("SKIP: Vulkan/SDL2 development files not found")
        return
    glslang = shutil.which("glslangValidator")
    if not glslang:
        raise SystemExit("glslangValidator is required to build the fixture's texel reader")
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "vulkan", "sdl2"], text=True))
    libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", "sdl2"], text=True))
    sources = ["libs/video/tests/test_vulkan_depth_z24.c"] + ["libs/video/" + name + ".c" for name in
        ("rsx_draw_engine", "rsx_commands", "rsx_dispatch", "rsx_texture_layout", "rsx_vertex_compact",
         "rsx_vertex_fetch", "rsx_vp_decompiler", "rsx_fp_decompiler")] + [
        "runtime/diagnostics/critical_trace.c", "runtime/diagnostics/render_ablation.c",
        "libs/video/rsx_shader_spirv.cpp"]
    with tempfile.TemporaryDirectory(prefix="vulkan-depth-z24-") as tmp:
        glsl = Path(tmp) / "vk_fetch.comp"
        glsl.write_text(FETCH_GLSL)
        subprocess.run([glslang, "-V", "--target-env", "vulkan1.0", "--vn", "vk_fetch_comp_spv",
                        "-o", str(Path(tmp) / "vk_fetch_comp.spv.h"), str(glsl)], check=True, timeout=60,
                       stdout=subprocess.DEVNULL)
        objects = []
        for index, source in enumerate(sources):
            output = Path(tmp) / f"{index}.o"
            cxx = source.endswith(".cpp")
            command = [os.environ.get("CXX", "c++") if cxx else os.environ.get("CC", "cc"),
                       "-std=c++17" if cxx else "-std=gnu17", "-O1", "-g", "-UNDEBUG", "-ffunction-sections",
                       "-fdata-sections", "-Iinclude", "-Ilibs/video", "-I" + tmp, *flags, "-c", source, "-o", str(output)]
            subprocess.run(command, cwd=ROOT, check=True, timeout=120)
            objects.append(str(output))
        binary = Path(tmp) / "fixture"
        subprocess.run([os.environ.get("CXX", "c++"), *objects, "-Wl,--gc-sections", "-pthread", "-ldl", "-lm",
                        *libraries, "-o", str(binary)], cwd=ROOT, check=True, timeout=120)
        if args.build_only:
            print("Vulkan depth z24 fixture compiled; GPU execution pending")
            return
        environment = dict(os.environ, PS3RECOMP_VK_DEVICE=args.gpu_index, VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation")
        if args.exhaustive:
            environment["DEPTH_Z24_EXHAUSTIVE"] = "1"
        loader = subprocess.check_output(["pkg-config", "--variable=libdir", "vulkan"], text=True).strip()
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None, [loader, environment.get("LD_LIBRARY_PATH", "")]))
        for mode, batched in (("on", "0"), ("off", "0"), ("on", "1"), ("off", "1")):
            run_environment = dict(environment, PS3RECOMP_VK_BATCHED_ENGINE=batched)
            print(f"--- PS3RECOMP_VK_GPU_Z24 {mode}, PS3RECOMP_VK_BATCHED_ENGINE={batched}")
            result = subprocess.run([str(binary), mode, str(int(args.require_hardware)), args.vendor_id, args.device_id],
                                    env=run_environment, capture_output=True, text=True, timeout=1800)
            print(result.stdout, end="")
            print(result.stderr, end="")
            result.check_returncode()
            if "VUID-" in result.stdout + result.stderr:
                raise SystemExit("Vulkan validation rejected the fixture")
            if "Validation Error" in result.stdout + result.stderr:
                raise SystemExit("Vulkan validation reported an error")


if __name__ == "__main__":
    main()
