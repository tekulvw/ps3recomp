#!/usr/bin/env python3
"""Verify opt-in source statistics through direct/list PUTs using synthetic data."""
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires a POSIX C compiler')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
with tempfile.TemporaryDirectory(prefix='spu-dma-source-') as tmp:
    binary = Path(tmp) / 'fixture'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', '-pthread',
                    *shlex.split(os.environ.get('CFLAGS', '')),
                    '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'),
                    str(root/'runtime/spu/tests/test_spu_dma_source_f32be.c'),
                    '-o', str(binary)], check=True)
    def run(extra, mode=None):
        result = subprocess.run([str(binary)] + ([mode] if mode else []),
                                env=env | extra, cwd=tmp, text=True,
                                capture_output=True, timeout=5, check=True)
        return [line for line in result.stderr.splitlines() if line.startswith('[dma-source-f32be]')]
    flag = {'SPU_DMA_SOURCE_F32BE': '1'}
    watch = {'SPU_DMA_RANGE': '12000-12020'}
    assert run(watch) == [], 'disabled diagnostic emitted output'
    for disabled in ['0', 'true', '']:
        assert run(watch | {'SPU_DMA_SOURCE_F32BE': disabled}) == []
    assert run(flag) == [], 'missing watch range emitted output'
    assert run(flag | {'SPU_DMA_RANGE': '12020-12000'}) == []
    assert run(flag | {'SPU_DMA_RANGE': '12000-100000001'}) == []
    assert run(flag | {'SPU_DMA_RANGE': '12000-12020garbage'}) == []
    for mode in [None, "list"]:
        lines = run(flag | watch, mode)
        assert len(lines) == 2, f'expected two rate-limited source reports, got {lines!r}'
        for line in lines:
            assert 'img=7 pc=0x02340 cmd=0x20 lsa=0x00100 ea=0x00012000 size=32' in line, line
            assert 'words=8 finite=5 nonzero=3 nonfinite=3 peak=2.5' in line, line
    lines = run(flag | watch, "threads")
    assert len(lines) == 1 and 'words=8 finite=5 nonzero=3 nonfinite=3 peak=2.5' in lines[0], lines
    # A partial watch must exclude words outside it; negative zero is not nonzero.
    lines = run(flag | {'SPU_DMA_RANGE': '12005-12010'})
    assert len(lines) == 2 and all('words=2 finite=2 nonzero=2 nonfinite=0 peak=2.5' in s for s in lines), lines
    lines = run(flag | {'SPU_DMA_RANGE': '12000-16000'}, "full")
    assert len(lines) == 1 and 'words=4096 finite=4096 nonzero=1 nonfinite=0 peak=1.25' in lines[0], lines
print('PASS: direct/list PUT source f32BE statistics, gating, bounds, partial range, and rate limit; DMA bytes unchanged')
