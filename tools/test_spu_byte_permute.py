#!/usr/bin/env python3
"""Check SPU byte permutes against an architectural byte oracle and interpreter.

Optional --bench --compare-root SDK benchmarks only these helper wrappers against
an unchanged SDK; compiler auto-vectorization is enabled in both trees. Numbers
include wrapper/loop overhead and do not measure game or displayed FPS.
"""
import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compare-root", type=Path)
parser.add_argument("--bench", action="store_true")
parser.add_argument("--output-dir", type=Path)
args = parser.parse_args()
if os.name == "nt":
    print("SKIP: interpreter fixture requires a POSIX compiler and compatibility layer")
    raise SystemExit(0)
if args.bench and not args.compare_root:
    parser.error("--bench requires --compare-root to an unchanged SDK")
root = Path(__file__).resolve().parents[1]
source = root / "runtime/spu/tests/test_spu_byte_permute.c"
cc = os.environ.get("CC", "cc")
commands = []
results = {}
codegen = {}
benchmark = {}
common = ["-std=gnu17", "-UNDEBUG", "-fno-strict-aliasing", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-parameter"]

def run(argv, label, output, env=None):
    process = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=60, env=env)
    commands.append({"label": label, "argv": [str(x) for x in argv], "exit_code": process.returncode,
                     "output": process.stdout})
    (output / (label + ".log")).write_text(process.stdout)
    if process.returncode:
        raise RuntimeError(f"{label} failed ({process.returncode}):\n{process.stdout}")
    return process.stdout.strip()

def build(tree, label, flags, output, interpreter=True):
    binary = output / label
    command = [cc, *common, *flags, "-I" + str(tree / "runtime/spu"), str(source)]
    if interpreter:
        command += [str(tree / "runtime/spu/spu_interp.c"), str(tree / "runtime/platform/win32_compat.c"), "-pthread"]
    else:
        command += ["-DBYTE_PERMUTE_NO_INTERPRETER"]
    run(command + ["-lm", "-o", str(binary)], "build-" + label, output)
    return binary

holder = tempfile.TemporaryDirectory(prefix="spu-byte-permute-") if args.output_dir is None else None
output = args.output_dir or Path(holder.name)
output.mkdir(parents=True, exist_ok=True)
try:
    reference = None
    for optimization in ("O2", "O3"):
        if args.compare_root:
            baseline_label = "baseline-" + optimization
            binary = build(args.compare_root, baseline_label, ["-" + optimization], output)
            value = run([str(binary)], "run-" + baseline_label, output)
            if reference is None:
                reference = value
            assert value == reference, "Unchanged baseline optimization levels disagree"
            results[baseline_label] = value
            print(baseline_label + ": " + value.replace("\n", "; "), flush=True)
        for variant, flags in (("native", []), ("scalar", ["-DSPU_SCALAR_HELPERS"]),
                               ("ubsan", ["-fsanitize=undefined", "-fno-sanitize-recover=all"]),
                               ("scalar-ubsan", ["-DSPU_SCALAR_HELPERS", "-fsanitize=undefined", "-fno-sanitize-recover=all"])):
            label = optimization + "-" + variant
            binary = build(root, label, ["-" + optimization, *flags], output)
            value = run([str(binary)], "run-" + label, output)
            if reference is None:
                reference = value
            assert value == reference, label + " differs from oracle-checked baseline"
            results[label] = value
            print(label + ": " + value.replace("\n", "; "), flush=True)
        # Reusing the same LS PC for changing words also exercises the enabled
        # cache's live-word invalidation; check uncached dispatch independently.
        label = optimization + "-uncached"
        value = run([str(output / (optimization + "-native"))], "run-" + label, output,
                    env={**os.environ, "SPU_INTERP_DECODE_CACHE": "0"})
        assert value == reference, "Cached and uncached interpreter results differ"
        results[label] = value

    if platform.machine().lower() in ("arm64", "aarch64"):
        trees = [("candidate", root)] + ([("baseline", args.compare_root)] if args.compare_root else [])
        for optimization in ("O2", "O3"):
            for tree_name, tree in trees:
                label = "codegen-" + tree_name + "-" + optimization
                assembly = output / (label + ".s")
                run([cc, *common, "-" + optimization, "-DBYTE_PERMUTE_NO_INTERPRETER", "-I" + str(tree / "runtime/spu"),
                     "-S", str(source), "-o", str(assembly)], label, output)
                text = assembly.read_text()
                wrappers = {}
                for name in ("rotqbyi", "rotqby", "shlqbyi"):
                    body = re.split(r"_?byte_" + name + r"_wrapper:\s*", text, maxsplit=1)[1].split(".cfi_endproc", 1)[0]
                    instructions = [op.split(".", 1)[0] for op in re.findall(r"^\s+([a-z][a-z0-9.]*)\s+", body, re.M)]
                    wrappers[name] = {"instruction_count": len(instructions), "tbl_count": instructions.count("tbl"),
                                      "body_sha256": hashlib.sha256(body.encode()).hexdigest()}
                    if tree_name == "candidate":
                        assert instructions.count("tbl") == 1, name + " should emit one ARM64 table lookup"
                codegen[label] = wrappers
        # Compile the actual fallback branch for x86, without executing Rosetta.
        if platform.system() == "Darwin":
            run([cc, *common, "-O2", "-arch", "x86_64", "-DBYTE_PERMUTE_NO_INTERPRETER", "-I" + str(root / "runtime/spu"),
                 "-c", str(source), "-o", str(output / "fallback-x86_64.o")], "compile-x86_64-fallback", output)
            print("x86 fallback compiles", flush=True)
        print("ARM64 O2/O3: each candidate wrapper emits one TBL", flush=True)

    if args.bench:
        # Build both with the compiler's normal vectorizers enabled. Interleave
        # tree order between O2 and O3; five reported rounds, two million calls
        # per op/round. No game, GPU, scheduler, or display path is involved.
        for optimization in ("O2", "O3"):
            trees = [("baseline", args.compare_root), ("candidate", root)]
            if optimization == "O3":
                trees.reverse()
            for tree_name, tree in trees:
                label = "bench-" + tree_name + "-" + optimization
                binary = build(tree, label, ["-" + optimization], output, interpreter=False)
                text = run([str(binary), "--bench"], "run-" + label, output)
                rows = []
                for line in text.splitlines():
                    match = re.fullmatch(r"BYTE_PERMUTE BENCH op=(\d+) round=(\d+) n=(\d+) ns_per_op=([0-9.]+) checksum=([0-9a-f]+)", line)
                    assert match, line
                    op, round_number, count, ns, checksum = match.groups()
                    rows.append({"op": int(op), "round": int(round_number), "n": int(count), "ns_per_op": float(ns), "checksum": checksum})
                assert len(rows) == 15
                benchmark[label] = rows
        for optimization in ("O2", "O3"):
            for op, name in enumerate(("rotqbyi", "rotqby", "shlqbyi")):
                before = [x for x in benchmark["bench-baseline-" + optimization] if x["op"] == op]
                after = [x for x in benchmark["bench-candidate-" + optimization] if x["op"] == op]
                assert [x["checksum"] for x in before] == [x["checksum"] for x in after], "Benchmark outputs differ"
                old = statistics.median(x["ns_per_op"] for x in before)
                new = statistics.median(x["ns_per_op"] for x in after)
                print(f"{optimization} {name}: baseline={old:.3f}ns candidate={new:.3f}ns ratio={old/new:.2f}x (helper wrapper only)", flush=True)
finally:
    receipt = {"compiler": cc, "commands": commands, "results": results, "codegen": codegen,
               "benchmark": benchmark, "compiler_auto_vectorization_enabled": True,
               "native_application_launches": False, "fps_measured": False}
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2))
    if holder:
        holder.cleanup()
