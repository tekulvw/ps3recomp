#!/usr/bin/env python3
from pathlib import Path
import os,subprocess,tempfile,sys,plistlib,shutil
r=Path(__file__).resolve().parents[1]
if sys.platform=='win32':
 print('SKIP: native OSK fixture requires a POSIX C toolchain');raise SystemExit(0)
gc='-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections'
with tempfile.TemporaryDirectory(prefix='native-osk-') as t:
 out=Path(t)/'fixture'
 src=[r/'tests/test_native_osk.c',r/'libs/misc/cellOskDialog.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c']
 extra=r/'libs/misc/osk_native.c'
 if extra.exists():src.append(extra)
 subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-ffunction-sections','-fdata-sections','-DPS3RECOMP_TEST_NATIVE_OSK','-I'+str(r/'include'),*map(str,src),'-o',str(out),'-pthread',gc],check=True)
 env=os.environ.copy();env.pop('PS3RECOMP_OSK_INITIAL_TEXT',None);env.pop('OSK_NATIVE_CONTROL_FILE',None)
 subprocess.run([str(out)],check=True,timeout=10,env=env)
 cases=[('ascii','PlayerRetail'),('unicode','A¢Ω€🚀'),('pair','A🚀B'),
        ('split','A🚀B'),('zero','A🚀B'),('empty',''),
        ('maximum','a'*510+'🚀B'),('unchanged',None)]
 # Invalid UTF-8 must leave the complete guest default intact, including when
 # the invalid suffix occurs past the field's limit. Hand-checked invalid forms:
 # stray continuation, overlong sequences, surrogate, out-of-range, truncation.
 invalid=[b'\x80',b'\xc0\xaf',b'\xe0\x80\xaf',b'\xf0\x80\x80\xaf',
          b'\xed\xa0\x80',b'\xf4\x90\x80\x80',b'\xf5\x80\x80\x80',
          b'\xe2\x82',b'\xf0\x9f\x9a',b'A'*40+b'\xff',b'\xc2A']
 cases.extend(('unchanged',os.fsdecode(value)) for value in invalid)
 for mode,value in cases:
  case_env=env.copy()
  if value is not None:case_env['PS3RECOMP_OSK_INITIAL_TEXT']=value
  subprocess.run([str(out),mode],check=True,timeout=10,env=case_env)

 for enabled in (False,True):
  control_env=env.copy();control_env['TEST_CONTROL_FILE']=str(Path(t)/'control')
  if enabled:control_env['OSK_NATIVE_CONTROL_FILE']=control_env['TEST_CONTROL_FILE']
  subprocess.run([str(out),'control' if enabled else 'control-off'],check=True,timeout=10,env=control_env)

 if sys.platform=='darwin' and '--mailbox-only' not in sys.argv:
  objects=[]
  for i,p in enumerate((r/'libs/misc/cellOskDialog.c',r/'libs/misc/osk_native.c',r/'libs/system/cellSysutil.c',r/'libs/system/cellSysutilNpEula.c')):
   o=Path(t)/f'ui-{i}.o';objects.append(str(o))
   subprocess.run(['cc','-std=gnu17','-O1','-UNDEBUG','-I'+str(r/'include'),'-c',str(p),'-o',str(o)],check=True)
  app=Path(t)/'OskFixture.app';(app/'Contents/MacOS').mkdir(parents=True)
  (app/'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':'ui','CFBundleIdentifier':'org.ps3recomp.tests.nativeosk','CFBundleName':'Native OSK fixture','CFBundlePackageType':'APPL','CFBundleVersion':'1'}))
  ui=app/'Contents/MacOS/ui'
  subprocess.run(['clang','-fobjc-arc','-O1','-UNDEBUG','-I'+str(r/'include'),str(r/'tests/test_native_osk_macos.m'),*objects,str(r/'libs/input/pad_macos.m'),str(r/'libs/video/rsx_texture_layout.c'),'-framework','Metal','-framework','QuartzCore','-framework','AppKit','-pthread','-Wl,-dead_strip','-o',str(ui)],check=True)
  other=Path(t)/'OtherFixture.app';(other/'Contents/MacOS').mkdir(parents=True)
  info=plistlib.loads((app/'Contents/Info.plist').read_bytes());info['CFBundleIdentifier']='org.ps3recomp.tests.nativeoskother';info['CFBundleName']='Other focus fixture'
  (other/'Contents/Info.plist').write_bytes(plistlib.dumps(info));shutil.copy2(ui,other/'Contents/MacOS/ui')
  for controlled in (False,True):
   label='controlled' if controlled else 'default'
   receipt=Path(t)/('ui-passed-'+label);log=Path(t)/('ui-'+label+'.log');errors=Path(t)/('ui-errors-'+label+'.log')
   # LaunchServices does not inherit the subprocess environment reliably; pass
   # each per-run control path explicitly. Empty is the default opt-out case.
   control_path=str(Path(t)/'ui-control') if controlled else ''
   subprocess.run(['/usr/bin/open','-W','-n','--env','OSK_FIXTURE_RECEIPT='+str(receipt),'--env','OSK_FIXTURE_OTHER_APP='+str(other),'--env','OSK_NATIVE_CONTROL_FILE='+control_path,'--stdout',str(log),'--stderr',str(errors),str(app)],check=True,timeout=30,env=env)
   print(label+' real AppKit fixture:',flush=True)
   print(log.read_text() if log.exists() else 'No AppKit fixture output',flush=True)
   print(errors.read_text() if errors.exists() else 'No AppKit fixture errors',flush=True)
   assert receipt.exists() and receipt.read_text()=='passed\n', 'real native UI fixture failed; see output above'
