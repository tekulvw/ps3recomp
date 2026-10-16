import json,subprocess,tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 exe=Path(d)/'actual-dma';out=Path(d)/'capture.json'
 subprocess.run(['clang','-DPS3RECOMP_CRITICAL_WRITE_AVAILABLE=1','-std=gnu17','-O2','-UNDEBUG','-pthread','-I'+str(r/'include'),'-I'+str(r/'runtime/spu'),'-I'+str(r/'runtime/diagnostics'),str(r/'runtime/diagnostics/tests/test_critical_dma.c'),str(r/'runtime/diagnostics/critical_trace.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe),str(out)],check=True)
 e=json.loads(out.read_text())['events'];pub=[x for x in e if x['kind']==9];coherent=[x for x in pub if x['words'][6]==1];non=[x for x in pub if x['words'][6]==0]
 assert len(coherent)==1 and coherent[0]['words'][0]==0x12072 and coherent[0]['words'][1]==17
 assert coherent[0]['words'][2]>>16==105 and coherent[0]['words'][4]==42
 assert len(non)==8 and any(x['words'][0]==0x12072 and x['words'][1]==33 for x in non)
 print('PASS actual reserved/unreserved MFC PUT: guest result unchanged; context generation queried outside lock; coherent changed lane only; unreserved source receipts explicitly noncoherent')
