#!/usr/bin/env python3
"""The FIFO walker's position as other threads see it (libs/video/cellGcmSys.c).

The pass keeps its position in a local and publishes it to s_fifo_getoff at
control flow, flips, semaphores/references, resyncs and the end of the pass.
libs/video/tests/test_gcm_fifo_getoff_threads.c runs a long pass while a
second thread samples the position and while the real ring-recycle wait runs
against it. --mutants builds each listed mutation of the walker and requires
the fixture to fail it.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
video = root / 'libs/video'
if os.name == 'nt':
    print('SKIP: the GCM walker fixture uses POSIX threads and linking')
    raise SystemExit(0)

MUTANTS = {
    # The JUMP between segments no longer publishes: another thread sees the
    # position only at the end of the pass.
    'jump-not-published': [('              get = tgt; GCM_PUBLISH_GET();\n', '              get = tgt;\n')],
    # The end of the pass no longer publishes (neither after the loop nor at
    # the exit): s_fifo_getoff stays at the last control-flow point.
    'end-not-published': [('    GCM_PUBLISH_GET();\n    gcm_observer_flush_zeros();\n', '    gcm_observer_flush_zeros();\n'),
                          ('    GCM_PUBLISH_GET();\n    g_gcm_fifo_drained_ea = gcm_io2ea(get);\n',
                           '    g_gcm_fifo_drained_ea = gcm_io2ea(get);\n')],
}


def build_and_run(tmp, label, include_first=None):
    binary = Path(tmp) / ('fixture-' + label)
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-lm']
    includes = ([f'-I{include_first}'] if include_first else []) + ['-I' + str(root / 'include'), '-I' + str(video)]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', *includes,
                    str(video / 'tests/test_gcm_fifo_getoff_threads.c'),
                    str(video / 'tests/gcm_deferred_fixture_stubs.c'), str(video / 'rsx_fifo_busy.c'),
                    '-pthread', *linker, '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], timeout=120, capture_output=True, text=True)


parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
walker = (video / 'cellGcmSys.c').read_text()
failures = []
with tempfile.TemporaryDirectory(prefix='gcm-getoff-threads-') as tmp:
    result = build_and_run(tmp, 'walker')
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        failures.append('fixture')
    if args.mutants and not failures:
        for name, pairs in MUTANTS.items():
            mutated = walker
            for old, new in pairs:
                assert mutated.count(old) == 1, (name, old)
                mutated = mutated.replace(old, new, 1)
            mdir = Path(tmp) / f'mutant-{name}'
            mdir.mkdir()
            (mdir / 'cellGcmSys.c').write_text(mutated)
            r = build_and_run(tmp, f'mutant-{name}', mdir)
            caught = r.returncode != 0
            print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
            if not caught:
                failures.append(f'mutant {name}')
if failures:
    raise SystemExit('test_gcm_fifo_getoff_threads failed: ' + ', '.join(failures))
print('[gcm_fifo_getoff_threads] PASS')
