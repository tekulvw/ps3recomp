#!/usr/bin/env python3
"""Real Metal draw/target diagnostic bounds and RGB readback."""
import pathlib,platform,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
def main():
 if platform.system()!="Darwin":print("SKIP: Metal diagnostics require macOS");return
 with tempfile.TemporaryDirectory(prefix="ps3-metal-draw-") as tmp:
  binary=pathlib.Path(tmp)/"fixture"
  subprocess.run(["clang","-fobjc-arc","-I","include","-I","libs/video","libs/video/tests/test_metal_draw_diagnostics.m","libs/video/rsx_texture_layout.c","-Wl,-dead_strip","-framework","Metal","-framework","Cocoa","-framework","QuartzCore","-o",str(binary)],cwd=ROOT,check=True,timeout=120)
  for mode in ("disabled","enabled"):
   p=subprocess.run([str(binary),mode],check=True,capture_output=True,text=True,timeout=60)
   draws=[x for x in p.stderr.splitlines() if "[rsx draw diagnostic]" in x]
   targets=[x for x in p.stderr.splitlines() if "[rsx target diagnostic]" in x]
   if mode=="disabled":assert not draws and not targets,p.stderr;continue
   assert len(draws)==32,(len(draws),p.stderr)
   assert "rt=1" in draws[0] and "vertex0=1,2,3,4" in draws[0],draws[0]
   assert len(targets)==10,(len(targets),p.stderr)
   assert sum("rgb_nonzero=1" in x and "rgb_max=31" in x for x in targets)==8,targets
   assert any("surface=2" in x and "rgb_nonzero=2" in x and "rgb_max=64" in x for x in targets),targets
   assert any("surface=3" in x and "rgb_nonzero=0" in x for x in targets),targets
   assert all("gpu_status=4" in x for x in targets),targets
  print("Metal draw diagnostics PASS: opt-in, 32 draw budget, 8 submission budget, colored/alpha-only RGB readback")
if __name__=="__main__":main()
