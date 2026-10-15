#!/usr/bin/env python3
"""Exercise the real SPU decoder and interpreter without title data.

Only a test counter is inserted into a temporary copy of spu_decode1; no
production test hooks or architectural changes are added to the SDK.
"""
import os
import subprocess
import tempfile
from pathlib import Path

root=Path(__file__).resolve().parents[1]
spu=root/'runtime/spu'
source=(spu/'spu_interp.c').read_text()
needle='static void spu_decode1(uint32_t insn, uint32_t pc, spu_ins* d) {'
assert source.count(needle)==1
source=source.replace(needle,needle+'\n    ++test_decode_calls;')
prefix='_Thread_local unsigned long test_decode_calls;\n'
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','YDKJ_','LBP_','GH3_'))}
cc=os.environ.get('CC','cc')
with tempfile.TemporaryDirectory(prefix='spu-decode-cache-') as temporary:
    work=Path(temporary)
    (work/'instrumented-spu-interp.c').write_text(prefix+source)
    binary=work/'test'
    args=[cc,'-std=gnu17','-O2','-UNDEBUG','-pthread','-I'+str(root/'include'),'-I'+str(spu),'-I'+str(work),str(spu/'tests/test_spu_decode_cache.c'),str(root/'runtime/platform/win32_compat.c'),'-lm','-o',str(binary)]
    if os.environ.get('UBSAN')=='1':args[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
    subprocess.run(args,check=True)
    outputs=[]
    for mode in ['0','1']:
        current=env|{'SPU_INTERP_DECODE_CACHE':mode}
        run=subprocess.run([str(binary)],env=current,cwd=work,capture_output=True,text=True,timeout=20)
        print('cache='+mode,run.stdout.strip(),run.stderr.strip(),flush=True)
        if run.returncode:raise SystemExit(run.returncode)
        outputs.append(run.stdout)
    assert outputs[0]==outputs[1],(outputs[0],outputs[1])
    print('PASS: cached/uncached architectural state identical; repeated words reuse decode')
