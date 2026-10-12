"""Capture real draw-engine descriptors without changing rendering or flooding logs."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if os.name == "nt":
    print("SKIP: descriptor fixture requires a POSIX compiler")
    sys.exit(0)

root = Path(__file__).resolve().parents[1]
sources = ["tests/test_texture_descriptors.c", "rsx_draw_engine.c", "rsx_dispatch.c",
           "rsx_vertex_compact.c", "rsx_texture_layout.c", "rsx_vp_decompiler.c",
           "rsx_fp_decompiler.c"]
with tempfile.TemporaryDirectory(prefix="rsx-descriptors-") as directory:
    binary = Path(directory) / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-Iinclude", "-Ilibs/video",
                    *["libs/video/" + s for s in sources], "-o", str(binary)],
                   cwd=root, check=True)
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith("PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS"):
            del env[key]
    def capture(mode):
        result = subprocess.run([str(binary), mode], env=env, capture_output=True,
                                text=True, check=True, timeout=20)
        return [line for line in result.stderr.splitlines()
                if line.startswith("[rsx texture descriptor]")]
    assert capture("upload") == []
    env.update(PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS="1",
               PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_START_FRAME="1",
               PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_LIMIT="2")
    rows = capture("upload")
    assert len(rows) == 2, rows
    assert all("frame=1 " in row and "source=upload " in row for row in rows), rows
    assert all("fmt=A5 " in row and "size=8x4 " in row and "pitch=48 " in row
               and "advertised_mips=3 " in row and "effective_mips=3 " in row
               and "compressed=0 " in row and "mips=0:0:48," in row
               for row in rows), rows
    # Accept either the original packed lower levels or the separate pitch
    # correction, so this evidence fixture remains usable after integration.
    assert all("mips=0:0:48,1:192:16,2:224:8" in row
               or "mips=0:0:48,1:192:48,2:288:48" in row for row in rows), rows
    assert "filter=02060000 " in rows[0] and "filter=02050000 " in rows[1], rows
    rows = capture("surface")
    assert len(rows) == 2 and all("source=surface " in row and "surface_size=256x256 " in row
                                 for row in rows), rows
    rows = capture("compressed")
    assert len(rows) == 2 and all("fmt=A6 " in row and "compressed=1 " in row
                                 and "pitch=48 " in row and "mips=0:0:16," in row
                                 for row in rows), rows
    env["PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_TRIGGER"] = str(Path(directory) / "arm")
    assert capture("upload") == []
    Path(env["PS3RECOMP_RSX_TEXTURE_DIAGNOSTICS_TRIGGER"]).touch()
    assert len(capture("upload")) == 2
print("PASS: disabled, delayed, bounded unique descriptors, mip pitches, surface provenance and trigger")
