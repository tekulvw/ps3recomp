#!/usr/bin/env python3
"""tools/ppu_header_inline.py: inline guest stores (and loads, for an old header) behave as the out-of-line ones.

The lifter's own header now has inline loads and stores (ppu_mm, PPU_MM_SNAP); the tool is for headers
generated before that. Lifts a stub function, then builds the real ppu_loader.cpp and spu_coherency.c
with these and runs tools/test_vm_fast_accessors.py's driver (values at every width and offset, raw SPU
registers, the GCM ref-poll and PUT wake words, the dropped null-page store, LR on stores to an
SPU-reserved line, and the diagnostics that must turn the fast paths off) against each:
  - the lifter's header with PPU_NO_INLINE_STORES (inline loads, out-of-line stores): the reference;
  - the lifter's header as generated (its own inline stores);
  - an old-style header and lifted source without any inline accesses, given loads and stores by the tool.
The transcripts must be identical. Each also runs with the runtime's fast accessors compiled in. The tool
must refuse a header that already has inline stores, as the lifter's now does.
"""
import os
if os.name == 'nt':
    print('SKIP: POSIX fixture: compiles ppu_loader.cpp with cc/c++, whose Windows path needs MSVC __try/__except')
    raise SystemExit(0)
import os, re, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ppu_lifter as L
from ppu_disasm import Instruction
root = Path(__file__).resolve().parents[1]
driver = re.search(r"driver = r'''(.*?)'''", (root / 'tools/test_vm_fast_accessors.py').read_text(), re.S).group(1)
LOADS_START = '/* Inline guest-load fast paths.'
LOADS_END = '#define vm_write64(a, v) vm_write64_inl(a, v)\n#endif\n'   # the header's inline loads and stores
MM_START = '/* Each lifted function opens with PPU_MM_SNAP;'                 # the source's per-function copy
MM_END = '#define vm_write64(a, v) vm_write64_mm(&_mm, a, v)\n#endif\n'

with tempfile.TemporaryDirectory(prefix='ppu-header-inline-') as tmp:
    p = Path(tmp); l = L.PPULifter()
    l.lift_function([Instruction(0x1000, 0, 'blr', '')], 0x1000, 0x1004)
    header = l.emit_header()
    source = l.emit_source()
    assert 'vm_read32_inl' in header and 'vm_write32_inl' in header and 'PPU_MM_SNAP' in source
    i, j = header.index(LOADS_START), header.index(LOADS_END) + len(LOADS_END)
    old = header[:i] + header[j:]
    i, j = source.index(MM_START), source.index(MM_END) + len(MM_END)
    old_source = '\n'.join(x for x in (source[:i] + source[j:]).split('\n') if x.strip() != 'PPU_MM_SNAP')
    assert 'vm_read32_inl' not in old and 'vm_write32_inl' not in old and 'PPU_MM_SNAP' not in old_source
    dirs = {}
    for name, text, src in (('lifter', header, source), ('old', old, old_source)):
        d = p / f'src-{name}'; d.mkdir(); (d / 'ppu_recomp.h').write_text(text); (d / 'lifted.cpp').write_text(src)
        dirs[name] = d
    tool = [sys.executable, str(root / 'tools/ppu_header_inline.py')]
    subprocess.run(tool + [str(dirs['old']), str(p / 'src-old-both'), '--stores'], check=True)
    for already in (dirs['lifter'], p / 'src-old-both'):
        r = subprocess.run(tool + [str(already), str(p / f'src-twice-{already.name}'), '--stores'], capture_output=True, text=True)
        assert r.returncode != 0, 'a header with inline stores must be refused'
    variants = {'reference': (dirs['lifter'], ['-DPPU_NO_INLINE_STORES']), 'lifter': (dirs['lifter'], []),
                'old+both': (p / 'src-old-both', [])}
    for v, _ in variants.values():
        assert 'vm_read32_inl' in (v / 'ppu_recomp.h').read_text()
        assert 'vm_write32_inl' in (v / 'ppu_recomp.h').read_text()
    common = ['-O2', '-ffunction-sections', '-fdata-sections']
    transcripts = {}
    for vname, (src, vdefines) in variants.items():
        (src / 'driver.cpp').write_text(driver)
        for fast, defines in (('slow', vdefines), ('fast', vdefines + ['-DPS3RECOMP_VM_FAST_ACCESSORS=1'])):
            b = p / f'b-{vname}-{fast}'; b.mkdir()
            inc = ['-I'+str(root/'runtime/ppu'), '-I'+str(src), '-I'+str(root/'include'), '-I'+str(root/'runtime/spu'), '-I'+str(root/'runtime/platform')]
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', *common, '-I'+str(root/'include'), '-c', str(root/'runtime/spu/spu_coherency.c'), '-o', str(b/'coh.o')], check=True)
            subprocess.run([os.environ.get('CC', 'cc'), *common, '-c', str(root/'runtime/platform/win32_compat.c'), '-o', str(b/'compat.o')], check=True)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-UNDEBUG', *defines, *common, *inc, str(root/'runtime/ppu/ppu_loader.cpp'),
                            str(src/'driver.cpp'), str(src/'lifted.cpp'), str(b/'coh.o'), str(b/'compat.o'), '-pthread',
                            ('-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'), '-o', str(b/'test')], check=True)
            out = []
            for mode, env in (('values', {}), ('force-read', {'PPU_FORCE_READ_ADDR': '00100100', 'PPU_FORCE_READ_VAL': 'CAFE'}),
                              ('wwatch', {'PPU_WWATCH': '00100200'})):
                e = {k: v for k, v in os.environ.items() if not k.startswith(('PPU_', 'PS3_', 'SPU_', 'YDKJ_'))}; e.update(env)
                r = subprocess.run([str(b/'test'), mode], capture_output=True, text=True, timeout=60, env=e)
                if r.returncode:
                    print(r.stdout, r.stderr[-3000:]); raise SystemExit(f'{vname}/{fast}:{mode} failed')
                out.append(r.stdout.strip())
            transcripts[f'{vname}/{fast}'] = out
            print(f'{vname}/{fast}:', ' | '.join(out))
    ref = transcripts['reference/slow']
    for k, v in transcripts.items():
        assert v == ref, f'{k} differs from the out-of-line stores'
    print('PASS inline guest stores and header-applied loads: same results as the out-of-line accessors')
