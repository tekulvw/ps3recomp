#!/usr/bin/env python3
"""Public-engine fixture companion: explicit cap validation and default retention.
Build test_producer_geometry.c normally, then pass its executable as argv1.
All programs, geometry and selectors in the C fixture are neutral synthetic data.
"""
import json,os,re,subprocess,sys,tempfile
from pathlib import Path
exe=Path(sys.argv[1]).resolve();results=[]
for value,want in [(None,32),('33',33),('256',256),('257',0),('0',0),('-1',0),('184467440737095516160',0),('33garbage',0)]:
 with tempfile.TemporaryDirectory(prefix='rsx-producer-cap-') as tmp:
  out=Path(tmp);trigger=out/'trigger'
  env={k:v for k,v in os.environ.items() if not k.startswith(('RSX_ENGINE_PRODUCER_CAPTURE','RSX_ENGINE_RASTER_PROBE'))}
  env.update(RSX_ENGINE_PRODUCER_CAPTURE_DIR=str(out),RSX_ENGINE_PRODUCER_CAPTURE_TARGET='0:00040000',RSX_ENGINE_PRODUCER_CAPTURE_TRIGGER=str(trigger))
  if value is not None:env['RSX_ENGINE_PRODUCER_CAPTURE_LIMIT']=value
  r=subprocess.run([str(exe),'census',str(trigger)],env=env,capture_output=True,text=True)
  assert r.returncode==0 and 'draws=303 texture_mask=0 GPU_sync_or_read=0' in r.stdout,(value,r.stdout,r.stderr)
  files=list(out.glob('producer-*.meta'));assert len(files)==want,(value,len(files),want)
  if not want:assert 'invalid configuration' in r.stderr
  for p in files:
   m=dict(re.findall(r'(\w+)=([^\s]+)',p.read_text()));assert m['frame']=='1' and m['arm_frame']=='0' and m['payload_saved']=='1' and m['status']=='complete'
   assert int(m['limit'])==(32 if value is None else int(value));assert int(m['total_reserved_bytes'])<=64*1024*1024
  results.append({'explicit_limit':value,'captures':len(files),'expected':want,'passed':True})
print(json.dumps({'checks':len(results),'failures':0,'results':results},indent=2))
