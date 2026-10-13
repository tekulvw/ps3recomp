#!/usr/bin/env python3
"""Source-free synthetic per-overlay guard/quota regression, C and C++."""
import os,re,subprocess,tempfile
from pathlib import Path
if os.name=='nt':
    print('SKIP: fixture requires POSIX compiler/linker');raise SystemExit(0)
root=Path(__file__).resolve().parents[1];spu=root/'runtime/spu'
flags=os.environ.get('SPU_OBSERVER_TEST_CFLAGS','').split()
base='limit 12\nidentity 7\nguard 30010 ABCDEF00\n'+''.join(f'group {g} 4\ngroupguard {g} 30100 {0xAAA0+g:X}\ngroupsite {g} {0x30000+(g-1)*4:X} 40200000 3 4 1 5 6 2\n' for g in range(1,4))+'end\n'
with tempfile.TemporaryDirectory(prefix='spu-pc-groups-') as t:
    t=Path(t);config=t/'config';compat=t/'compat.o';binary=t/'test'
    subprocess.run([os.environ.get('CC','cc'),'-std=gnu17','-O1',*flags,'-c',str(root/'runtime/platform/win32_compat.c'),'-o',str(compat)],check=True)
    def run(mode,text):
        config.write_text(text);env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_'))};env['SPU_PC_OBSERVE']=str(config)
        p=subprocess.run([str(binary),mode],capture_output=True,text=True,env=env,timeout=10)
        assert p.returncode==0,p.stdout+p.stderr
        return [dict(re.findall(r'(\w+)=([^ ]+)',s)) for s in p.stderr.splitlines() if s.startswith('[spu-pc-buffer]')]
    for compiler,std in [(os.environ.get('CC','cc'),'gnu17'),(os.environ.get('CXX','c++'),'c++17')]:
        subprocess.run([compiler,'-x','c' if std=='gnu17' else 'c++','-std='+std,'-O2','-UNDEBUG',*flags,'-I'+str(spu),'-I'+str(root/'include'),str(spu/'tests/test_spu_pc_observer_groups.c'),'-x','none',str(compat),'-pthread','-o',str(binary)],check=True)
        for mode in ('normal','threads'):
            r=run(mode,base);assert len(r)==12
            assert sorted(int(x['seq']) for x in r)==list(range(1,13))
            for g in range(1,4):
                subset=[x for x in r if int(x['group'])==g];assert len(subset)==4
                assert sorted(int(x['group_seq']) for x in subset)==list(range(1,5))
                assert all(x['src_nonzero']==x['dst_nonzero']=='4' for x in subset)
                assert all(x['src']==x['dst']=='0x01000' and x['src_bytes']==x['dst_bytes']=='16' for x in subset)
        r=run('guards',base);assert len(r)==1 and r[0]['group_seq']=='1'
        alias='limit 4\ngroup 1 2\ngroup 2 2\ngroupguard 1 30100 AAA1\ngroupguard 2 30100 AAA2\ngroupsite 1 30000 40200000 3 4 1 5 6 2\ngroupsite 2 30000 40200000 3 4 1 5 6 2\nend\n'
        r=run('alias',alias);assert [x['group'] for x in r]==['1','1','2','2']
        invalid=[base.replace('limit 12','limit 11'),base.replace('limit 12','limit 13'),base.replace('group 1 4','group 1 0'),base.replace('group 1 4','group 0 4'),base.replace('group 1 4','group 9 4'),base.replace('group 1 4','group 1 4\ngroup 1 4'),base.replace('group 1 4\n',''),base.replace('groupguard 1 30100 AAA1\n',''),base.replace('groupguard 1 30100 AAA1','groupguard 1 30100 AAA1\ngroupguard 1 30100 AAA1'),base.replace('groupsite 1 30000','site 30000'),base.replace('groupguard 1 30100','groupguard 1 30101'),base.replace('groupguard 1 30100','groupguard 1 40000'),base.replace('groupsite 1 30000','groupsite 1 30001'),base.replace('end\n','group 4 1\nend\n'),base.replace('end\n','groupsite 1 30000 40200000 3 4 1 5 6 2\nend\n')]
        # Per-group guard32/site8/file4096 limits: test complete oversized inputs.
        invalid += [base.replace('end\n',''.join(f'groupguard 1 {0x30200+i*4:X} 0\n' for i in range(32))+'end\n'),base.replace('end\n',''.join(f'groupsite 1 {0x31000+i*4:X} 40200000 3 4 1 5 6 2\n' for i in range(6))+'end\n'),base+'#'+'x'*4096+'\n']
        # Eight groups/sites and32 own guards fit under the unchanged file cap.
        maximum='limit 256\n'+''.join(f'group {g} 32\ngroupguard {g} 30100 {0xAAA0+g:X}\ngroupsite {g} {0x30000+(g-1)*4:X} 40200000 3 4 1 5 6 2\n' for g in range(1,9))+'end\n'
        assert len(maximum.encode())<4096
        assert not run('parse',maximum)
        own32=base.replace('end\n',''.join(f'groupguard 1 {0x30200+i*4:X} 0\n' for i in range(31))+'end\n')
        assert not run('parse',own32)
        invalid.append(maximum.replace('group 8 32','group 8 33'))
        for text in invalid:assert not run('invalid',text)
        print(std+': groups/quotas/interleaving/concurrency/overlay alias/guard conjunction/poisoned capped group/bounds/transparency/rejections PASS')
