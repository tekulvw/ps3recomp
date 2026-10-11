#!/usr/bin/env python3
"""Verify unresolved-syscall policy through the runnable synthetic PPU lift."""
import argparse
import os
import subprocess
from pathlib import Path

def main():
    if os.name == 'nt':
        print('SKIP: regression requires the POSIX PPU test toolchain')
        return
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/ps3recomp_boot_smoke'))
    parser.add_argument('--elf', type=Path, default=Path('build/smoke/smoke.elf'))
    args=parser.parse_args()
    for mode, expected in [('ok',0), ('halt',87)]:
        env=os.environ.copy()
        env.update(PS3RECOMP_SMOKE_UNKNOWN_SYSCALL='1', PS3_SYSCALL_UNRESOLVED=mode,
                   PS3RECOMP_METAL_HEADLESS='1')
        p=subprocess.run([str(args.binary.resolve()), str(args.elf.resolve())], env=env,
                         capture_output=True, text=True, timeout=15)
        if p.returncode != expected:
            raise AssertionError(f'{mode}: expected exit {expected}, got {p.returncode}\n{p.stdout}\n{p.stderr}')
        if mode=='halt':
            assert 'UNRESOLVED syscall 1023' in p.stderr
            assert 'lr=0x12345678' in p.stderr
            assert 'unknown syscall returned' not in p.stderr
        else:
            assert 'unknown syscall returned 0' in p.stderr
    print('PASS: unknown syscall default remains compatible; halt stops before returning success')

if __name__=='__main__':
    main()
