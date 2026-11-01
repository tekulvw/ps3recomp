#!/usr/bin/env python3
"""Native Windows actual SPU registry diagnostics; no game code or execution."""
import argparse,os,re,shutil,subprocess,tempfile
from pathlib import Path

def rows(text):
    return [dict(re.findall(r'(\w+)=([^ ]+)',line)) for line in text.splitlines() if line.startswith('[spu-registry] ')]

def main():
    if os.name!='nt': print('SKIP: native Windows SPU registry fixture'); return 0
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--runtime-library',type=Path,required=True)
    parser.add_argument('--case',choices=('all','basic'),default='all')
    args=parser.parse_args();root=Path(__file__).resolve().parents[1];lib=args.runtime_library.resolve()
    compiler=shutil.which(os.environ.get('CC','clang-cl'));assert compiler and lib.is_file()
    with tempfile.TemporaryDirectory(prefix='spu-registry-') as temp:
        work=Path(temp);objects=[]
        for i,source in enumerate((root/'runtime/spu/tests/test_spu_registry_trace.c',root/'runtime/host/host_stubs.c')):
            obj=work/(str(i)+'.obj')
            subprocess.run([compiler,'/nologo','/std:c17','/O1','/Gy','/Gw','/MT','/UNDEBUG','/D_CRT_SECURE_NO_WARNINGS',
                '/I'+str(root/'include'),'/I'+str(root/'runtime/spu'),'/c',str(source),'/Fo'+str(obj)],cwd=work,check=True,timeout=60)
            objects.append(str(obj))
        exe=work/'fixture.exe'
        libs=('gdi32','user32','ws2_32','winmm','synchronization','bcrypt','psapi','dbghelp')
        subprocess.run([compiler,'/nologo',*objects,str(lib),'/Fe'+str(exe),'/link','/OPT:REF',*(v+'.lib' for v in libs)],cwd=work,check=True,timeout=60)
        base={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','PS3_','MAG_','YDKJ_'))}
        for value in (None,'0','2'):
            env=dict(base)
            if value is not None:env['SPU_REGISTRY_TRACE']=value
            result=subprocess.run([str(exe),'basic'],cwd=work,env=env,check=True,capture_output=True,text=True,timeout=10)
            assert not rows(result.stderr),result.stderr
            print('PASS default/exact-1 silence',value,flush=True)
        cases=('basic',) if args.case=='basic' else ('basic','duplicate','reject','function-capacity','region-capacity','limit','mismatch','null','many-images')
        for case in cases:
            result=subprocess.run([str(exe),case],cwd=work,env=dict(base,SPU_REGISTRY_TRACE='1'),check=True,capture_output=True,text=True,timeout=30)
            records=rows(result.stderr);batches=[r for r in records if r['event']=='batch'];regions=[r for r in records if r['event']=='region']
            assert records,'Missing enabled actual registry trace: '+case
            if case=='basic':
                assert len(regions)==1 and regions[0]['result']=='inserted' and regions[0]['functions']=='2',records
                assert len(batches)==1 and batches[0]['functions']=='2' and batches[0]['regions']=='1',records
            elif case=='duplicate':
                assert len(regions)==2 and regions[1]['duplicate_region']=='1',records
                assert batches[0]['functions']=='2' and batches[0]['duplicates']=='1',records
            elif case=='reject':
                assert len(regions)==2 and all(r['result']=='rejected' for r in regions),records
                assert batches[0]['regions']=='0' and batches[0]['region_rejections']=='2',records
            elif case=='function-capacity':
                assert batches[0]['functions']=='262144' and batches[0]['function_overflows']=='1',records
            elif case=='region-capacity':
                assert len(regions)==129 and regions[-1]['result']=='rejected' and regions[-1]['reason']=='capacity',records
                assert batches[0]['regions']=='128' and batches[0]['region_rejections']=='1',records
            elif case=='limit':
                assert len(records)==513 and records[-1]['event']=='limit',len(records)
            elif case=='mismatch':
                assert regions[0]['batch_matches']=='0' and regions[0]['functions']=='unknown',records
            elif case=='null':assert batches[0]['null_functions']=='1',records
            elif case=='many-images':
                assert {int(r['image']) for r in regions}==set(range(1000,1030)),records
                assert all(r['result']=='inserted' and r['functions']=='2' and r['batch_matches']=='1' for r in regions),records
                assert len(batches)==31 and all(r['function_overflows']=='0' for r in batches),records
            print('PASS actual registry trace',case,flush=True)
    return 0
if __name__=='__main__':raise SystemExit(main())
