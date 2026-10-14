#!/usr/bin/env python3
"""Strip only declared diagnostic additions; compare all original DMA source bytes."""
from pathlib import Path
import re, subprocess, hashlib
root=Path(__file__).resolve().parents[1]
base='a2c20108af45174dbb7d3e2f87f6acaaef09c6b4'
old=subprocess.check_output(['git','show',base+':runtime/spu/spu_dma.h'],cwd=root,text=True)
new=(root/'runtime/spu/spu_dma.h').read_text()
new=new.replace('#include "spu_dma_disposition.h"\n','')
new=new.replace('    spu_dma_disposition_token disposition;\n    spu_dma_disposition_begin(&disposition, lsa, ea, size, cmd);\n    unsigned copy_disposition = SPU_DMA_DISPOSITION_NONE;\n','')
new,n=re.subn(r'return spu_dma_disposition_finish\(&disposition, spu, lsa, ea, size, cmd, (-1|0), (?:SPU_DMA_DISPOSITION_[A-Z_]+|copy_disposition)\);',r'return \1;',new)
assert n==6,n
new,n1=re.subn(r'^ +spu_dma_disposition_(?:before|after)\([^\n]+\);\n','',new,flags=re.M)
assert n1==6,n1
new,n2=re.subn(r'^ +copy_disposition = SPU_DMA_DISPOSITION_(?:GET|PUT);\n','',new,flags=re.M)
assert n2==3,n2
assert new==old,'Unexpected original DMA source difference'
print('PASS: entire original DMA source byte-identical after stripping six return wrappers, six sample hooks, three outcome assignments, admission locals and one include; sha256='+hashlib.sha256(old.encode()).hexdigest())
