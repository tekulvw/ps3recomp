#!/usr/bin/env python3
"""Execute manufactured ORBI/XORHI encodings and compare compiled lifted bodies."""
import os, subprocess, tempfile
from pathlib import Path
from spu_disasm import spu_decode
from spu_lifter import SPULifter
root=Path(__file__).resolve().parents[1]
if os.name=='nt':
    print('SKIP: POSIX C compiler fixture');raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix='spu-ri10-exec-') as tmp:
    tmp=Path(tmp);source=['#include "spu_helpers.h"','#include <stdlib.h>'];entries=[]
    for op,imm in [(6,4),(6,260),(6,-252),(6,-1),(0x45,4),(0x45,-1)]:
        for rt in (33,19):
            word=(op<<24)|((imm&1023)<<14)|(19<<7)|rt
            insn=spu_decode(word,0x100)
            assert insn.mnemonic==('orbi' if op==6 else 'xorhi')
            fn=SPULifter(prefix=f'ri10_{word:08x}_').lift_function([insn],0x100,0x104)
            source += [f'static void {fn.name}(spu_context* ctx) {{',*fn.body_lines,'}']
            entries += [f'case 0x{word:08x}u: {fn.name}(ctx); return;']
    source += ['static void lifted_run(uint32_t word,spu_context* ctx) {switch(word) {',*entries,'default: abort();}}']
    generated=tmp/'lifted.h';generated.write_text('\n'.join(source)+'\n')
    exe=tmp/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1','-UNDEBUG','-DRI10_LIFTED','-I'+str(root/'include'),'-I'+str(root/'runtime/spu'),'-include',str(generated),str(root/'runtime/spu/tests/test_spu_interpreter_orbi_xorhi.c'),str(root/'runtime/spu/spu_interp.c'),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=5,env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_'))})
