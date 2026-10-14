#!/usr/bin/env python3
"""Source-derived actual fn+trampoline dispatcher span; synthetic host work only."""
from pathlib import Path
import argparse,os,subprocess,tempfile,json
ROOT=Path(__file__).resolve().parents[1]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--sanitize',action='store_true');ap.add_argument('--compiler',default='clang++');ap.add_argument('--dispatcher-source',type=Path);a=ap.parse_args()
 source=(a.dispatcher_source or ROOT/'runtime/ppu/ppu_loader.cpp').read_text();start=source.index('        { vcall_scope vcall_guard;')+len('        { vcall_scope vcall_guard;');end=source.index('        }\n        /* PPU_RETWATCH',start);body=source[start:end]
 fixture=(ROOT/'runtime/ppu/tests/test_ppu_call_spans.cpp').read_text().replace('/* ACTUAL_DISPATCH_BODY */',body)
 flags=['-std=c++17','-O2','-pthread','-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime')]
 if a.sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
 with tempfile.TemporaryDirectory(prefix='ppu-call-spans-')as td:
  t=Path(td);src=t/'actual.cpp';exe=t/'actual';src.write_text(fixture)
  q=subprocess.run([a.compiler,*flags,str(src),'-o',str(exe)],capture_output=True,text=True);assert q.returncode==0,q.stderr
  out=[]
  for mode in ['disabled','pretrigger','normal','nested','unwind','expired','postflush','wrong-target','cap','threads','cross-deadline','pending-flush','no-target','invalid-target','ten-second-cap']:
   env={k:v for k,v in os.environ.items()if not k.startswith('AUDIO_CHRONOLOGY')};env.update(AUDIO_CHRONOLOGY_TRIGGER=str(t/('gate-'+mode)))
   if mode!='disabled':env['AUDIO_CHRONOLOGY']='1'
   env['AUDIO_CHRONOLOGY_PPU_TARGET']='0x12000'
   if mode=='cap':env['AUDIO_CHRONOLOGY_LIMIT']='3'
   if mode=='no-target':env.pop('AUDIO_CHRONOLOGY_PPU_TARGET')
   if mode=='invalid-target':env['AUDIO_CHRONOLOGY_PPU_TARGET']='-4'
   if mode=='ten-second-cap':env['AUDIO_CHRONOLOGY_MS']='30000'
   q=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True);assert q.returncode==0,mode+'\n'+q.stdout+q.stderr
   out.append({'case':mode,'stdout':q.stdout.strip()})
  print(json.dumps({'status':'PASS','sanitize':a.sanitize,'cases':out},indent=2))
if __name__=='__main__':main()
