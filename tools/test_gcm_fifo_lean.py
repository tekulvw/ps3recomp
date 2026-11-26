#!/usr/bin/env python3
"""The FIFO walker's lean header path (libs/video/cellGcmSys.c).

1. Every environment variable read inside gcm_rsx_process_fifo_pass or the
   FIFO observer must be either listed in libs/video/gcm_fifo_diag_env.h (it
   arms a per-word diagnostic, which the walker skips behind one word while
   none is set) or named below as read per pass, per branch or at a report,
   never per command word. A getenv() added to the pass fails this until it is
   classified, so a per-word diagnostic cannot silently stop working.
2. libs/video/tests/test_gcm_fifo_lean.c walks one synthetic FIFO unarmed,
   armed, under checked REF with a stand-down, and against an unmapped page;
   the calls must be the same and checked REF must still reject the packet.
3. --mutants builds each listed mutation of the walker and requires the
   fixture to fail it.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
video = root / 'libs/video'
if os.name == 'nt':
    print('SKIP: the GCM walker fixture uses POSIX linking')
    raise SystemExit(0)

# Read in the pass, but not on the per-word path: once per pass, in a branch
# the stream rarely takes (jump, call, semaphore, flip, unknown word), or at
# a recovery report.
PER_PASS = {
    # once per pass
    'GCM_RATE', 'GCM_CTXDBG', 'GCM_DRAINDBG_EVERY', 'GCM_SCANREF', 'GCM_FIFO_SNAP', 'RSX_TRACE',
    # in a branch the stream rarely takes: flip, jump, semaphore, unknown word, 2D subchannel
    # (GCM_SEMA_ACQUIRE[_MS] moved out of the pass, into gcm_acquire_blocking)
    'GCM_FLIPCOUNT', 'GCM_RECDBG', 'RTT_DUMP', 'GCM_SUBCH7',
    # at the circling recovery report
    'GCM_FIFO_CIRCLE_PASSES',
    # the general (non-plain) per-word path's own diagnostics; any of them
    # already turns PS3RECOMP_GCM_FAST_WALK off (gcm_plain_walk / s_walk_diag)
    'GCM_SUBCH_GATE', 'GCM_OBJDBG', 'RSX_LIVE_FEED_DBG', 'GCM_VAL_WATCH', 'GCM_METHOD_HIST',
}


def body(text, name):
    m = re.search(r'^static void ' + re.escape(name) + r'\(void\)\s*\{', text, re.M)
    assert m, name
    i = text.index('{', m.start())
    depth, k, instr = 0, i, None
    while k < len(text):
        c = text[k]
        if instr:
            if c == '\\':
                k += 2
                continue
            if c == instr:
                instr = None
        elif text.startswith('//', k):
            k = text.index('\n', k)
            continue
        elif text.startswith('/*', k):
            k = text.index('*/', k) + 2
            continue
        elif c in '"\'':
            instr = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:k + 1]
        k += 1
    raise AssertionError(name)


getenv = re.compile(r'(?:getenv|gcm_env_flag\(&[A-Za-z_0-9]+,|gcm_observer_option)\s*\(?\s*"([A-Z0-9_]+)"')
walker = (video / 'cellGcmSys.c').read_text()
read = set(getenv.findall(body(walker, 'gcm_rsx_process_fifo_pass')))
read |= set(getenv.findall((video / 'gcm_fifo_observer.h').read_text()))
listed = set(re.findall(r'^\s*"([A-Z0-9_]+)",\s*$', (video / 'gcm_fifo_diag_env.h').read_text(), re.M))
unknown = sorted(read - listed - PER_PASS)
assert not unknown, f'read in the walker but classified neither as per-word (gcm_fifo_diag_env.h) nor per-pass: {unknown}'
# RSX_ENGINE_PHASES is read by gph_enabled, outside the pass; the pass tests its cached word.
assert listed <= read | {'RSX_ENGINE_PHASES'}, f'listed but not read on the path: {sorted(listed - read)}'
observer = (video / 'gcm_fifo_observer.h').read_text()
assert all(f'"{n}"' in walker + observer for n in listed), 'a listed variable is not read by the walker at all'
stale = sorted(PER_PASS - read)
assert not stale, f'classified per-pass but no longer read in the walker: {stale}'
print(f'PASS env scan: {len(read)} variables read in the walker; {len(listed & read)} per-word (listed), '
      f'{len(read & PER_PASS)} per-pass')

MUTANTS = {
    # checked REF not read again after the stand-down: the malformed
    # SET_REFERENCE after the unknown word then fails the mode.
    'stale-after-stand-down': [
        ('gcm_checked_ref_stand_down(s_fifo_getoff, w); checked = gcm_checked_ref_mode(); }',
         'gcm_checked_ref_stand_down(s_fifo_getoff, w); }'),
    ],
    # only the first data word's page tested: a packet crossing into an
    # unmapped page is no longer rejected.
    'first-page-only': [
        ('if (page > 0xFFFFu || !gcm_io2ea((u32)(page << 20))) { mapped = 0; break; }',
         'if (page > 0xFFFFu) { mapped = 0; break; }'),
    ],
    # the armed word never arms: the observer goes silent.
    'never-armed': [
        ('    s_fifo_diag = armed;\n    return armed;', '    s_fifo_diag = 0;\n    return 0;'),
    ],
    # checked REF read once but never at all (always off): the unmapped
    # packet is walked into.
    'checked-never': [
        ('    int checked = gcm_checked_ref_mode();\n    if (checked && atomic_load(&s_checked_ref_failed)) return;',
         '    int checked = 0;\n    if (checked && atomic_load(&s_checked_ref_failed)) return;'),
    ],
}


def build_and_run(tmp, label, include_first=None):
    binary = Path(tmp) / ('fixture-' + label)
    linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections', '-pthread', '-lm']
    includes = ([f'-I{include_first}'] if include_first else []) + ['-I' + str(root / 'include'), '-I' + str(video)]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG',
                    '-ffunction-sections', '-fdata-sections', *includes,
                    str(video / 'tests/test_gcm_fifo_lean.c'),
                    str(video / 'tests/gcm_deferred_fixture_stubs.c'), str(video / 'rsx_fifo_busy.c'), *linker,
                    '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=True, text=True)
    return result


parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true', help='also build each mutant and require a failure')
args = parser.parse_args()
failures = []
with tempfile.TemporaryDirectory(prefix='gcm-fifo-lean-') as tmp:
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
    raise SystemExit('test_gcm_fifo_lean failed: ' + ', '.join(failures))
print('[gcm_fifo_lean] PASS')
