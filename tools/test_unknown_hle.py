#!/usr/bin/env python3
"""Exercise unresolved-import policy through the real PPU dispatcher."""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    if os.name == 'nt':
        print('SKIP: regression requires the POSIX PPU test toolchain')
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/ps3recomp_boot_smoke'))
    parser.add_argument('--elf', type=Path, default=Path('build/smoke/smoke.elf'))
    args = parser.parse_args()
    for mode, expected, value in [(None, 0, '00000000'), ('ok', 0, '00000000'),
                                  ('fail', 1, '8001003F'), ('halt', 88, None)]:
        env = os.environ.copy()
        env.pop('PS3_HLE_UNRESOLVED', None)
        env.update(PS3RECOMP_SMOKE_UNKNOWN_HLE='1', PS3RECOMP_METAL_HEADLESS='1')
        if mode:
            env['PS3_HLE_UNRESOLVED'] = mode
        p = subprocess.run([str(args.binary.resolve()), str(args.elf.resolve())],
                           env=env, capture_output=True, text=True, timeout=15)
        assert p.returncode == expected, f'{mode}: expected {expected}, got {p.returncode}\n{p.stderr}'
        assert 'UNRESOLVED NID 0xDEADBEEF' in p.stderr, p.stderr
        if mode == 'halt':
            assert 'lr=0x12345678' in p.stderr, p.stderr
            assert 'unknown HLE returned' not in p.stderr
        else:
            assert f'unknown HLE returned {value}' in p.stderr, p.stderr
    print('PASS: unresolved HLE default/ok, fail, and halt policies')


if __name__ == '__main__':
    main()
