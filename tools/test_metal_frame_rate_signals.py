#!/usr/bin/env python3
"""Frame signal math and callback fixtures; no windows or GPU submissions."""
import os, pathlib, platform, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='metal-fps-signals-') as tmp:
    binary = pathlib.Path(tmp) / 'math'
    subprocess.run(['clang', '-std=c11', '-I', 'libs/video', 'libs/video/tests/test_metal_frame_rate_signals.c', *([] if os.name == 'nt' else ['-lm']), '-o', str(binary)], cwd=ROOT, check=True, timeout=60)
    subprocess.run([str(binary)], check=True, timeout=20)
    if platform.system() != 'Darwin':
        print('SKIP: Objective-C callback fixture requires macOS')
    else:
        common = ['clang', '-fobjc-arc', '-DPS3RECOMP_DISABLE_NATIVE_OSK', '-I', 'include', '-I', 'libs/video']
        link = ['libs/video/rsx_texture_layout.c','runtime/diagnostics/critical_trace.c','libs/video/rsx_fifo_busy.c', 'libs/input/pad_macos.m', '-Wl,-dead_strip', '-framework', 'Metal', '-framework', 'Cocoa', '-framework', 'QuartzCore']
        # Each fixture links what its own runner does (test_metal_checked_completion.py,
        # test_metal_frame_rate.py); the backend reaches both extras.
        extra = {'test_metal_frame_rate_signals': ['runtime/diagnostics/render_ablation.c'],
                 'test_metal_frame_rate': ['libs/video/rsx_frontend.c', '-Wl,-U,_cellGcm_rsx_consumer_begin',
                                           '-Wl,-U,_cellGcm_rsx_consumer_end']}
        for name, args in [('test_metal_frame_rate_signals', []), ('test_metal_frame_rate', ['stats'])]:
            binary = pathlib.Path(tmp) / name
            subprocess.run(common + ['libs/video/tests/' + name + '.m'] + link + extra[name] + ['-o', str(binary)], cwd=ROOT, check=True, timeout=60)
            subprocess.run([str(binary)] + args, check=True, timeout=20)
