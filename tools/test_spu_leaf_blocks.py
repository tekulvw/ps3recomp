#!/usr/bin/env python3
"""Stateless raw-ISA block generation eligibility and control-flow contract."""
import sys,pathlib,struct,unittest
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
from spu_leaf_blocks import emit
class Leaf(unittest.TestCase):
 def build(self,words):return emit(struct.pack('>'+str(len(words))+'I',*words),0x10000,[(0x10000,0x10000+4*len(words))],701,'probe_')
 def test_raw_non_r0_return_without_classifier(self):
  h,c,m=self.build([0x1c004a14,0x35000300])
  self.assertEqual(len(m['units']),1);self.assertIn('spu_verified_leaf_enter',c)
  self.assertIn('ctx->gpr[6]._u32[0]',c);self.assertNotIn('SPU_RET',c)
  self.assertNotIn('host_depth',c);self.assertNotIn('spu_drain_call',c)
 def test_reject_calls_channels_and_irq_control(self):
  for w in (0x33000080,0x35200300,0x01a00b83,0x35040300,0x35400000,0x00002000):
   h,c,m=self.build([w]);self.assertFalse(m['units'],hex(w))
 def test_control_is_instruction_local(self):
  h,c,m=self.build([0x1cffc408,0x217fff88])
  self.assertEqual(len(m['units']),1);self.assertNotIn('goto leaf_pc_',c);self.assertIn('ctx->pc = 0x10000u',c)
  self.assertIn('ctx->pc = 0x10008u',c)
 def test_bad_bounds_fail_closed(self):
  for bounds in [[(0x10001,0x10008)],[(0x10000,0x10010)],[(0x10000,0x10000)]]:
   with self.assertRaises(ValueError):emit(b'\0'*8,0x10000,bounds,701,'probe_')
if __name__=='__main__':unittest.main()
