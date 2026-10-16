#!/usr/bin/env python3
"""Bounded opt-in presentation evidence through real Metal on macOS."""
import pathlib, platform, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
def main():
    if platform.system() != "Darwin":
        print("SKIP: Metal presentation diagnostics require macOS"); return
    with tempfile.TemporaryDirectory(prefix="ps3recomp-metal-diag-") as tmp:
        binary=pathlib.Path(tmp)/"fixture"
        subprocess.run(["clang","-fobjc-arc", "-DPS3RECOMP_DISABLE_NATIVE_OSK","-I","include","-I","libs/video",
            "libs/video/tests/test_metal_present_diagnostics.m","libs/video/rsx_texture_layout.c",'runtime/diagnostics/critical_trace.c', "libs/input/pad_macos.m",
            "-Wl,-dead_strip","-framework","Metal","-framework","Cocoa","-framework","QuartzCore",
            "-o",str(binary)],cwd=ROOT,check=True,timeout=120)
        results={}
        for mode in ("disabled","black","color","blit","drawable","missing","budget","flushed","drawable_periodic"):
            p=subprocess.run([str(binary),mode,str(pathlib.Path(tmp)/(mode+".ppm"))],check=True,
                             capture_output=True,text=True,timeout=60)
            results[mode]=[x for x in p.stderr.splitlines() if "[rsx present diagnostic]" in x]
        assert not results["disabled"], results
        for mode,reason in (("black","submitted readback=black"),("color","submitted readback=nonblack"),("blit","blit_skipped"),
                            ("drawable","no_drawable"),("missing","no_surface")):
            assert len(results[mode])==1, (mode,results[mode])
            assert "reason="+reason in results[mode][0], results[mode]
        assert "rgb_nonzero=0" in results["black"][0], results["black"]
        assert "rgb_nonzero=1" in results["color"][0] and "rgb_max=31" in results["color"][0]
        assert "gpu_status=4" in results["black"][0], results["black"]
        assert len(results["budget"])==8, results["budget"]
        assert sum("queued_draws=1" in x for x in results["budget"])==6,results["budget"]
        assert len(results["drawable_periodic"])==2, results["drawable_periodic"]
        assert "reason=no_drawable readback=black" in results["drawable_periodic"][-1],results["drawable_periodic"]
        assert len(results["flushed"])==8, results["flushed"]
        assert any("attempt=501 " in x for x in results["flushed"]), results["flushed"]
        assert all("queued_draws=0" in x for x in results["flushed"]), results["flushed"]
        print("Metal presentation diagnostics PASS: disabled, black, nonblack, skipped blit, no drawable, no surface, 2+6 bounded sampling")
if __name__ == "__main__": main()
