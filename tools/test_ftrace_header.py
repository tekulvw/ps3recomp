#!/usr/bin/env python3
"""PS3_FTRACE's header carries the base clock reading (base_ns) span times are
relative to: base_ns + a span's t0 must equal the absolute clock value."""
import os
import re
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ftrace-header-') as tmp:
    binary, trace = Path(tmp) / ('t.exe' if os.name == 'nt' else 't'), Path(tmp) / 'trace.txt'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-I' + str(root / 'include'),
                    str(root / 'tests/test_ftrace_header.c'), '-o', str(binary)], check=True)
    env = dict(os.environ, PS3_FTRACE=f'0:60:{trace}', PS3_FTRACE_MIN_US='0')
    out = subprocess.run([str(binary)], env=env, check=True, capture_output=True, text=True).stdout
    vals = dict(re.findall(r'(\w+)=(\d+)', out))
    lines = trace.read_text().splitlines()
    header = dict(re.findall(r'(\w+)=(\S+)', lines[0]))
    assert lines[0].startswith('# ps3_ftrace ') and header['clock'] in ('QPC', 'CLOCK_MONOTONIC'), lines[0]
    base = int(header['base_ns'])
    assert int(vals['before_ns']) <= base <= int(vals['after_ns']), (base, vals)   # set at the first hook
    span = [l.split() for l in lines[1:] if l.split()[0] == '4'][0]
    t0 = base + round(float(span[4]) * 1000)
    assert abs(t0 - int(vals['span_t0_ns'])) <= 1000, (t0, vals, span)
print('PASS: ftrace header base_ns aligns span times with the absolute clock')
