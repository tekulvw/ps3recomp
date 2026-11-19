#!/usr/bin/env python3
"""The walker's plain-method path (PS3RECOMP_GCM_FAST_WALK=1) and its packet runs
(PS3RECOMP_GCM_FAST_WALK_RUNS, rsx_draw_engine_methods_at) against the general per-word path
(libs/video/tests/test_gcm_plain_walk.c, which compiles cellGcmSys.c in). --mutants builds each listed
mutation of the walker and requires the fixture to fail it."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
video = root / 'libs/video'
if os.name == 'nt':
    print('SKIP: the GCM walker fixture uses POSIX linking')
    raise SystemExit(0)

MUTANTS = {
    # the last word of an incrementing packet is not tested for a special
    # method: a packet ending on SET_REFERENCE goes over as a run.
    'range-last-excluded': [
        ('if (specials[k] >= method && specials[k] <= last) return 1;',
         'if (specials[k] >= method && specials[k] < last) return 1;'),
    ],
    # a non-incrementing packet handed over as incrementing.
    'ni-as-incrementing': [
        ('rsx_draw_engine_methods_at(subch, type == 0 ? method + i * 4 : method, type == 0,',
         'rsx_draw_engine_methods_at(subch, type == 0 ? method + i * 4 : method, 1,'),
    ],
    # a run not cut at the IO page boundary: the words past it are read from
    # the wrong page.
    'run-past-page': [
        ('u32 run = (0x100000u - (dio & 0xFFFFFu)) >> 2;', 'u32 run = count;'),
    ],
    # the data address handed over with a run is off by one word.
    'run-ea-off': [
        ('vm_ptr32(dea), run, dea);', 'vm_ptr32(dea), run, dea + 4u);'),
    ],
}


def build_and_run(tmp, label, include_first=None):
    binary = Path(tmp) / ('fixture-' + label)
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    includes = ([f'-I{include_first}'] if include_first else []) + ['-I' + str(root / 'include'), '-I' + str(video)]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', *includes,
                    str(video / 'tests/test_gcm_plain_walk.c'),
                    str(video / 'tests/gcm_deferred_fixture_stubs.c'), str(video / 'rsx_fifo_busy.c'), *linker,
                    '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)


parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true', help='also build each mutant and require a failure')
args = parser.parse_args()
failures = []
walker = (video / 'cellGcmSys.c').read_text()
with tempfile.TemporaryDirectory(prefix='gcm-plain-walk-') as tmp:
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
                mutated = mutated.replace(old, new)
            mdir = Path(tmp) / f'mutant-{name}'
            mdir.mkdir()
            (mdir / 'cellGcmSys.c').write_text(mutated)
            r = build_and_run(tmp, f'mutant-{name}', mdir)
            caught = r.returncode != 0
            print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
            if not caught:
                failures.append(f'mutant {name}')
if failures:
    raise SystemExit('test_gcm_plain_walk failed: ' + ', '.join(failures))
print('[gcm_plain_walk] PASS')
