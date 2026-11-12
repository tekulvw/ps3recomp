#!/usr/bin/env python3
"""Run the lock-line coherence suites again with per-line version counters on.

The lifter-suite loop runs every tools/test_*.py with the default coherence
scheme. The suites below build the runtime's real coherence paths (GETLLAR and
PUTLLC, DMA and SDCRZ, PPU stores and store-conditionals, the SPURS and GCM host
sections), and each checks the guest-visible contract in a form that holds
under both schemes. This reruns them with PS3RECOMP_COH_VERSIONS=1, so a change
that breaks either scheme fails CI. The checks of one scheme's own machinery
(holder lists, epochs, the PPU store shard, wake counts) run only under it.
"""
import os
import subprocess
import sys
from pathlib import Path

if os.name == 'nt':
    print('SKIP: the suites below are POSIX fixtures')
    raise SystemExit(0)

SUITES = [
    'test_spu_context_registries.py',   # test_spu_coherency, test_spu_lockline_peer, registry allocation
    'test_spurs_flag_receiver.py',
    'test_spu_atomic_observe.py',
    'test_spu_sdcrz.py',
    'test_gcm_memory_to_memory.py',
    'test_ppu_spu_line_reservation.py',
    'test_ppu_store_fastpath.py',
    'test_ppu_store_live_spu.py',
    'test_ppu_header_inline.py',
    'test_vm_fast_accessors.py',
]

root = Path(__file__).resolve().parent
env = dict(os.environ, PS3RECOMP_COH_VERSIONS='1')
failed = []
for suite in SUITES:
    print('---', suite, 'with PS3RECOMP_COH_VERSIONS=1', flush=True)
    if subprocess.run([sys.executable, str(root / suite)], env=env).returncode:
        failed.append(suite)
if failed:
    raise SystemExit('failed with version counters: ' + ', '.join(failed))
print('PASS coherence suites with per-line version counters')
