#!/usr/bin/env python3
"""rsx_dispatch_plain_register (libs/video/rsx_dispatch.c) against the real
rsx_dispatch_method, for every register index (libs/video/tests/
test_rsx_dispatch_plain.c). The draw engine makes a plain register's two
stores itself and skips the call, so an unsound answer would drop a method's
effect; a case added to the dispatcher without a line in the mirror fails
here. --mutants builds each listed mutation of the mirror and requires the
fixture to fail it.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
video = root / 'libs/video'

MUTANTS = {
    'begin-end-plain': ('    case M_CLEAR_BUFFERS: case M_VERTEX_BEGIN_END: case M_INLINE_ARRAY:\n',
                        '    case M_CLEAR_BUFFERS: case M_INLINE_ARRAY:\n'),
    'constant-window-plain': ('    if (m >= M_VP_UPLOAD_CONST && m < M_VP_UPLOAD_CONST + 32 * 4) return 0;\n', ''),
    'data2f-plain': ('    if (m >= 0x1880 && m < 0x1A00) return 0;\n', '    if (m >= 0x1900 && m < 0x1A00) return 0;\n'),
    'immediate-plain': ('    if (m == 0x1508 || m == 0x1C0C) return 0;           /* immediate vertex */\n', ''),
}


def build_and_run(tmp, label, dispatch_c):
    binary = Path(tmp) / ('fixture-' + label)
    cc = os.environ.get('CC', 'clang-cl' if os.name == 'nt' else 'cc')
    if os.name == 'nt':
        cmd = [cc, '/nologo', '/std:c17', '/W3', '/I' + str(root / 'include'), '/I' + str(video),
               str(video / 'tests/test_rsx_dispatch_plain.c'), str(dispatch_c), '/Fe:' + str(binary) + '.exe']
        binary = Path(str(binary) + '.exe')
    else:
        cmd = [cc, '-std=gnu17', '-O1', '-Wall', '-Wextra', '-I' + str(root / 'include'), '-I' + str(video),
               str(video / 'tests/test_rsx_dispatch_plain.c'), str(dispatch_c), '-o', str(binary)]
    subprocess.run(cmd, check=True, cwd=tmp)
    return subprocess.run([str(binary)], timeout=120, capture_output=True, text=True)


parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
failures = []
source = (video / 'rsx_dispatch.c').read_text()
with tempfile.TemporaryDirectory(prefix='rsx-dispatch-plain-') as tmp:
    result = build_and_run(tmp, 'plain', video / 'rsx_dispatch.c')
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-2000:])
        failures.append('fixture')
    if args.mutants and not failures:
        for name, (old, new) in MUTANTS.items():
            assert source.count(old) == 1, (name, old)
            mutant = Path(tmp) / f'mutant-{name}.c'
            # The mutated copy includes the same headers through -I.
            mutant.write_text(source.replace(old, new, 1))
            r = build_and_run(tmp, f'mutant-{name}', mutant)
            caught = r.returncode != 0
            print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
            if not caught:
                failures.append(f'mutant {name}')
if failures:
    raise SystemExit('test_rsx_dispatch_plain failed: ' + ', '.join(failures))
print('[rsx_dispatch_plain] PASS')
