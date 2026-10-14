"""Actual-source observer identities; neutral data, no GPU or title inputs."""
import json,os,pathlib,subprocess,sys,tempfile
exe=pathlib.Path(sys.argv[1]).resolve();results=[]
for case in ['normal','recycle-fp','recycle-vp','early','prefetch','invalid-vp','invalid-fp','valid-then-early','inline-constant','inactive-poison','max-vp','max-fp']:
 with tempfile.TemporaryDirectory(prefix='draw-program-identity-')as td:
  p=pathlib.Path(td);env={k:v for k,v in os.environ.items()if not k.startswith(('RSX_ENGINE_DRAW_OBSERVER','RSX_ENGINE_PRODUCER_CAPTURE','RSX_ENGINE_RASTER_PROBE'))};env.update(RSX_ENGINE_DRAW_OBSERVER=str(p/'rows.jsonl'),RSX_ENGINE_DRAW_OBSERVER_MAX='4096')
  if case=='inactive-poison':env.pop('RSX_ENGINE_DRAW_OBSERVER')
  q=subprocess.run([str(exe),case],env=env,capture_output=True,text=True);assert q.returncode==0,(case,q.stdout,q.stderr)
  if case=='inactive-poison':
   assert not (p/'rows.jsonl').exists() and 'reader_calls=0' in q.stdout;results.append({'case':case,'rows':0,'stdout':q.stdout.strip()});continue
  rows=[json.loads(l)for l in (p/'rows.jsonl').read_text().splitlines()];assert len(rows)==(2 if case.startswith('recycle')or case in ['valid-then-early','inline-constant']else 1)
  for r in rows:
   assert all(k in r for k in ['program_identity_observed','vp_content_valid','vp_content_hash','vp_instructions','fp_structure_valid','fp_structural_hash','fp_bytes'])
  a=rows[0]
  if case in ['early','prefetch']:assert a['program_identity_observed']==0 and not a['vp_content_valid']and not a['fp_structure_valid']and a['vp_instructions']==a['fp_bytes']==0 and 'fp_reads=0'in q.stdout
  else:
   assert a['program_identity_observed']==1
   assert a['vp_content_valid']==(case!='invalid-vp')and a['fp_structure_valid']==(case!='invalid-fp')
   if a['vp_content_valid']:assert a['vp_instructions']==(544 if case=='max-vp'else 1)
   if a['fp_structure_valid']:assert a['fp_bytes']==(65536 if case=='max-fp'else 32 if case=='inline-constant'else 16)
  if case=='recycle-fp':assert rows[0]['fp_program']==rows[1]['fp_program']and rows[0]['fp_structural_hash']!=rows[1]['fp_structural_hash']and rows[0]['vp_content_hash']==rows[1]['vp_content_hash']
  if case=='recycle-vp':assert rows[0]['vp_start']==rows[1]['vp_start']and rows[0]['vp_content_hash']!=rows[1]['vp_content_hash']and rows[0]['fp_structural_hash']==rows[1]['fp_structural_hash']
  if case=='valid-then-early':assert rows[1]['program_identity_observed']==rows[1]['fp_structure_valid']==rows[1]['vp_content_valid']==0 and rows[1]['fp_bytes']==rows[1]['vp_instructions']==0
  if case=='inline-constant':assert rows[0]['fp_structural_hash']==rows[1]['fp_structural_hash'] and rows[0]['fp_bytes']==rows[1]['fp_bytes']==32
  results.append({'case':case,'rows':len(rows),'stdout':q.stdout.strip()})
print(json.dumps({'checks':len(results),'failures':0,'results':results},indent=2))
