#!/usr/bin/env python3
"""Frame-rate contract, independent Metal timestamps, GPU readback and AppKit timer."""
import pathlib, platform, subprocess, tempfile, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
if platform.system()!='Darwin':
    print('SKIP: Metal requires macOS'); raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='metal-fps-') as tmp:
    binary=pathlib.Path(tmp)/'test'
    subprocess.run(['clang','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I','include','-I','libs/video','libs/video/tests/test_metal_frame_rate.m','libs/video/rsx_texture_layout.c','libs/input/pad_macos.m','-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
    modes=('stats','callback_contract') if '--unit-only' in sys.argv else ('stats','callback_contract','window','drawable','disabled')
    for mode in modes:
        subprocess.run([str(binary),mode],check=True,timeout=20)
