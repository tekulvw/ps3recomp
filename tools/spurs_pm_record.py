#!/usr/bin/env python3
"""Decode SPURS policy-run recordings (PS3RECOMP_SPURS_PM_RECORD).

The runtime writes one file per policy worker thread (pmrec-<pid>-t<n>.bin,
runtime/spu/spurs_pm_record.inc). This module knows the file format only; it
says nothing about any particular policy module.

File: a 32-byte header ("SPMR", u32 version, u32 header bytes, u32 byte cap,
u32 pid, u32 worker, u64 monotonic ns at open), then records of
{u8 kind, u8 0, u16 0, u32 payload bytes, payload}. All fields little-endian.

    python3 tools/spurs_pm_record.py <file-or-dir>... [--events] [--json]

prints a per-run summary (default), every event (--events), or JSON lines.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

MAGIC = b'SPMR'
VERSION = 1
RUN_BEGIN, RUN_END, COMMAND, TRANSFER, ATOMIC, BRANCH, KERNEL, LSREAD, REGS = range(1, 10)
KIND_NAMES = {RUN_BEGIN: 'run_begin', RUN_END: 'run_end', COMMAND: 'command', TRANSFER: 'transfer',
              ATOMIC: 'atomic', BRANCH: 'branch', KERNEL: 'kernel', LSREAD: 'lsread', REGS: 'regs'}
LSREAD_IN_CTX, LSREAD_AT_CTX = 1, 2
XFER_NAMES = {1: 'get', 2: 'put', 3: 'zero'}
BRANCH_NAMES = {1: 'job_enter', 2: 'job_return', 3: 'service_call', 4: 'service_return'}
KERNEL_NAMES = {1: 'select', 2: 'exit'}
REGION_NAMES = {0: 'policy', 1: 'job', 2: 'service'}
ATOMIC_NAMES = {0xD0: 'getllar', 0xB4: 'putllc', 0xB0: 'putlluc', 0xB8: 'putqlluc'}
NO_SLOT = 0xFFFFFFFF


class FormatError(ValueError):
    pass


def _fields(fmt: str, names: str, data: bytes, off: int = 0) -> tuple[dict, int]:
    vals = struct.unpack_from('<' + fmt, data, off)
    return dict(zip(names.split(), vals)), off + struct.calcsize('<' + fmt)


def decode_payload(kind: int, p: bytes) -> dict:
    if kind == RUN_BEGIN:
        e, o = _fields('IIIIQIIIIQIIQ', 'run wid lane spurs_ea wkl_data r5 image_id pm_ea pm_size pm_fp '
                       'fresh line_valid t_ns', p)
        e['line'] = p[o:o + 128]
        if len(e['line']) != 128:
            raise FormatError('short run_begin')
    elif kind == RUN_END:
        e, o = _fields('IiIIIIIIQ', 'run result polls exited status pc jobs line_valid t_ns', p)
        e['line'] = p[o:o + 128]
        if len(e['line']) != 128:
            raise FormatError('short run_end')
    elif kind == COMMAND:
        e, _ = _fields('IIQIIII', 'cmd lsa ea size tag region depth', p)
    elif kind == TRANSFER:
        e, o = _fields('IIIIQQIII', 'xfer cmd lsa size ea hash region depth captured', p)
        e['xfer'] = XFER_NAMES.get(e['xfer'], e['xfer'])
        e['bytes'] = p[o:o + e['captured']]
        if len(e['bytes']) != e['captured']:
            raise FormatError('short transfer')
    elif kind == ATOMIC:
        e, o = _fields('IIIIII', 'cmd lsa ea status region depth', p)
        e['op'] = ATOMIC_NAMES.get(e['cmd'], hex(e['cmd']))
        e['line'] = p[o:o + 128]
        if len(e['line']) != 128:
            raise FormatError('short atomic')
    elif kind == BRANCH:
        e, o = _fields('IIII4I' + 'I' * 9, 'branch pc r0 r1 r3_0 r3_1 r3_2 r3_3 r4 r5 r6 depth slot '
                       'module_ea ctx_lsa how extra_n', p)
        e['branch'] = BRANCH_NAMES.get(e['branch'], e['branch'])
        e['r3'] = [e.pop(f'r3_{i}') for i in range(4)]
        e['inferred'] = bool(e.pop('how'))
        e['extra'] = p[o:o + e.pop('extra_n')]
    elif kind == KERNEL:
        e, _ = _fields('I4IIIII', 'service r3_0 r3_1 r3_2 r3_3 region depth polls link', p)
        e['service'] = KERNEL_NAMES.get(e['service'], e['service'])
        e['r3'] = [e.pop(f'r3_{i}') for i in range(4)]
    elif kind == LSREAD:
        # PS3RECOMP_SPURS_PM_RECORD_LSREAD: a job-region local-store quadword read
        e, o = _fields('IIIIIIIII4II', 'lsa size flags module_ea entry ctx_lsa pc r0 r1 '
                       'r3_0 r3_1 r3_2 r3_3 pc_module_ea', p)
        e['r3'] = [e.pop(f'r3_{i}') for i in range(4)]
        e['in_ctx'] = bool(e['flags'] & LSREAD_IN_CTX)
        e['at_ctx'] = bool(e['flags'] & LSREAD_AT_CTX)
        e['value'] = p[o:o + 16]
        if len(e['value']) != 16:
            raise FormatError('short lsread')
    elif kind == REGS:
        # PS3RECOMP_SPURS_PM_RECORD_REGS: the register file at a job entry (1) or halt (2)
        e, o = _fields('IIIIIIII', 'why pc int_enable event_mask status region entry module_ea', p)
        e['why'] = {1: 'job_enter', 2: 'halt'}.get(e['why'], e['why'])
        raw = p[o:o + 2048]
        if len(raw) != 2048:
            raise FormatError('short regs')
        e['gpr'] = [list(struct.unpack_from('<4I', raw, 16 * r)) for r in range(128)]
        e['ls'] = p[o + 2048:]   # at a halt: local store from the halted function's entry
    else:
        raise FormatError(f'unknown record kind {kind}')
    if 'region' in e:
        e['region'] = REGION_NAMES.get(e['region'], e['region'])
    e['kind'] = KIND_NAMES[kind]
    return e


def read_file(path: Path) -> tuple[dict, list[dict]]:
    data = Path(path).read_bytes()
    if len(data) < 32 or data[:4] != MAGIC:
        raise FormatError(f'{path}: not a policy-run recording')
    head, _ = _fields('IIIIIQ', 'version header_bytes byte_cap pid worker t0_ns', data, 4)
    if head['version'] != VERSION:
        raise FormatError(f'{path}: version {head["version"]} (decoder knows {VERSION})')
    off, events = head['header_bytes'], []
    while off < len(data):
        if off + 8 > len(data):
            raise FormatError(f'{path}: truncated record header at {off}')
        kind, length = data[off], struct.unpack_from('<I', data, off + 4)[0]
        payload = data[off + 8:off + 8 + length]
        if len(payload) != length:
            raise FormatError(f'{path}: truncated record at {off}')
        events.append(decode_payload(kind, payload))
        off += 8 + length
    return head, events


def runs(events: list[dict]) -> list[list[dict]]:
    """Split a worker's events into runs (run_begin ... run_end)."""
    out, cur = [], None
    for e in events:
        if e['kind'] == 'run_begin':
            cur = [e]
            out.append(cur)
        elif cur is not None:
            cur.append(e)
            if e['kind'] == 'run_end':
                cur = None
    return out


def _jsonable(e: dict) -> dict:
    return {k: (v.hex() if isinstance(v, (bytes, bytearray)) else v) for k, v in e.items()}


def _files(args: list[str]) -> list[Path]:
    out = []
    for a in args:
        p = Path(a)
        out.extend(sorted(p.glob('pmrec-*.bin')) if p.is_dir() else [p])
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--events', action='store_true', help='print every event')
    ap.add_argument('--json', action='store_true', help='JSON lines (one per event)')
    a = ap.parse_args(argv)
    for f in _files(a.paths):
        head, events = read_file(f)
        if a.json:
            for e in events:
                print(json.dumps(dict(_jsonable(e), file=f.name)))
            continue
        print(f'{f.name}: worker={head["worker"]} pid={head["pid"]} byte_cap={head["byte_cap"]} events={len(events)}')
        for r in runs(events):
            b, kinds = r[0], {}
            for e in r[1:]:
                key = e.get('branch') or e.get('service') or e.get('op') or e.get('xfer') or e['kind']
                kinds[key] = kinds.get(key, 0) + 1
            print(f'  run {b["run"]} wid={b["wid"]:X} lane={b["lane"]} inst=0x{b["spurs_ea"]:X} '
                  f'data=0x{b["wkl_data"]:X} ' + ' '.join(f'{k}={v}' for k, v in sorted(kinds.items())))
            if a.events:
                for e in r[1:]:
                    print('    ' + json.dumps(_jsonable(e)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
