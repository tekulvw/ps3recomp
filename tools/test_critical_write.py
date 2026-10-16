import json,subprocess,tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 for mode in ('optimized','ubsan'):
  exe=Path(d)/mode;out=Path(d)/(mode+'.json')
  flags=['-DPS3RECOMP_CRITICAL_WRITE_AVAILABLE=1','-std=c17','-O2','-Wall','-Wextra','-Werror','-pthread','-I'+str(r/'runtime/diagnostics')]
  if mode=='ubsan':flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
  subprocess.run(['clang',*flags,str(r/'runtime/diagnostics/critical_trace.c'),str(r/'runtime/diagnostics/tests/test_critical_write.c'),'-o',str(exe)],check=True)
  subprocess.run([str(exe),str(out)],check=True)
  e=json.loads(out.read_text())['events'];publications=[x for x in e if x['kind']==9]
  assert len(publications)==1 and publications[0]['words'][0]==0x155ed572 and publications[0]['words'][1]==17
  assert publications[0]['words'][6]==1 and publications[0]['words'][4]==0x80000001
  print(mode,'PASS: dynamic predicate binding; bounded lane intersection; committed change only; failed conditional emits nothing; finalization disarms watch')
