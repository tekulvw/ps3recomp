from pathlib import Path
import subprocess
import tempfile
r=Path(__file__).resolve().parents[1];temporary=tempfile.TemporaryDirectory(prefix='metal-checked-empty-gpu-');w=r;b=Path(temporary.name)/'fixture'
subprocess.run(['clang','-O2','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I'+str(r/'include'),'-I'+str(r/'libs/video'),str(w/'libs/video/tests/test_metal_checked_empty_flush_gpu.m'),str(r/'libs/video/rsx_texture_layout.c'),str(r/'runtime/diagnostics/critical_trace.c'),str(r/'runtime/diagnostics/render_ablation.c'),str(r/'libs/input/pad_macos.m'),'-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(b)],check=True,timeout=60)
subprocess.run([str(b)],check=True,timeout=20)
