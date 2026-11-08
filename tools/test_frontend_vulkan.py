"""Bounded Vulkan frontend fixture; hardware acceptance is an explicit mode."""
import argparse
import hashlib
import json
from pathlib import Path
import os
import shlex
import subprocess
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def shader_header(root, output):
    rows = ["#include <stdint.h>",
            "typedef struct shader_fixture { const uint32_t *source; uint64_t length; const char *entry; const rsx_frontend_binding_layout *bindings; uint32_t count, threads[3]; } shader_fixture;"]
    hashes = {}
    for name in ("copy_transform", "compact"):
        directory = root / name
        payload = (directory / "shader.spv").read_bytes()
        manifest = json.loads((directory / "manifest.json").read_text())
        if manifest["schema_version"] != 1 or len(payload) % 4:
            raise ValueError("invalid source-bound shader fixture")
        words = struct.unpack(f"<{len(payload) // 4}I", payload)
        rows.append(f"static const uint32_t {name}_spirv[] = {{" + ",".join(hex(word) for word in words) + "};")
        size_indices = {item["binding"]: item["offset_bytes"] // 4 for item in manifest["metal_sizes_buffer"]["entries"]}
        rows.append(f"static const rsx_frontend_binding_layout {name}_bindings[] = {{")
        for binding in manifest["buffers"]:
            runtime = binding["binding"] in size_indices
            rows.append("{" + ",".join(str(value) for value in [binding["binding"],
                "RSX_FRONTEND_READ" if binding["access"] == "read" else "RSX_FRONTEND_READ_WRITE",
                binding["minimum_binding_bytes"], binding["layout"]["stride_bytes"] if runtime else 0,
                size_indices.get(binding["binding"], "RSX_FRONTEND_NO_SIZE_INDEX"), binding["layout"]["alignment_bytes"],
                "RSX_FRONTEND_UNIFORM" if binding["address_space"] == "uniform" else "RSX_FRONTEND_STORAGE"]) + "},")
        rows.append("};")
        threads = ",".join(str(value) for value in manifest["workgroup_size"])
        rows.append(f"static const shader_fixture {name}_fixture = {{{name}_spirv, sizeof({name}_spirv), " +
                    json.dumps(manifest["targets"]["spirv"]["entry_point"]) + f", {name}_bindings, {len(manifest['buffers'])}, {{{threads}}}}};")
        for filename in ("shader.spv", "manifest.json"):
            hashes[f"{name}/{filename}"] = hashlib.sha256((directory / filename).read_bytes()).hexdigest()
    output.write_text("\n".join(rows) + "\n")
    return hashes

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-hardware", action="store_true")
    parser.add_argument("--vendor-id", default="0")
    parser.add_argument("--device-id", default="0")
    parser.add_argument("--gpu-index", default="0")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--require-validation", action="store_true", help="require Khronos validation layer and reject every VUID")
    parser.add_argument("--shader-root", type=Path)
    parser.add_argument("--contract-only", action="store_true", help="CPU-only injected driver-fault boundary")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.report:
        args.report.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="frontend-vulkan-") as temporary:
        binary = Path(temporary) / "fixture"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "vulkan"], text=True))
        contract = Path(temporary) / "contract"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c17", "-Wall", "-Wextra", "-Werror", "-Ilibs/video",
                        *flags, "libs/video/tests/test_frontend_vulkan_contract.c", "libs/video/rsx_frontend.c",
                        "-pthread", "-o", str(contract)], cwd=ROOT, check=True, timeout=60)
        subprocess.run([str(contract)], check=True, timeout=5)
        if args.contract_only:
            return
        if not args.shader_root:
            parser.error("--shader-root is required unless --contract-only is used")
        shader_hashes = shader_header(args.shader_root, Path(temporary) / "frontend_shader_fixture.h")
        flags += shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
        command = [os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-g", "-UNDEBUG",
                   "-ffunction-sections", "-fdata-sections", "-Iinclude", "-Ilibs/video", "-I" + temporary,
                   "libs/video/tests/test_frontend_vulkan.c", "libs/video/rsx_frontend.c",
                   "libs/video/rsx_frontend_vulkan.c", "-Wl,--gc-sections", "-pthread", "-ldl", "-lm",
                   *flags, "-o", str(binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=90)
        if not args.build_only:
            environment = dict(os.environ, PS3RECOMP_VK_DEVICE=args.gpu_index)
            if args.require_validation:
                environment["MAG_FRONTEND_REQUIRE_VALIDATION"] = "1"
            # SDK uses dlopen rather than linking Vulkan. Ephemeral package
            # environments may expose headers/pkg-config without loader search
            # paths; set only this owned subprocess's library path.
            loader_directory = subprocess.check_output(["pkg-config", "--variable=libdir", "vulkan"], text=True).strip()
            environment["LD_LIBRARY_PATH"] = os.pathsep.join(filter(None,
                [loader_directory, environment.get("LD_LIBRARY_PATH", "")]))
            result = subprocess.run([str(binary), str(int(args.require_hardware)), args.vendor_id, args.device_id],
                                    cwd=ROOT, env=environment, timeout=20, capture_output=True, text=True)
            print(result.stdout, end=""); print(result.stderr, end="")
            result.check_returncode()
            if "VUID-" in result.stdout + result.stderr:
                raise RuntimeError("Vulkan validation rejected the fixture; no success receipt written")
            if args.report:
                sources = ["libs/video/rsx_frontend.h", "libs/video/rsx_frontend.c", "libs/video/rsx_frontend_vulkan.h",
                           "libs/video/rsx_frontend_vulkan.c", "libs/video/rsx_vulkan_backend.c",
                           "libs/video/tests/test_frontend_vulkan.c", "libs/video/tests/test_frontend_vulkan_contract.c",
                           "tools/test_frontend_vulkan.py"]
                record = {"status": "passed", "hardware_required": args.require_hardware,
                          "validation_required": args.require_validation,
                          "execution": "hardware" if args.require_hardware else "diagnostic_only",
                          "gpu_index": args.gpu_index, "vendor_id": args.vendor_id, "device_id": args.device_id,
                          "source_hashes": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in sources},
                          "shader_hashes": shader_hashes, "stdout": result.stdout, "stderr": result.stderr}
                args.report.parent.mkdir(parents=True, exist_ok=True)
                args.report.write_text(json.dumps(record, indent=2) + "\n")

if __name__ == "__main__":
    main()
