#!/usr/bin/env python3
"""PS3RECOMP_RSX_VERTEX_SIMD: rsx_vertex_fetch_many's vector decode against
its scalar loop (libs/video/tests/test_vertex_fetch_simd.c). Builds and runs
the fixture for the host; on an arm64 macOS host with Rosetta it also builds
the x86-64 SSE4.1 form and runs it under `arch -x86_64`, so both vector paths
are checked from one machine. --mutants also builds each listed mutation of
the vector code and requires the fixture to catch it. Synthetic inputs only;
POSIX (the fixture uses mmap guard pages)."""
from pathlib import Path
import argparse
import os
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "libs/video/rsx_vertex_compact.c"
FIXTURE = ROOT / "libs/video/tests/test_vertex_fetch_simd.c"
OTHER = [ROOT / "libs/video/rsx_dispatch.c"]

# Each mutant is a list of (old, new) text replacements over
# rsx_vertex_compact.c; at least one must apply, and every one that applies
# must be unique in the file. Pairs cover the x86 and arm64 forms of the same
# step, so one mutant breaks whichever form the build has.
MUTANTS = {
    "half-exponent-bias": [("_mm_set1_epi32(112)", "_mm_set1_epi32(113)"),
                           ("vdupq_n_u32(112)", "vdupq_n_u32(113)")],
    "half-denormal-not-flushed": [("return _mm_blendv_epi8(r, sign, is0);", "return r;"),
                                  ("return vbslq_u32(is0, sign, r);", "return r;")],
    "half-infinity-bits": [("_mm_set1_epi32(0x7F800000)", "_mm_set1_epi32(0x7F000000)"),
                           ("vdupq_n_u32(0x7F800000)", "vdupq_n_u32(0x7F000000)")],
    "float-byte-order": [("_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)",
                          "_mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)"),
                         ("vrev32q_u8(", "vrev16q_u8(")],
    "half-byte-order": [("_mm_setr_epi8(1, 0, -1, -1, 3, 2, -1, -1, 5, 4, -1, -1, 7, 6, -1, -1)",
                         "_mm_setr_epi8(0, 1, -1, -1, 2, 3, -1, -1, 4, 5, -1, -1, 6, 7, -1, -1)"),
                        ("{1, 0, 255, 255, 3, 2, 255, 255, 5, 4, 255, 255, 7, 6, 255, 255}",
                         "{0, 1, 255, 255, 2, 3, 255, 255, 4, 5, 255, 255, 6, 7, 255, 255}")],
    "missing-w-fill": [("rsx_v4_fill(0x3F800000u)", "rsx_v4_fill(0)")],
    "unorm8-scale": [("rsx_v4_u8(raw)), 255.0f)", "rsx_v4_u8(raw)), 256.0f)")],
    "snorm16-unsigned": [("rsx_v4_sext16(rsx_v4_be16(raw))), 32767.0f)", "rsx_v4_be16(raw)), 32767.0f)")],
    "sint16-unsigned": [("RSX_FETCH_LOOP_SIMD(rsx_v4_f32(rsx_v4_sext16(rsx_v4_be16(raw))))",
                         "RSX_FETCH_LOOP_SIMD(rsx_v4_f32(rsx_v4_be16(raw)))")],
    "twelve-byte-load-short": [("memcpy(&w, p + 8, 4);", "w = 0;"),
                               ("case 12: memcpy(tmp, p, 12); break;", "case 12: memcpy(tmp, p, 8); break;")],
    "byte-types-overread": [("type == RSX_VTX_TYPE_UNORM8 || type == RSX_VTX_TYPE_UINT8 ? 1u : 2u",
                             "type == RSX_VTX_TYPE_UNORM8 || type == RSX_VTX_TYPE_UINT8 ? 2u : 2u")],
}


def targets():
    """(label, extra compile flags, run prefix) for every form this host can run."""
    machine = platform.machine().lower()
    out = []
    if machine in ("x86_64", "amd64"):
        out.append(("x86-64 sse4.1", ["-msse4.1"], []))
    else:
        out.append((machine, [], []))
    if sys.platform == "darwin" and machine == "arm64":
        if subprocess.run(["arch", "-x86_64", "/usr/bin/true"], capture_output=True).returncode == 0:
            out.append(("x86-64 sse4.1 (rosetta)", ["-target", "x86_64-apple-macos12", "-msse4.1"], ["arch", "-x86_64"]))
    return out


def build_and_run(label, flags, prefix, source, tmp, expect_pass, quiet):
    binary = Path(tmp) / ("fixture-" + label.replace(" ", "-").replace("(", "").replace(")", ""))
    cc = os.environ.get("CC", "cc")
    command = [cc, "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", str(ROOT / "include"),
               "-I", str(ROOT / "libs/video"), *flags, "-o", str(binary), str(FIXTURE), str(source), *map(str, OTHER)]
    subprocess.run(command, cwd=ROOT, check=True, timeout=300)
    result = subprocess.run([*prefix, str(binary)], cwd=ROOT, capture_output=True, text=True, timeout=600)
    passed = result.returncode == 0
    if not quiet or passed != expect_pass:
        sys.stdout.write(result.stdout[-4000:])
        if result.returncode < 0:
            print(f"  (fixture killed by signal {-result.returncode})")
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutants", action="store_true", help="also build each mutant and require a failure")
    args = parser.parse_args()
    if os.name != "posix":
        print("SKIP: POSIX only")
        return 0
    failures = []
    with tempfile.TemporaryDirectory(prefix="vertex-fetch-simd-") as tmp:
        for label, flags, prefix in targets():
            print(f"== {label}")
            if not build_and_run(label, flags, prefix, SOURCE, tmp, True, False):
                failures.append(label)
                continue
            print(f"[PASS] {label}: vector and scalar fetch agree")
            if not args.mutants:
                continue
            original = SOURCE.read_text()
            for name, pairs in MUTANTS.items():
                mutated = original
                applied = 0
                for old, new in pairs:
                    count = mutated.count(old)
                    assert count <= 1, f"{name}: '{old}' occurs {count} times"
                    if count:
                        mutated = mutated.replace(old, new)
                        applied += 1
                assert applied, f"{name}: no replacement applied"
                mutant = Path(tmp) / f"mutant-{name}.c"
                mutant.write_text(mutated)
                caught = not build_and_run(label, flags, prefix, mutant, tmp, False, True)
                print(f"[{'PASS' if caught else 'FAIL'}] {label}: mutant {name} {'caught' if caught else 'NOT caught'}")
                if not caught:
                    failures.append(f"{label} mutant {name}")
    if failures:
        print("[vertex_fetch_simd] FAIL: " + ", ".join(failures))
        return 1
    print("[vertex_fetch_simd] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
