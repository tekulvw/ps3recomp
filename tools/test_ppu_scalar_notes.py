#!/usr/bin/env python3
"""Neutral existing-ring scalar observation tests; no guest/device/process control."""
from pathlib import Path
import argparse,tempfile,subprocess,os,json
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--sanitize',action='store_true');p.add_argument('--case');a=p.parse_args();r=a.source_root
 with tempfile.TemporaryDirectory(prefix='ppu-scalar-notes-')as td:
  d=Path(td);exe=d/'test';flags=['clang++','-std=c++17','-O2','-pthread','-I'+str(r/'include'),'-I'+str(r/'runtime')]
  if a.sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
  subprocess.run(flags+[str(ROOT/'runtime/ppu/tests/test_ppu_scalar_notes.cpp'),'-o',str(exe)],check=True)
  results=[]
  for mode in ([a.case]if a.case else ['disabled','pretrigger','option-off','outside','postflush','invalid-option','no-target','notes-off','normal','nested','unwind','threads','cap','expiry']):
   env={k:v for k,v in os.environ.items()if not k.startswith('AUDIO_CHRONOLOGY')};env.update(AUDIO_CHRONOLOGY='1',AUDIO_CHRONOLOGY_TRIGGER=str(d/('gate-'+mode)),AUDIO_CHRONOLOGY_PPU_TARGET='0x12000',AUDIO_CHRONOLOGY_PPU_WAIT_NOTES='1',AUDIO_CHRONOLOGY_PPU_SCALAR_NOTES='1')
   if mode=='disabled':env.pop('AUDIO_CHRONOLOGY')
   if mode=='option-off':env.pop('AUDIO_CHRONOLOGY_PPU_SCALAR_NOTES')
   if mode=='invalid-option':env['AUDIO_CHRONOLOGY_PPU_SCALAR_NOTES']='true'
   if mode=='no-target':env.pop('AUDIO_CHRONOLOGY_PPU_TARGET')
   if mode=='notes-off':env.pop('AUDIO_CHRONOLOGY_PPU_WAIT_NOTES')
   if mode=='cap':env['AUDIO_CHRONOLOGY_LIMIT']='3'
   q=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True);assert q.returncode==0,mode+'\n'+q.stdout+q.stderr;results.append({'case':mode,'stdout':q.stdout.strip()})
  print(json.dumps({'status':'PASS','sanitize':a.sanitize,'cases':results},indent=2))
if __name__=='__main__':main()
