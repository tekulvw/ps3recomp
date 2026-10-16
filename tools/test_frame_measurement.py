#!/usr/bin/env python3
"""Portable receipt/math and macOS protocol tests; no window or GPU work."""
import json, pathlib, platform, subprocess, tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='frame-measurement-fixtures-') as tmp:
    for name in ('test_frame_measurement','test_gcm_flip_receipt'):
        binary=pathlib.Path(tmp)/name
        subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I','libs/video','libs/video/tests/'+name+'.c','-pthread','-lm','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
        subprocess.run([str(binary)],check=True,timeout=20)
    if platform.system()=='Darwin':
        binary=pathlib.Path(tmp)/'callbacks'
        subprocess.run(['clang','-fobjc-arc','-DPS3RECOMP_DISABLE_NATIVE_OSK','-I','include','-I','libs/video','libs/video/tests/test_metal_frame_measurement.m','libs/video/rsx_texture_layout.c','runtime/diagnostics/critical_trace.c','libs/input/pad_macos.m','-Wl,-dead_strip','-framework','Metal','-framework','Cocoa','-framework','QuartzCore','-o',str(binary)],cwd=ROOT,check=True,timeout=60)
        completed=subprocess.run([str(binary)],check=True,timeout=20,capture_output=True,text=True)
        print(completed.stdout,end='');print(completed.stderr,end='')
        prefix='[RSX FPS window] '
        events=[json.loads(line[len(prefix):]) for line in completed.stderr.splitlines() if line.startswith(prefix)]
        result=next(event for event in events if event.get('phase')=='complete')
        assert result['qualified'] and result['unique_guest_displayed']==1 and result['committed']==1
        assert result['seconds']==1 and result['verified_guest_fps']==1 and not result['meets_30_displayed_guest_fps']
    else:
        print('SKIP: Objective-C callback fixture requires macOS')
