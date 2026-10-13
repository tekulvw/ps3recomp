#!/usr/bin/env python3
"""Validate bounded buffer observation through real execution; no game data."""
import os
import subprocess
import tempfile
from pathlib import Path

if os.name == 'nt':
    print('SKIP: fixture currently requires the POSIX compiler/linker')
    raise SystemExit(0)
root = Path(__file__).resolve().parents[1]
spu = root / 'runtime/spu'
sources = ['spu_channels.c', 'spu_drain.c', 'spu_lockstep.c', 'spu_coherency.c',
           'spu_workload.c', 'spurs_policy.c', 'spurs_job.c', 'spu_tsp_weak.c',
           'spu_vm_pagemap.c', 'spurs_policy_blob_weak.c', 'spu_interp.c']
with tempfile.TemporaryDirectory(prefix='spu-pc-observer-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu17', '-O1', '-UNDEBUG', *os.environ.get('SPU_OBSERVER_TEST_CFLAGS','').split(),
                    '-I'+str(root/'include'), '-I'+str(spu),
                    '-I'+str(root/'runtime/platform'),
                    str(spu/'tests/test_spu_pc_observer.c'),
                    *[str(spu / name) for name in sources],
                    str(root/'runtime/platform/win32_compat.c'), '-pthread', '-lm',
                    '-o', str(binary)], check=True)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SPU_', 'SPURS_', 'YDKJ_', 'LBP_', 'GH3_'))}
    config = Path(tmp)/'observe.conf'
    import re
    base = "limit 8\nguard 30010 35002780\nidentity 3 6\n" + "".join(
        f"site {pc:X} {word:X} 3 4 1 5 6 2\n" for pc, word in [
            (0x30000,0x34000194),(0x30004,0xB2850A16),
            (0x30008,0x24000194),(0x3000C,0x40200000)]) + "end\n"
    def run(exe, mode='', text=base):
        if text is not None: config.write_text(text)
        current=dict(env,SPU_PC_OBSERVE=str(config)) if text is not None else env
        result=subprocess.run([str(exe)]+([mode] if mode else []), check=False,
                              capture_output=True,text=True,timeout=10,cwd=tmp,env=current)
        assert result.returncode==0, result.stdout+result.stderr
        records=[dict(re.findall(r"(\w+)=([^ ]+)",line)) for line in result.stderr.splitlines()
                 if line.startswith('[spu-pc-buffer]')]
        return result,records
    for mode in ('','zero'):
        result,records=run(binary,mode)
        assert len(records)==8, result.stderr
        contexts=dict(re.findall(r"context(\d+)=(\S+)",result.stdout))
        for i,r in enumerate(records):
            k=i//4
            assert r['ctx']==contexts[str(k)] and int(r['r0'],16)==0xABC000+k and int(r['r1'],16)==0xDEF000+k
            assert int(r['pc'],16)==0x30000+4*(i%4) and int(r['seq'])==i+1
            assert r['src']==r['dst']=='0x01000' and r['src_bytes']==r['dst_bytes']=='16'
            assert r['src_finite']==r['dst_finite']=='4'
            assert r['src_nonzero']==r['dst_nonzero']==('0' if mode else '4')
            assert r['id_r3']=='0x00001000' and r['id_r6']=='0x00000020'
    _,records=run(binary,text=None);assert not records
    # Shared cap includes actual microstep and fallback paths, also under contention.
    for mode in ('','threads'):
        result,records=run(binary,mode,base.replace('limit 8','limit 5'))
        assert len(records)==5 and sorted(int(r['seq']) for r in records)==list(range(1,6)),result.stderr
        contexts=dict(re.findall(r"context(\d+)=(\S+)",result.stdout))
        for r in records:
            k=int(r['r0'],16)-0xABC000
            assert r['ctx']==contexts[str(k)] and int(r['r1'],16)==0xDEF000+k
    safety=Path(tmp)/'safety'
    include=['-I'+str(root/'include'),'-I'+str(spu),'-I'+str(root/'runtime/platform')]
    for compiler, standard in [(os.environ.get('CC','cc'),'gnu17'),(os.environ.get('CXX','c++'),'c++17')]:
        subprocess.run([compiler,'-x','c++' if standard=='c++17' else 'c','-std='+standard,
                        '-O2','-UNDEBUG',*os.environ.get('SPU_OBSERVER_TEST_CFLAGS','').split(),*include,str(spu/'tests/test_spu_pc_observer_safety.c'),
                        '-pthread','-o',str(safety)],check=True)
        for mode in ('guard','bounds','nonmatch'):
            _,records=run(safety,mode); assert not records
        _,records=run(safety,'cap');assert len(records)==8
        _,records=run(safety,'stats')
        assert len(records)==3
        assert [records[0][f'src_{k}'] for k in ('words','finite','nonzero','nonfinite')]==['4','2','1','2']
        assert [records[1][f'src_{k}'] for k in ('bytes','words','finite','nonzero')]==['6','1','1','0']
        assert records[2]['src_peak']=='1' and records[2]['dst_words']=='0'
        maximum=base.replace('limit 8','limit 256').replace('identity 3 6','identity 0 1 2 3 4 5 6 7').replace('end\n', ''.join(f'guard {0x2000+i*4:X} 0\n' for i in range(31))+''.join(f'site {0x30100+i*4:X} 0 3 4 1 5 6 2\n' for i in range(4))+'end\n')
        _,records=run(safety,'nonmatch',maximum);assert not records
        malformed=['',base[:-4],base[:-1],base+'site 0 0 0 0 1 0 0 1\n',base+'x'*4097,
                   base.replace('30010','40000'),base.replace('30010','FFFFFFFF'),
                   base.replace('30010','30011'),base.replace('site 30000','site 40000'),
                   base.replace('site 30000','site 3FFFF'),base.replace('site 30000','site FFFFFFFF'),base.replace('limit 8','limit 257'),
                   base.replace('limit 8','limit -1'),base.replace('3 4 1','128 4 1'),
                   base.replace('3 4 1','3 4 0'),maximum.replace('end\n','guard 0 0\nend\n'),
                   maximum.replace('end\n','site 0 0 3 4 1 5 6 2\nend\n'),
                   base.replace('end\n','identity 1\nend\n'),base.replace('end\n','unknown\nend\n'),
                   base.replace('end\n','guard 30010 35002780\nend\n'),base.replace('end\n','\x00end\n'),base+'#'+'x'*160+'\n',
                   base.replace('limit 8','limit +8'),base.replace('limit 8','limit \v8')]
        for text in malformed:
            _,records=run(safety,'poison',text); assert not records
        result,records=run(safety,'poison',None); assert not records
        print(standard+': '+result.stdout.strip())
    objects=[]
    for compiler,source,standard,extra in [
        (os.environ.get('CC','cc'),'test_spu_pc_observer_safety.c','gnu17',['-DSPU_OBSERVER_LINKAGE']),
        (os.environ.get('CXX','c++'),'test_spu_pc_observer_linkage.cpp','c++17',[])]:
        obj=Path(tmp)/(source+'.o');objects.append(str(obj))
        subprocess.run([compiler,'-std='+standard,'-O2','-UNDEBUG',*extra,*include,
                        *os.environ.get('SPU_OBSERVER_TEST_CFLAGS','').split(),
                        '-c',str(spu/'tests'/source),'-o',str(obj)],check=True)
    subprocess.run([os.environ.get('CXX','c++'),*objects,'-pthread',
                    *os.environ.get('SPU_OBSERVER_TEST_CFLAGS','').split(),'-o',str(safety)],check=True)
    _,records=run(safety,'stats');assert len(records)==3
    print('observer: real paths, handoff, shared cap, identity, zero/nonzero, guards, bounds, trailing words, C/C++, disabled poison and initialization contention passed')
