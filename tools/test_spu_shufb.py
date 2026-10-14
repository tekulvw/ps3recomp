#!/usr/bin/env python3
"""Run SHUFB semantics in native, scalar, and sanitized builds."""
import os
import platform
import re
import subprocess
import tempfile
from pathlib import Path

if os.name == "nt":
    print("SKIP: SHUFB fixture currently requires a POSIX compiler and compatibility layer")
    raise SystemExit(0)

root = Path(__file__).resolve().parents[1]
source = root / "runtime/spu/tests/test_spu_shufb.c"
cc = os.environ.get("CC", "cc")
with tempfile.TemporaryDirectory(prefix="spu-shufb-") as tmp:
    outputs = []
    for label, flags in [("native", []), ("scalar", ["-DSPU_SCALAR_HELPERS"]),
                          ("sanitized", ["-fsanitize=undefined", "-fno-sanitize-recover=all"])]:
        binary = Path(tmp) / label
        subprocess.run([cc, "-std=gnu17", "-O2", "-UNDEBUG", *flags,
                        str(source), "-lm", "-o", str(binary)], check=True)
        result = subprocess.check_output([str(binary)], text=True, timeout=60).strip()
        print(label + ": " + result)
        outputs.append(result)
    assert len(set(outputs)) == 1, "Native/scalar/sanitized SHUFB results differ"
    binary = Path(tmp) / "interpreter"
    subprocess.run([cc, "-std=gnu17", "-O2", "-UNDEBUG", "-DSHUFB_INTERPRETER",
                    str(source), str(root / "runtime/spu/spu_interp.c"),
                    str(root / "runtime/platform/win32_compat.c"), "-pthread", "-lm",
                    "-o", str(binary)], check=True)
    result = subprocess.check_output([str(binary)], text=True, timeout=60).strip()
    assert result.splitlines()[0] == outputs[0], "Interpreter fixture helper digest differs"
    print(result)
    if platform.machine().lower() in ("arm64", "aarch64"):
        assembly = Path(tmp) / "native.s"
        subprocess.run([cc, "-std=gnu17", "-O2", "-S", str(source), "-o", str(assembly)], check=True)
        text = assembly.read_text()
        wrapper = text.split("shufb_hot_wrapper:", 1)[1].split(".cfi_endproc", 1)[0]
        assert re.search(r"\btbl\b", wrapper), "ARM64 SHUFB wrapper lacks table-lookup fast path"
        print("ARM64 generated hot wrapper: TBL fast path present")
