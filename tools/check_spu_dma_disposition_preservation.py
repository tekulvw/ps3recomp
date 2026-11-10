#!/usr/bin/env python3
"""Check that the DMA disposition observer stays a pure addition to spu_dma.h.

The observer (runtime/spu/spu_dma_disposition*.h) hooks mfc_do_transfer in a
fixed set of forms: one include, the token and its begin call, the outcome
local, a finish wrapper on every return, a before/after sample pair around
each copy and an outcome assignment after each copy. Stripping exactly those forms must leave no other
reference to the observer, so the observer cannot change what a transfer does.

This check used to compare the stripped file byte for byte with 45bcb9a1. That
was the observer's parent before it was rebased, not the commit it landed on
(356446e8), so the comparison never held on main; against the real parent it
held only until f83c6c3e, the first later change to spu_dma.h. Any change to
the DMA code broke it whatever the observer did. It now checks the structure
of the current file, which stays meaningful as the DMA code changes.

--base REV also compares the stripped file with the stripped file at REV, byte
for byte: evidence that a change to spu_dma.h only moved or added hooks.
"""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PATH = 'runtime/spu/spu_dma.h'
INCLUDE = '#include "spu_dma_disposition.h"\n'
ADMISSION = ('    spu_dma_disposition_token disposition;\n'
             '    spu_dma_disposition_begin(&disposition, lsa, ea, size, cmd);\n')
OUTCOME_LOCAL = '    unsigned copy_disposition = SPU_DMA_DISPOSITION_NONE;\n'


def strip(text):
    """Remove the declared hook forms; return the rest and how many of each."""
    counts = {'include': text.count(INCLUDE), 'admission': text.count(ADMISSION),
              'outcome_local': text.count(OUTCOME_LOCAL)}
    text = text.replace(INCLUDE, '').replace(ADMISSION, '').replace(OUTCOME_LOCAL, '')
    text, counts['finish'] = re.subn(
        r'return spu_dma_disposition_finish\(&disposition, spu, lsa, ea, size, cmd, (-1|0), '
        r'(?:SPU_DMA_DISPOSITION_[A-Z_]+|copy_disposition)\);', r'return \1;', text)
    text, counts['sample'] = re.subn(r'^ +spu_dma_disposition_(?:before|after)\([^\n]+\);\n', '',
                                     text, flags=re.M)
    text, counts['outcome'] = re.subn(r'^ +copy_disposition = SPU_DMA_DISPOSITION_(?:GET|PUT);\n',
                                      '', text, flags=re.M)
    return text, counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', help='also compare with the stripped spu_dma.h at this revision')
    args = parser.parse_args()
    current, counts = strip((ROOT / PATH).read_text())
    assert counts['include'] == 1 and counts['admission'] == 1 and counts['outcome_local'] == 1, counts
    # Each copy is bracketed by one before/after pair and followed by one outcome.
    assert counts['outcome'] >= 3 and counts['sample'] == 2 * counts['outcome'], counts
    assert counts['finish'] >= 9, counts
    leftover = [n + 1 for n, line in enumerate(current.split('\n')) if 'disposition' in line]
    assert not leftover, f'{PATH}: observer references outside the declared forms (stripped lines {leftover})'
    message = (f"PASS: {counts['finish']} return wrappers, {counts['sample']} sample hooks, "
               f"{counts['outcome']} outcome assignments, admission locals and one include; "
               "no other observer reference")
    if args.base:
        old, _ = strip(subprocess.check_output(['git', 'show', f'{args.base}:{PATH}'], cwd=ROOT, text=True))
        assert old == current, f'{PATH} differs from {args.base} outside the observer hooks'
        message += f'; identical to {args.base} outside them, sha256={hashlib.sha256(current.encode()).hexdigest()}'
    print(message)


if __name__ == '__main__':
    main()
