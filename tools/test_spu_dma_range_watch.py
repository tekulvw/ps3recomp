#!/usr/bin/env python3
"""SPU_DMA_RANGE: a fixed <lo>-<hi> window, or =watch following the runtime's
store-watch window (g_ww_lo..g_ww_hi) as it stands at each transfer. Rows name
the context image and the resident job images. Reuses the synthetic
DMA-disposition fixture (image 7, synthetic memory)."""
import os, subprocess, tempfile, re
from pathlib import Path
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items() if not k.startswith(('SPU_', 'SPURS_', 'LBP_'))}
with tempfile.TemporaryDirectory(prefix='dma-range-') as tmp:
    binary = Path(tmp) / 'fixture'
    flags = ['-std=gnu17', '-O2', '-UNDEBUG', '-pthread', '-I' + str(root / 'include'), '-I' + str(root / 'runtime/spu')]
    subprocess.run([os.environ.get('CC', 'cc'), *flags, str(root / 'runtime/spu/tests/test_spu_dma_disposition.c'),
                    '-o', str(binary)], check=True)
    def run(settings, mode):
        p = subprocess.run([str(binary), mode], env=dict(env, **settings), capture_output=True, text=True, check=True, timeout=10)
        return [dict(re.findall(r'(\w+)=(\S+)', l)) for l in p.stderr.splitlines() if l.startswith('[dma-range]')]
    # Unset: nothing. A fixed window: only the transfer inside it.
    assert run({}, 'watch') == []
    rows = run({'SPU_DMA_RANGE': '12020-12030'}, 'watch')
    assert len(rows) == 1 and rows[0]['ea'] == '0x00012010' and rows[0]['cmd'] == '0x20', rows
    # The dynamic window: the PUT before arming and the PUT outside are not
    # reported; the PUT inside and, after the window moves, the GET are.
    rows = run({'SPU_DMA_RANGE': 'watch'}, 'watch')
    assert [(r['ea'], r['cmd']) for r in rows] == [('0x00012010', '0x20'), ('0x00013000', '0x40')], rows
    assert rows[0]['img'] == '7' and rows[0]['res'] == '224' and rows[0]['window'] == '0x00012000-0x00012020', rows
    assert rows[1]['window'] == '0x00013000-0x00013040', rows
    # Rejections past the 8 logged lines are counted by issue site: the 9th
    # opens the census, naming the resident job that issued them.
    p = subprocess.run([str(binary), 'census'], env=env, capture_output=True, text=True, check=True, timeout=10)
    logged = [l for l in p.stderr.splitlines() if l.startswith('[mfc] REJECTED')]
    census = [l for l in p.stderr.splitlines() if l.startswith('[mfc] rejected census')]
    assert len(logged) == 8 and len(census) == 1, p.stderr
    c = dict(re.findall(r'(\w+)=(\S+)', census[0]))
    assert c['pc'] == '0x02340' and c['cmd'] == '0x40' and c['n'] == '9' and c['res'] == '224', census
print('spu dma range watch: ok')
