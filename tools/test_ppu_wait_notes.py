#!/usr/bin/env python3
"""Actual existing sleep region and entire workload-info function; no device/guest."""
from pathlib import Path
import argparse,subprocess,tempfile,os,json,re
ROOT=Path(__file__).resolve().parents[1]
def function(s,name):
 p=s.index('s32 '+name+'(');a=s.index('{',p);d=1;i=a+1
 while d:
  d+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[p:i]
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--sanitize',action='store_true');ap.add_argument('--functional-only',action='store_true');ap.add_argument('--source-root',type=Path,default=ROOT);a=ap.parse_args();r=a.source_root
 timer=(r/'runtime/syscalls/sys_timer.c').read_text();start=timer.index('#ifdef _WIN32',timer.index('/* POLLSITE:'));end=timer.index('\n    return CELL_OK;',start);start=timer.rfind('\n',0,start)+1
 # Include the new begin hook immediately preceding the unchanged platform branch.
 b=timer.rfind('    audio_chronology_wait_token ',timer.index('/* POLLSITE:'),start)
 if b>=0:start=b
 sleep=timer[start:end]
 spurs=(r/'libs/spurs/cellSpurs.c').read_text();constants='\n'.join('static constexpr u32 '+k+'='+re.search(r'\b'+k+r'\s*=\s*(0x[0-9A-Fa-f]+)',spurs)[1]+';'for k in ['SPURS_WKL_CURCONT','SPURS_WKL_READY1','SPURS_WKL_IDLE2','SPURS_WKL_SIGNAL1'])
 fixture=(ROOT/'runtime/ppu/tests/test_ppu_wait_notes.cpp').read_text().replace('/* ACTUAL_SLEEP */',sleep).replace('/* ACTUAL_CONSTANTS */',constants).replace('/* ACTUAL_WORKLOAD */',function(spurs,'cellSpursGetWorkloadInfo'))
 flags=['-std=c++17','-O2','-pthread','-I'+str(r/'include'),'-I'+str(r/'runtime')]
 if a.functional_only:flags+=['-DWAIT_FUNCTIONAL_ONLY']
 if a.sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
 with tempfile.TemporaryDirectory(prefix='ppu-wait-notes-')as td:
  t=Path(td);src=t/'actual.cpp';exe=t/'actual';src.write_text(fixture);q=subprocess.run(['clang++',*flags,str(src),'-o',str(exe)],capture_output=True,text=True);assert q.returncode==0,q.stderr
  results=[]
  modes=['disabled','pretrigger','option-off','outside-span','postflush','normal','nested','eintr','zero','api','cap','expiry','alias','unwind','cross-deadline','threads','invalid-option','no-target']
  if a.functional_only:modes=['api','alias','eintr','zero']
  for mode in modes:
   env={k:v for k,v in os.environ.items()if not k.startswith('AUDIO_CHRONOLOGY')};env.update(AUDIO_CHRONOLOGY_TRIGGER=str(t/('gate-'+mode)),AUDIO_CHRONOLOGY_PPU_TARGET='0x12000')
   if mode!='disabled':env['AUDIO_CHRONOLOGY']='1'
   if mode!='option-off':env['AUDIO_CHRONOLOGY_PPU_WAIT_NOTES']='1'
   if mode=='invalid-option':env['AUDIO_CHRONOLOGY_PPU_WAIT_NOTES']='true'
   if mode=='no-target':env.pop('AUDIO_CHRONOLOGY_PPU_TARGET')
   if mode=='cap':env['AUDIO_CHRONOLOGY_LIMIT']='3'
   q=subprocess.run([str(exe),mode],env=env,capture_output=True,text=True);assert q.returncode==0,mode+'\n'+q.stdout+q.stderr
   results.append({'case':mode,'stdout':q.stdout.strip()})
  print(json.dumps({'status':'PASS','sanitize':a.sanitize,'cases':results},indent=2))
if __name__=='__main__':main()
