"""Actual producer caller wildcard/exact/invalid selection and finite full-frame gate."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="producer-selector-") as directory:
    directory = Path(directory)
    binary = directory / "fixture"
    sources = ["libs/video/tests/test_producer_capture_selector.c", "libs/video/rsx_dispatch.c",
               "libs/video/rsx_vertex_compact.c", "libs/video/rsx_texture_layout.c", "libs/video/rsx_vp_decompiler.c",
               "libs/video/rsx_fp_decompiler.c", "runtime/diagnostics/render_ablation.c", "runtime/diagnostics/critical_trace.c"]
    subprocess.run(["cc", "-std=gnu17", "-O2", "-UNDEBUG", "-fsanitize=undefined", "-Iinclude", "-Ilibs/video",
                    *sources, "-o", str(binary)], cwd=root, check=True, timeout=60)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("RSX_ENGINE_", "PS3RECOMP_RSX_BINDING_CAPTURE"))}
    for mode in ("wildcard", "exact", "wrong-control", "unset", "empty", "malformed", "multiple", "wrong-target", "oversized-limit"):
        output = directory / mode
        output.mkdir()
        result = subprocess.run([str(binary), str(output), mode], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, (mode, result.stdout, result.stderr)
        records = list(output.glob("*.meta"))
        assert len(records) == (2 if mode in ("wildcard", "exact") else 0), (mode, records, result.stderr)
        for record in records:
            text = record.read_text()
            assert "frame=1 " in text and "status=complete payload_saved=1" in text and "cpu_only=1" in text, text
            prefix = str(record)[:-5]
            assert Path(prefix + ".render-state.bin").stat().st_size > 0
            assert Path(prefix + ".vp-program.bin").stat().st_size == 16
            assert Path(prefix + ".fp-program.bin").stat().st_size == 16
    print("PASS: actual producer caller explicit wildcard/exact, malformed/unset/target/control refusal, full-frame and finite2-record gates; draws unchanged")
