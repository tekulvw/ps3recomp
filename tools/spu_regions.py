"""Connected physical regions over unchanged logical SPU translations.

Design reference: Yakuza-Dead-Souls-EX fd2cff9442dc6f5b977c6c4705dc483c060f1682
uses multi-entry native regions. This emitter retains this SDK's stronger logical
return classifier, exact guards, call drains and interrupt-sensitive dispatch.
"""
import re

def plan_regions(functions, cap_bytes=1024):
    if cap_bytes < 4 or cap_bytes > 0x40000 or cap_bytes & 3:
        raise ValueError('region byte cap must be aligned and within local store')
    regions=[]
    previous=0
    for f in functions:
        if (f.start_addr | f.end_addr) & 3 or not 0 <= f.start_addr < f.end_addr <= 0x40000 or f.start_addr < previous:
            raise ValueError('invalid, overlapping or unsorted logical bounds')
        join=(regions and f.region_eligible and regions[-1][-1].region_eligible
              and regions[-1][-1].end_addr==f.start_addr
              and f.end_addr-regions[-1][0].start_addr<=cap_bytes)
        if join:regions[-1].append(f)
        else:regions.append([f])
        previous=f.end_addr
    return regions

def emit_connected(lifter, header_name, preamble):
    if not lifter.verified_code_image or not lifter.verified_code_spans:
        raise ValueError('connected regions require exact body-span guards')
    groups=plan_regions(lifter.functions,lifter.region_bytes)
    lines=[preamble.format(header_name=header_name),'#include "spu_verified_overlay.h"','']
    records=[];entries=[];region_count=0
    def gate(f):
        return f'spu_verified_code_enter_span(ctx, {lifter.verified_code_image}, 0x{f.start_addr:X}u, 0x{f.end_addr:X}u)'
    def reject(pc):return f'ctx->pc = {pc}; g_spu_trampoline_fn = spu_indirect_branch; return;'
    for group in groups:
        first=group[0]
        if not first.region_eligible:
            lines.append(f'void {first.name}(spu_context* ctx) {{')
            lines.append(f'    if (!{gate(first)}) {{ {reject(f"0x{first.start_addr:X}u")} }}')
            lines.extend('    '+b for b in first.body_lines)
            lines.append('}')
            entries.append((first.start_addr,first.name))
            records.append({'start':first.start_addr,'end':first.end_addr,'kind':'legacy-sensitive-or-unsupported','entries':[first.start_addr]})
            continue
        region_count+=1
        region=f'{lifter.prefix}spu_region_{first.start_addr:08X}'
        dispatch=region+'_dispatch'
        body_by_start={f.start_addr:f for f in group}
        jump=re.compile(r'ctx->pc = 0x([0-9A-Fa-f]+)u?;\s*g_spu_trampoline_fn = ('+re.escape(lifter.prefix)+r'spu_func_[0-9A-Fa-f]+); return;')
        def localize(text):
            def replace(m):
                target=int(m[1],16)
                if target not in body_by_start:return m[0]
                return (f'ctx->pc = 0x{target:X}u; '
                        f'if (spu_region_local_step(ctx, {m[2]})) goto region_enter_{target:08X}; '
                        f'g_spu_trampoline_fn = {m[2]}; return;')
            return jump.sub(replace,text).replace("goto loc_", "goto region_pc_")
        lines.append(f'static void {region}(spu_context* ctx, uint32_t entry) {{')
        lines.append('    switch (entry) {')
        region_entries=[]
        for f in group:
            addresses=[pc for pc,_ in f.instruction_lines]
            if addresses != list(range(f.start_addr,f.end_addr,4)):
                raise ValueError('region instruction coverage differs from logical body')
            for pc in addresses:lines.append(f'    case 0x{pc:X}u:')
            lines.append(f'        if (!{gate(f)}) {{ {reject("entry")} }}')
            lines.append('        switch (entry) {')
            for pc in addresses:lines.append(f'        case 0x{pc:X}u: goto region_pc_{pc:08X};')
            lines.append('        }')
            lines.append('        break;')
            for pc in addresses:
                entries.append((pc,f.name if pc==f.start_addr else dispatch))
            region_entries.extend(addresses)
        lines.append(f'    default: {reject("entry")}')
        lines.append('    }')
        lines.append(f'    {reject("entry")}')
        for f in group:
            lines.append('    {')
            lines.append(f'region_enter_{f.start_addr:08X}:')
            lines.append(f'        if (!{gate(f)}) {{ {reject(f"0x{f.start_addr:X}u")} }}')
            for pc,body in f.instruction_lines:
                lines.append(f'region_pc_{pc:08X}:')
                lines.extend('        '+localize(b) for b in body)
            lines.extend('        '+localize(b) for b in f.region_suffix)
            # No accidental fall-through into the next logical body or a gap.
            lines.append('        return;')
            lines.append('    }')
        lines.append('}')
        lines.append(f'static void {dispatch}(spu_context* ctx) {{ {region}(ctx, (uint32_t)ctx->pc & SPU_LS_MASK); }}')
        for f in group:lines.append(f'void {f.name}(spu_context* ctx) {{ {region}(ctx, 0x{f.start_addr:X}u); }}')
        records.append({'start':first.start_addr,'end':group[-1].end_addr,'kind':'connected','logical_bodies':len(group),'entries':region_entries})
    defined={f.start_addr for f in lifter.functions}
    for target in sorted((lifter.call_targets|lifter.branch_targets)-defined):
        lines.append(f'void {lifter.prefix}spu_func_{target:08X}(spu_context* ctx) {{ {reject(f"0x{target:X}u")} }}')
    if len(entries)>65536 or len({pc for pc,_ in entries})!=len(entries):
        raise ValueError('invalid or excessive image entry census')
    lines.extend(['typedef struct { uint32_t addr; void (*func)(spu_context*); const char* name; } spu_func_entry;',
                  'static const spu_func_entry spu_function_table[] = {'])
    for pc,name in sorted(entries):lines.append(f'    {{ 0x{pc:08X}u, {name}, "{name}" }},')
    lines.extend(['    {0, NULL, NULL}','};',f'void {lifter.prefix}spu_recomp_register(void) {{',
                  '    for (const spu_func_entry* e=spu_function_table; e->func; ++e) spu_register_function(e->addr,e->func);','}'])
    lifter.region_manifest={'schema':1,'regions':region_count,'logical_bodies':len(lifter.functions),
                           'registered_entries':len(entries),'cap_bytes':lifter.region_bytes,'groups':records,
                           'contract':'Original logical translation/calls/returns; exact full-image indirect admission and complete logical-body guards; sensitive local edges return to original dispatcher.'}
    return '\n'.join(lines)+'\n'
