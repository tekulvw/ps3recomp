#!/usr/bin/env python3
"""The native WWS job manager's pure core (runtime/spu/wwsjm/wwsjm_core.c).

runtime/spu/tests/test_wwsjm_core.c checks the core against hand-computed
vectors (job list claims and retires, gates, array switches, index wrap,
dependency decrements, ready counts, command lists, refusals, buffer
placement, DMA tags, gather layout, module residency) and generated property
cases. Every input is synthetic.

--mutants builds each listed mutation of the core and requires the fixture to
fail it (the validation plan's M1, M2, M5-M8 and the contract details the
replay of recorded runs established).
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
wwsjm = root / 'runtime/spu/wwsjm'
test_c = root / 'runtime/spu/tests/test_wwsjm_core.c'

MUTANTS = {
    # M1: retire publishes without the lane halfword
    'M1-retire-skips-lane': ('    wwsjm_put16(out + lane_offset(array, lane), v);\n',
                             '    wwsjm_put16(out + marker_offset(array) + 2, (uint16_t)(v + 0 * lane));\n'),
    # M2: the lane marker records index + 1 at claim
    'M2-lane-marker-plus-one': ('            wwsjm_put16(mine, taken);\n', '            wwsjm_put16(mine, next);\n'),
    # M5: a gate passes with a non-zero counter
    'M5-gate-ignores-counter': ('    if (wwsjm_header_is_gate(h) && gate_value != 0)\n',
                                '    if (wwsjm_header_is_gate(h) && gate_value > 1)\n'),
    # M6: shareable copies reloaded every time / never reloaded
    'M6a-always-reload': ('        resident = resident && uses;\n', '        resident = 0 * uses;\n'),
    'M6b-never-reload': ('        resident = resident && uses;\n', '        resident = uses;\n'),
    # M7: gather element sizes rounded down
    'M7-gather-rounds-down': ('        uint32_t span = (word + 15u) & ~15u;\n', '        uint32_t span = word & ~15u;\n'),
    # M8: the context's userdata pointer is stale / unaligned
    'M8a-userdata-unaligned': ('    out->userdata_offset = (out->command_bytes + 15u) & ~15u;\n',
                               '    out->userdata_offset = out->command_bytes;\n'),
    'M8b-context-stale-copy': ('        wwsjm_put32(out + 0x20 + 4 * i, a->userdata_ls);\n',
                               '        wwsjm_put32(out + 0x20 + 4 * i, a->userdata_ls - (i == 0 ? 0x10u : 0u));\n'),
    # the S rule: S never raised / always N + 1
    'S-never-raised': ('    if (before16(wwsjm_be16(m), next))\n', '    if (0)\n'),
    'S-always-set': ('    if (before16(wwsjm_be16(m), next))\n', '    if (1)\n'),
    # array switch modulus
    'switch-mod-3': ('(uint16_t)((cur + 1u) % WWSJM_ARRAYS)', '(uint16_t)((cur + 1u) % 3u)'),
    # header slot: AND with capacity - 1 instead of mod capacity
    'slot-and-mask': ('8u * (capacity ? (uint32_t)index % capacity : index)',
                      '8u * ((uint32_t)index & l->mask[a] + 0 * capacity)'),
    # ready rule: busy whenever claimable, or S >= N
    'ready-ignores-count': ('    return before16(l->m[a].next, l->m[a].submitted) ? busy : 0;\n', '    return busy;\n'),
    'ready-counts-equal': ('    return before16(l->m[a].next, l->m[a].submitted) ? busy : 0;\n',
                           '    return before16((uint16_t)(l->m[a].next - 1u), l->m[a].submitted) ? busy : 0;\n'),
    # counters decrement twice
    'decrement-by-two': ('    uint16_t v = (uint16_t)(wwsjm_be16(old + offset) - 1u);\n',
                         '    uint16_t v = (uint16_t)(wwsjm_be16(old + offset) - 2u);\n'),
    # get-buffer: tag dropped / gather source dropped / buffer byte dropped
    'descriptor-no-tag': ('    out[3] = tag;\n', '    out[3] = (uint8_t)(tag & 0);\n'),
    'descriptor-no-source': ('            wwsjm_put32(out + 8, u->ea);\n', '            wwsjm_put32(out + 8, 0);\n'),
    'descriptor-no-buffer': ('    out[1] = (uint8_t)buf;\n', '    out[1] = 0;\n'),
    # placement: the ring never rotates
    'placement-no-rotation': ('                start = b;\n', '                start = 0 * b;\n'),
    # held LS ignores releases
    'held-ignores-release': ('(released >> k & 1)', '(released >> k & 0)'),
    # gather: the quadword offset of the EA is ignored; stall flags accepted
    'gather-drops-ea-offset': ('        out[k].ls = ls + at + (ea & 15u);\n', '        out[k].ls = ls + at;\n'),
    'gather-accepts-stall': ('        if (word & 0xFFFF8000u)                /* stall-and-notify or reserved bits */\n'
                             '            return WWSJM_E_GATHER;\n', '        word &= 0x7FFFu;\n'),
}


def build_and_run(tmp, label, core_c):
    binary = Path(tmp) / ('wwsjm-' + label)
    cc = os.environ.get('CC', 'clang-cl' if os.name == 'nt' else 'cc')
    inc = str(wwsjm)
    if os.name == 'nt':
        binary = Path(str(binary) + '.exe')
        cmd = [cc, '/nologo', '/std:c11', '/W3', '/I' + inc, str(test_c), str(core_c), '/Fe:' + str(binary)]
    else:
        cmd = [cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I' + inc, str(test_c), str(core_c),
               '-o', str(binary)]
    subprocess.run(cmd, check=True, cwd=tmp)
    return subprocess.run([str(binary)], timeout=120, capture_output=True, text=True)


parser = argparse.ArgumentParser()
parser.add_argument('--mutants', action='store_true')
args = parser.parse_args()
failures = []
source = (wwsjm / 'wwsjm_core.c').read_text()
with tempfile.TemporaryDirectory(prefix='wwsjm-core-') as tmp:
    result = build_and_run(tmp, 'core', wwsjm / 'wwsjm_core.c')
    print(result.stdout.strip())
    if result.returncode:
        print(result.stderr[-4000:])
        failures.append('fixture')
    if args.mutants and not failures:
        for name, (old, new) in MUTANTS.items():
            assert source.count(old) == 1, (name, old)
            # The mutated copy finds wwsjm_core.h through -I.
            mutant = Path(tmp) / f'mutant-{name}.c'
            mutant.write_text(source.replace(old, new, 1))
            r = build_and_run(tmp, f'mutant-{name}', mutant)
            caught = r.returncode != 0
            print(f"[{'PASS' if caught else 'FAIL'}] mutant {name} {'caught' if caught else 'NOT caught'}")
            if not caught:
                failures.append(f'mutant {name}')
if failures:
    raise SystemExit('test_wwsjm_core failed: ' + ', '.join(failures))
print('[wwsjm_core] PASS')
