#!/usr/bin/env python3
"""Compile/run synthetic actual DMA and cellAudio chronology fixtures.

macOS author verification uses clang and SDL2 headers; all SDL/VM services in the
cellAudio fixture are inert, so no device, application or game is controlled.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]

def run(command,env=None,expected=0):
    result=subprocess.run(command,env=env,capture_output=True,text=True)
    if result.returncode!=expected:
        raise AssertionError(f'{command}: exit={result.returncode}\n{result.stdout}\n{result.stderr}')
    return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--sanitize',action='store_true');parser.add_argument('--sdl-include',default='/opt/homebrew/include');parser.add_argument('--compiler',default='clang');parser.add_argument('--cxx',default='clang++');args=parser.parse_args()
    cases=[]
    with tempfile.TemporaryDirectory(prefix='audio-chronology-tests-') as temp:
        out=Path(temp);flags=['-std=c11','-O2','-pthread','-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu')]
        if args.sanitize:flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        dma=out/'dma';audio=out/'audio'
        run([args.compiler,*flags,str(ROOT/'runtime/spu/tests/test_audio_chronology.c'),'-o',str(dma)])
        run([args.compiler,*flags,'-ffunction-sections','-fdata-sections','-I'+args.sdl_include,str(ROOT/'runtime/spu/tests/test_audio_chronology_cellAudio.c'),'-Wl,-dead_strip','-o',str(audio)])
        base=os.environ.copy()
        for key in list(base):
            if key.startswith('AUDIO_CHRONOLOGY')or key.startswith('SPU_DMA_DISPOSITION'):base.pop(key)
        for i,mode in enumerate(['disabled','pretrigger','normal','overlap','epoch','cap','threads']):
            env=base.copy();env['AUDIO_CHRONOLOGY_TRIGGER']=str(out/f'gate-{i}')
            if mode!='disabled':env['AUDIO_CHRONOLOGY']='1'
            if mode in ('cap','threads'):env['AUDIO_CHRONOLOGY_LIMIT']='32'
            result=run([str(dma),mode],env)
            if mode in ('disabled','pretrigger'):assert 'audio-chronology'not in result.stderr
            if mode in ('normal','cap','threads'):assert '[audio-chronology-summary]'in result.stderr
            cases.append(mode)
        for key,value in [('AUDIO_CHRONOLOGY','true'),('AUDIO_CHRONOLOGY_LIMIT','0'),('AUDIO_CHRONOLOGY_LIMIT','32769'),('AUDIO_CHRONOLOGY_MS','30001'),('AUDIO_CHRONOLOGY_MS','-1')]:
            env=base.copy();env.update(AUDIO_CHRONOLOGY='1',AUDIO_CHRONOLOGY_TRIGGER=str(out/'badgate'));env[key]=value
            result=run([str(dma),'disabled'],env);assert 'audio-chronology'not in result.stderr;cases.append('reject-'+key+'-'+value)
        disabled=run([str(audio)],base)
        env=base.copy();env.update(AUDIO_CHRONOLOGY='1',AUDIO_CHRONOLOGY_TRIGGER=str(out/'audio-gate'))
        enabled=run([str(audio)],env)
        assert enabled.stdout==disabled.stdout
        assert 'audio-chronology'not in disabled.stderr
        cases.append('actual-cellAudio-enabled-disabled-functional-equivalence')
        linkage=out/'linkage'
        cxxflags=[x if x!='-std=c11' else '-std=c++17' for x in flags]
        run([args.cxx,*cxxflags,str(ROOT/'runtime/spu/tests/test_audio_chronology_linkage.cpp'),'-o',str(linkage)])
        run([str(linkage)],base)
        cases.append('C++17-C-ABI-disabled-linkage')
        print(json.dumps(dict(status='PASS',sanitize=args.sanitize,cases=cases,functional_digest=enabled.stdout.strip()),indent=2))

if __name__=='__main__':main()
