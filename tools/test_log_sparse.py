#!/usr/bin/env python3
"""ps3_log_sparse (include/ps3emu/log_first.h): a repeating report that starts dense
(step, 2*step, 4*step ... below step*cap) and then settles at every multiple of step*cap."""
import os, shutil, subprocess, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <stdio.h>
#include "ps3emu/log_first.h"
int main(void) {
    unsigned long long n;
    /* The census schedule: 1Ki ... 512Ki, then every 1Mi. */
    for (n = 1; n <= 3ull << 20; ++n) if (ps3_log_sparse(n, 1024, 1024)) printf("%llu\n", n);
    printf("--\n");
    /* The pipeline-reuse schedule: 50K ... 1.6M, then every 3.2M. */
    for (n = 1; n <= 10000000ull; ++n) if (ps3_log_sparse(n, 50000, 64)) printf("%llu\n", n);
    printf("--\n");
    printf("%d %d %d %d\n", ps3_log_sparse(0, 1, 1), ps3_log_sparse(5, 0, 4), ps3_log_sparse(5, 5, 0), ps3_log_sparse(1, 1, 1));
    return 0;
}
'''

def main():
    cc = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('gcc') or shutil.which('clang')
    if not cc:
        if os.environ.get('CI'):
            print('FAIL: no C compiler'); return 1
        print('skip test_log_sparse: no C compiler'); return 0
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / 'sparse.c', Path(tmp) / ('sparse.exe' if os.name == 'nt' else 'sparse')
        src.write_text(SOURCE)
        subprocess.run([cc, '-std=c99', '-O1', '-I' + str(root / 'include'), str(src), '-o', str(exe)], check=True)
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.split('--\n')
    census = [int(x) for x in out[0].split()]
    assert census == [1024 << k for k in range(10)] + [1 << 20, 2 << 20, 3 << 20], census
    reuse = [int(x) for x in out[1].split()]
    assert reuse == [50000 << k for k in range(6)] + [3200000, 6400000, 9600000], reuse
    assert out[2].split() == ['0', '0', '0', '1'], out[2]
    print('log sparse: ok')
    return 0

if __name__ == '__main__':
    sys.exit(main())
