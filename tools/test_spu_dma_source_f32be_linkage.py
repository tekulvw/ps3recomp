#!/usr/bin/env python3
"""Compile the real DMA headers as C++ and link to a C-defined runtime clock."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires POSIX C/C++ compilers')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
env = {k: v for k, v in os.environ.items() if not k.startswith('SPU_')}
env |= {'SPU_DMA_SOURCE_F32BE': '1', 'SPU_DMA_RANGE': '12000-12004'}
with tempfile.TemporaryDirectory(prefix='spu-dma-linkage-') as tmp:
    tmp = Path(tmp)
    clock = tmp/'clock.c'
    clock.write_text('#include "platform/win32_compat.h"\nvoid SetLastError(DWORD error) { (void)error; }\nunsigned long long ps3_ms_now(void) { return 6000; }\n')
    clock_obj = tmp/'clock.o'
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu17', '-I'+str(root/'runtime'), '-c', str(clock), '-o', str(clock_obj)], check=True)
    cpp = os.environ.get('CXX','c++')
    for header in ['spu_dma.h', 'spu_dma_source_trace.h']:
        source = tmp/'probe.cpp'
        source.write_text(f'''#include "{header}"
int main() {{
    static spu_context ctx;
    static uint8_t ls[SPU_LS_SIZE];
    ctx.ls = ls; ctx.image_id = 7; ctx.pc = 0x2340;
    ctx.gpr[0]._u32[0] = 0xA1234567; ctx.gpr[0]._u32[3] = 0x11111111;
    ctx.gpr[1]._u32[0] = 0xC7654321; ctx.gpr[1]._u32[3] = 0x22222222;
    spu_ls_write32(&ctx, 0x100, 0x3F000000);
    mfc_trace_source_f32be(&ctx, 0x100, 0x12000, 4, 0x20);
    return spu_ls_read32(&ctx, 0x100) == 0x3F000000 ? 0 : 1;
}}
''')
        binary = tmp/'probe'
        subprocess.run([cpp, '-std=gnu++17', '-O1', '-I'+str(root/'include'),
                        '-I'+str(root/'runtime/spu'), str(source), str(clock_obj), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, check=True, timeout=5)
        assert 'words=1 finite=1 nonzero=1 nonfinite=0 peak=0.5' in result.stderr, result.stderr
        assert 'ctx=0x' in result.stderr and 'r0=0xA1234567 r1=0xC7654321 seq=1' in result.stderr, result.stderr
    # Negative control: the same C object cannot satisfy a C++-linked clock.
    bad_source = tmp/'bad.cpp'
    bad_source.write_text('unsigned long long ps3_ms_now();\nint main() { return ps3_ms_now() == 6000 ? 0 : 1; }\n')
    bad_obj = tmp/'bad.o'
    subprocess.run([cpp, '-std=gnu++17', '-c', str(bad_source), '-o', str(bad_obj)], check=True)
    bad_link = subprocess.run([cpp, str(bad_obj), str(clock_obj), '-o', str(tmp/'bad')], capture_output=True, text=True)
    assert bad_link.returncode != 0 and 'ps3_ms_now' in bad_link.stderr, bad_link.stderr
print('PASS: full DMA header and standalone observer compile/link as C++ against C runtime clock; mismatched-linkage negative control rejected')
