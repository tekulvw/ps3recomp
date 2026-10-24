#!/usr/bin/env python3
"""Clock-controlled actual cellPad HLE; no live game or physical controller."""
import argparse,json,os,pathlib,shlex,subprocess,tempfile
r=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--output-dir',type=pathlib.Path,required=True);a=p.parse_args()
o=a.output_dir.resolve();o.mkdir(parents=True,exist_ok=True)
cmd=['cc','-std=gnu17','-O2','-UNDEBUG','-ffunction-sections','-fdata-sections','-Wl,-dead_strip','-I'+str(r/'include'),'-I'+subprocess.check_output(['sdl2-config','--prefix'],text=True).strip()+'/include',*shlex.split(subprocess.check_output(['sdl2-config','--cflags'],text=True)),str(r/'libs/input/tests/test_pad_file_reports.c'),*shlex.split(subprocess.check_output(['sdl2-config','--libs'],text=True)),'-o',str(o/'fixture')]
(o/'build-argv.json').write_text(json.dumps(cmd,indent=2)+'\n')
b=subprocess.run(cmd,capture_output=True,text=True,timeout=30)
(o/'build.stdout').write_text(b.stdout);(o/'build.stderr').write_text(b.stderr)
assert not b.returncode,b.stdout+b.stderr
env={k:v for k,v in os.environ.items() if not k.startswith(('PAD_','YDKJ_'))}
ok=True
for mode in ('drains','default','ports-clear','physical','pressure-sensor','always','other-inputs','analog'):
    with tempfile.TemporaryDirectory(prefix='pad-reports-') as d:
        argv=[str(o/'fixture'),mode,str(pathlib.Path(d)/'pad.txt')]
        (o/(mode+'.argv.json')).write_text(json.dumps(argv)+'\n')
        x=subprocess.run(argv,env=env,capture_output=True,text=True,timeout=5)
        (o/(mode+'.stdout')).write_text(x.stdout);(o/(mode+'.stderr')).write_text(x.stderr)
        print(mode,x.returncode,x.stdout.strip(),flush=True);ok=ok and not x.returncode
if not ok:raise SystemExit(1)
