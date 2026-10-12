#!/usr/bin/env python3
import pathlib,platform,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
if platform.system()!='Darwin':print('SKIP: Metal requires macOS');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='metal-checked-') as tmp:
 binary=pathlib.Path(tmp)/'test'
 subprocess.run(['clang','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I','include','-I','libs/video','libs/video/tests/test_metal_checked_completion.m','libs/video/rsx_texture_layout.c','libs/input/pad_macos.m','-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
 for mode in ('prior_error','empty','clear','independent_depth_stencil','independent_clears','missing_queue','invalid_record','dropped','nil_cb','nil_stage','nil_encoder','gpu_error'):subprocess.run([str(binary),mode],check=True,timeout=20)
