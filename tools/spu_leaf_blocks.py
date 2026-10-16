"""Experimental independent instruction blocks; no whole-image ABI classifiers.

Only the pure instruction lowering in spu_lifter is reused. Calls, services,
IRQ/state changes and all unknown translations remain on the original path.
"""
import hashlib,re
from spu_disasm import spu_decode,sign_extend
from spu_lifter import SPULifter,LiftedFunction
DIRECT={'br','bra','brz','brnz','brhz','brhnz'}
INDIRECT={'bi','biz','binz','bihz','bihnz'}
DENY={'brsl','brasl','bisl','bisled','iret','rdch','rchcnt','wrch','stop','stopd',
      'hgt','hlgt','heq','hgti','hlgti','heqi','sync','syncc','dsync',
      'fscrrd','fscrwr','mfspr','mtspr'}
def emit(raw,base,bounds,image,prefix):
 if not raw or len(raw)&3 or base&3 or base<0 or base+len(raw)>0x40000 or image<=0:
  raise ValueError('invalid code buffer/image')
 if not re.fullmatch(r'[A-Za-z_]\w*',prefix):raise ValueError('invalid symbol prefix')
 if not prefix.endswith('_'):prefix+='_'
 lifter=SPULifter();units=[];skipped=[];previous=base;definitions=[]
 for start,end in bounds:
  if start&3 or end&3 or not base<=start<end<0x40000 or end>base+len(raw) or start<previous:
   raise ValueError('invalid, overlapping or unsorted bounds')
  previous=end;ops=[];reason=None;f=LiftedFunction(start_addr=start,end_addr=end)
  def transfer(target):
   if start<=target<end:return f'goto leaf_pc_{target:08X};'
   return f'ctx->pc = 0x{target:X}u; g_spu_trampoline_fn = spu_indirect_branch; return;'
  for pc in range(start,end,4):
   w=int.from_bytes(raw[pc-base:pc-base+4],'big');d=spu_decode(w,pc);mn=d.mnemonic
   if mn in DENY:reason='control:'+mn;break
   if mn in DIRECT:
    displacement=sign_extend((w>>7)&0xffff,16)*4
    target=((0 if mn=='bra' else pc)+displacement)&0x3fffc
    if target==pc:reason='self_branch';break
    c=transfer(target)
    if mn not in ('br','bra'):
     r=w&127;expr=f'(ctx->gpr[{r}]._u32[0]'+(' & 0xffffu)'if mn in ('brhz','brhnz')else')')
     test='=='if mn in ('brz','brhz')else'!=';c=f'if ({expr} {test} 0) {{ {c} }}'
   elif mn in INDIRECT:
    if (w>>18)&3:reason='irq_control';break
    target=(w>>7)&127;c=f'ctx->pc = ctx->gpr[{target}]._u32[0] & 0x3fffcu; g_spu_trampoline_fn = spu_indirect_branch; return;'
    if mn!='bi':
     r=w&127;expr=f'(ctx->gpr[{r}]._u32[0]'+(' & 0xffffu)'if mn in ('bihz','bihnz')else')')
     test='=='if mn in ('biz','bihz')else'!=';c=f'if ({expr} {test} 0) {{ {c} }}'
   else:
    c=lifter._translate(d,f)
    if any(x in c for x in ('TODO','unsupported','SPU_RET','g_spu_trampoline','goto ','return;')):
     reason='unsupported:'+mn;break
    # Pure lowering may touch GPRs and private LS; it may not use other context state.
    rest=re.sub(r'ctx->gpr\[\d+\]','REG',c)
    rest=re.sub(r'spu_ls_(read|write)128\(ctx,','LS(',rest)
    if re.search(r'\bctx\b',rest):reason='state:'+mn;break
   ops.append((pc,c))
  if reason:skipped.append({'start':start,'end':end,'reason':reason});continue
  name=f'{prefix}block_{start:08X}';definitions.append(f'void {name}(spu_context* ctx) {{')
  definitions.append(f'    if (!spu_verified_leaf_enter(ctx,{image},0x{start:X}u,0x{end:X}u)) {{ ctx->pc=0x{start:X}u; g_spu_trampoline_fn=spu_indirect_branch; return; }}')
  for pc,c in ops:definitions.extend([f'leaf_pc_{pc:08X}:','    '+c])
  definitions.extend(['    '+transfer(end),'}'])
  units.append({'start':start,'end':end,'symbol':name,'sha256':hashlib.sha256(raw[start-base:end-base]).hexdigest()})
 header='#pragma once\n#include "spu_context.h"\n#include "spu_verified_overlay.h"\n'+f'int {prefix}spu_leaf_register(void);\n'
 source='#pragma STDC FP_CONTRACT OFF\n#include "spu_recomp.h"\n#include "spu_helpers.h"\n'+ '\n'.join(definitions)+'\n'
 if units:
  source+='static const spu_verified_leaf_unit units[]={\n'
  source+=''.join(f'    {{0x{u["start"]:X}u,0x{u["end"]:X}u,{u["symbol"]}}},\n'for u in units)+'};\n'
  source+=f'int {prefix}spu_leaf_register(void) {{ return spu_verified_leaf_register({image},units,sizeof(units)/sizeof(units[0])); }}\n'
 else:source+=f'int {prefix}spu_leaf_register(void) {{ return 1; }}\n'
 return header,source,{'schema':1,'image':image,'units':units,'skipped':skipped,
  'contract':'Raw ISA blocks with no guest calls/services/IRQ/state controls; exact complete bytes on every lookup and entry; no outside-code classifiers; original legacy admission retained.'}
