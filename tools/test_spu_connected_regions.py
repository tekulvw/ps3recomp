#!/usr/bin/env python3
"""Neutral connected-region planning and executable emission regressions."""
import json,pathlib,struct,subprocess,sys,tempfile,unittest
from types import SimpleNamespace
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from spu_regions import plan_regions
ROOT=pathlib.Path(__file__).resolve().parents[1]
def body(a,b,supported=True):
 return SimpleNamespace(start_addr=a,end_addr=b,region_eligible=supported)
class Planning(unittest.TestCase):
 def test_join_adjacent_preserve_logical_bounds(self):
  fs=[body(0x1000,0x1008),body(0x1008,0x1010)]
  self.assertEqual(plan_regions(fs,1024),[[fs[0],fs[1]]])
  self.assertEqual([(f.start_addr,f.end_addr) for f in fs],[(0x1000,0x1008),(0x1008,0x1010)])
 def test_gap_never_becomes_code(self):
  fs=[body(0x1000,0x1008),body(0x1010,0x1018)]
  self.assertEqual(plan_regions(fs,1024),[[fs[0]],[fs[1]]])
 def test_cap_and_oversized_body(self):
  fs=[body(0x1000,0x1010),body(0x1010,0x1020),body(0x1020,0x1060)]
  self.assertEqual(plan_regions(fs,32),[fs[:2],[fs[2]]])
 def test_unsupported_body_isolated(self):
  fs=[body(0x1000,0x1008),body(0x1008,0x1010,False),body(0x1010,0x1018)]
  self.assertEqual(plan_regions(fs,1024),[[f] for f in fs])
 def test_bad_bounds_rejected(self):
  for fs in ([body(0x1001,0x1008)],[body(0x1000,0x1000)],[body(0x1000,0x1010),body(0x1008,0x1018)]):
   with self.assertRaises(ValueError):plan_regions(fs,1024)
class Emission(unittest.TestCase):
 def test_real_lifter_keeps_guards_aliases_and_middle_entry(self):
  with tempfile.TemporaryDirectory() as td:
   p=pathlib.Path(td)
   words=[0x40800007,0x40800188,0x1c004387,0x1cffc408,0x217fff08,0x35000000]
   (p/'program.bin').write_bytes(struct.pack('>6I',*words))
   (p/'bounds.json').write_text(json.dumps([{'start':hex(a),'end':hex(b)} for a,b in [(0x10000,0x10008),(0x10008,0x10010),(0x10010,0x10014),(0x10014,0x10018)]]))
   command=[sys.executable,str(ROOT/'tools/spu_lifter.py'),str(p/'program.bin'),'--base','0x10000','--functions',str(p/'bounds.json'),'--symbol-prefix','region_test_','--verified-code-image','701','--verified-code-spans','--connected-regions','--output',str(p/'out')]
   r=subprocess.run(command,capture_output=True,text=True);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   source=(p/'out/spu_recomp.c').read_text()
   self.assertIn('case 0x1000Cu:',source)
   self.assertIn('spu_region_local_step',source)
   self.assertIn('goto region_enter_00010008',source)
   for a in (0x10000,0x10008,0x10010,0x10014):
    self.assertIn(f'void region_test_spu_func_{a:08X}(spu_context* ctx)',source)
   c=p/'out/spu_recomp.c';c.write_text(source.replace('../../runtime/spu/',''))
   h=p/'out/spu_recomp.h';h.write_text(h.read_text().replace('../../runtime/spu/',''))
   compiled=subprocess.run(['cc','-std=gnu17','-Werror=implicit-function-declaration','-I'+str(ROOT/'include'),'-I'+str(ROOT/'runtime/spu'),'-c',str(c),'-o',str(p/'region.o')],capture_output=True,text=True)
   self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
   manifest=json.loads((p/'out/spu_regions.json').read_text())
   self.assertEqual(manifest['registered_entries'],6)
   self.assertEqual(manifest['logical_bodies'],4)
   self.assertEqual(manifest['regions'],1)
if __name__=='__main__':unittest.main()
