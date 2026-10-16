"""Bounded trace storage and disabled/overflow/concurrent publication fixture."""
import json,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 for mode in ('optimized','ubsan'):
  exe=Path(d)/mode; output=Path(d)/(mode+'.json')
  cmd=['clang','-std=c17','-pthread','-Wall','-Wextra','-Werror','-O2','-I'+str(root/'runtime/diagnostics'),str(root/'runtime/diagnostics/critical_trace.c'),str(root/'runtime/diagnostics/tests/test_critical_trace.c'),'-o',str(exe)]
  if mode=='ubsan':cmd[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
  subprocess.run(cmd,check=True);subprocess.run([str(exe),str(output)],check=True)
  data=json.loads(output.read_text());assert data['capture_id']==2 and len(data['events'])==1
  overflow=json.loads(Path(str(output)+'.1').read_text());assert len(overflow['events'])==262144 and overflow['dropped']==57856
  assert all(e['words'][1]==((~e['words'][0])&((1<<64)-1)) for e in overflow['events'])
  print(mode,'PASS: disabled errno; concurrent publication; capacity; overflow; reuse')
