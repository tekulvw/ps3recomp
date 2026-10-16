from pathlib import Path
import subprocess,os
import tempfile
root=Path(__file__).resolve().parents[1]; temporary=tempfile.TemporaryDirectory(prefix='metal-empty-');work=root;binary=Path(temporary.name)/'fixture'
cmd=['clang','-O2','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I'+str(root/'include'),'-I'+str(root/'libs/video'),str(work/'libs/video/tests/test_metal_empty_flush.m'),str(root/'libs/video/rsx_texture_layout.c'),str(root/'runtime/diagnostics/critical_trace.c'),str(root/'libs/input/pad_macos.m'),'-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)]
if os.getenv('UBSAN')=='1':cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
subprocess.run(cmd,check=True,timeout=60)
for mode in ('0','1'):
 for name in ('empty','completed','pending','mixed','error','record','stage','dropped','prior_error','inactive','missing_device','missing_queue','tracking_failure','checked','retired'):
  subprocess.run([str(binary),mode,name],check=True,timeout=15)
