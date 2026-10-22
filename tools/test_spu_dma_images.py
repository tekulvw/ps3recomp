#!/usr/bin/env python3
"""SPU_DMA_IMAGES: a listed image's transfers are reported, others are not.
Reuses the synthetic DMA-disposition fixture (image 7, synthetic memory)."""
import os, subprocess, tempfile, re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'LBP_'))}
with tempfile.TemporaryDirectory(prefix='dma-images-') as tmp:
    binary = Path(tmp) / 'fixture'
    flags = ['-std=gnu17', '-O2', '-UNDEBUG', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu')]
    subprocess.run([os.environ.get('CC', 'cc'), *flags, str(root / 'runtime/spu/tests/test_spu_dma_disposition.c'),
                    '-o', str(binary)], check=True)
    def run(settings):
        p = subprocess.run([str(binary)], env=dict(env, **settings), capture_output=True, text=True, check=True, timeout=10)
        return [l for l in p.stderr.splitlines() if l.startswith('[dma-img]')]
    assert run({}) == []
    assert run({'SPU_DMA_IMAGES': '8'}) == []
    rows = run({'SPU_DMA_IMAGES': '3,7'})
    assert rows, rows
    parsed = [dict(re.findall(r'(\w+)=(\S+)', r)) for r in rows]
    assert all(r['img'] == '7' for r in parsed), rows
    assert parsed[0]['cmd'] == '0x20' and parsed[0]['ea'] == '0x00012000' and parsed[0]['size'] == '32', rows
    assert any(r['cmd'] == '0x40' for r in parsed), rows
print('spu dma images: ok')
