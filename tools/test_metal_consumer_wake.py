#!/usr/bin/env python3
"""Real GPU command completion wakes consumer only when launch option enabled."""
import pathlib,platform,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
if platform.system()!='Darwin':print('SKIP: Metal requires macOS');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='metal-consumer-wake-') as tmp:
 binary=pathlib.Path(tmp)/'test'
 subprocess.run(['clang','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I','include','-I','libs/video','libs/video/tests/test_metal_consumer_wake.m','libs/video/rsx_texture_layout.c','runtime/diagnostics/critical_trace.c','libs/input/pad_macos.m','-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
 for enabled in ('0','1'):subprocess.run([str(binary),enabled],check=True,timeout=10)
