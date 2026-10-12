"""Real binding snapshots: disabled, bounded, layout bytes and stale-cache evidence."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
sources=["tests/test_texture_binding_capture.c", "rsx_draw_engine.c", "rsx_dispatch.c",
         "rsx_vertex_compact.c", "rsx_texture_layout.c", "rsx_vp_decompiler.c", "rsx_fp_decompiler.c"]
with tempfile.TemporaryDirectory(prefix="rsx-binding-capture-") as directory:
    directory=Path(directory); binary=directory/"test"
    subprocess.run([os.environ.get("CC","cc"),"-std=gnu17","-Iinclude","-Ilibs/video",
                    *["libs/video/"+s for s in sources],"-o",str(binary)],cwd=root,check=True)
    env={k:v for k,v in os.environ.items() if not k.startswith("PS3RECOMP_RSX_BINDING_CAPTURE")}
    def run(mode, output):
        output.mkdir()
        result=subprocess.run([str(binary),mode],env=env,capture_output=True,text=True,check=True,timeout=20)
        return list(output.glob("*.meta")),result.stderr
    output=directory/"disabled"; rows,_=run("normal",output); assert not rows
    env.update(PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS="00020000",
               PS3RECOMP_RSX_BINDING_CAPTURE_LOCATION="1",
               PS3RECOMP_RSX_BINDING_CAPTURE_START_FRAME="1")
    for mode in ("normal","compressed"):
        output=directory/mode; env["PS3RECOMP_RSX_BINDING_CAPTURE_DIR"]=str(output)
        rows,stderr=run(mode,output); assert len(rows)==1,(rows,stderr)
        meta=rows[0].read_text(); prefix=str(rows[0])[:-5]
        assert "frame=1 " in meta and "source=upload " in meta and "cache_matches_current=1 " in meta and "gpu_saved=1 " in meta,meta
        assert Path(prefix+".metal.rgba").read_bytes()==b"test"
        assert Path(prefix+".vertices.bin").stat().st_size>0
        assert Path(prefix+".vp-constants.bin").stat().st_size>0
        raw=Path(prefix+".source.bin").read_bytes(); decoded=Path(prefix+".decoded-mip0.bin").read_bytes()
        if mode=="compressed": assert raw==decoded==b"\x5A"*32
        else:
            assert len(raw)==192 and len(decoded)==128
            assert decoded[:4]==bytes((1,2,83,255)) and decoded[32:36]==bytes((1,3,83,255))
    # Arm only after the first cache population: trigger checks happen once per
    # frame, so fixture advances after creating its marker.
    output=directory/"trigger"; env["PS3RECOMP_RSX_BINDING_CAPTURE_DIR"]=str(output)
    env["PS3RECOMP_RSX_BINDING_CAPTURE_TRIGGER"]=str(directory/"arm")
    rows,_=run("trigger",output); assert len(rows)==1 and "frame=2 " in rows[0].read_text()
    del env["PS3RECOMP_RSX_BINDING_CAPTURE_TRIGGER"]
    env["PS3RECOMP_RSX_BINDING_CAPTURE_START_DRAW"]="3"
    output=directory/"stale"; env["PS3RECOMP_RSX_BINDING_CAPTURE_DIR"]=str(output)
    rows,_=run("stale",output); assert len(rows)==1 and "cache_matches_current=0 " in rows[0].read_text()
    env["PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS"]="1,2,3,4,5,6,7,8,9"
    output=directory/"oversized-list"; env["PS3RECOMP_RSX_BINDING_CAPTURE_DIR"]=str(output)
    rows,_=run("normal",output); assert not rows and not list(output.iterdir())
    env["PS3RECOMP_RSX_BINDING_CAPTURE_OFFSETS"]="00020000x"
    output=directory/"malformed-list"; env["PS3RECOMP_RSX_BINDING_CAPTURE_DIR"]=str(output)
    rows,_=run("normal",output); assert not rows and not list(output.iterdir())
print("PASS: bounded binding capture, immutable decode, trigger and stale-cache evidence")
