#!/usr/bin/env python3
"""Standalone observer transparency, returned-byte and concurrency regression."""
from pathlib import Path
import argparse,os,re,subprocess,tempfile,json,hashlib
ROOT=Path(__file__).resolve().parents[1]
HEADER=ROOT/'runtime/ppu/spurs_workload_info_trace.h'
def body_at(s,key):
 start=s.index(key);brace=s.index('{',start);depth=1;i=brace+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]
PRE=r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include "ppu_context.h"
extern "C" { uint32_t ppu_vm_size=0x20000;uint8_t* vm_base; }
using u64=uint64_t;using u32=uint32_t;using u8=uint8_t;using s32=int32_t;
constexpr unsigned CELL_SPURS_MAX_WORKLOAD=16,CELL_SPURS_MAX_SPU=8;
constexpr int CELL_OK=0,CELL_SPURS_CORE_ERROR_NULL_POINTER=0x80410702,CELL_SPURS_CORE_ERROR_INVAL=0x80410704;
static void vm_write32(u32 ea,u32 v){assert(ea+4<=ppu_vm_size);vm_base[ea]=v>>24;vm_base[ea+1]=v>>16;vm_base[ea+2]=v>>8;vm_base[ea+3]=v;}
static void vm_write64(u32 ea,u64 v){vm_write32(ea,v>>32);vm_write32(ea+4,v);}
static u32 vm_read32(u32 ea){assert(ea+4<=ppu_vm_size);return u32(vm_base[ea])<<24|u32(vm_base[ea+1])<<16|u32(vm_base[ea+2])<<8|vm_base[ea+3];}
'''
MAIN=r'''
static void original_call(ppu_context* c){c->gpr[3]=uint64_t(int64_t(cellSpursGetWorkloadInfo(c->gpr[3],u32(c->gpr[4]),c->gpr[5])));}
static void observed_call(ppu_context* c){SpursWorkloadInfoTrace t(0x4E153E3E,c);original_call(c);}
int main(int argc,char**argv){const char*mode=argv[1];
 if(!strcmp(mode,"disabled")||!strcmp(mode,"othernid")){auto p=mmap(nullptr,ppu_vm_size,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0);assert(p!=MAP_FAILED);vm_base=(uint8_t*)p;for(int i=0;i<10000;i++){errno=EDOM;SpursWorkloadInfoTrace t(!strcmp(mode,"othernid")?42:0x4E153E3E,(ppu_context*)p);assert(errno==EDOM);}munmap(p,ppu_vm_size);return 0;}
 vm_base=(uint8_t*)malloc(ppu_vm_size);memset(vm_base,0x55,ppu_vm_size);u32 se=0x1000,info=0x4000;unsigned wid=2;
 s_workloads[wid].in_use=1;s_workloads[wid].data=0x0102030405060708;s_workloads[wid].pm=(void*)0x8888;s_workloads[wid].sizePm=0x80;s_workloads[wid].minContention=1;s_workloads[wid].maxContention=6;s_workloads[wid].name_ea=0x9999;memset(s_workloads[wid].priority,3,8);
 vm_base[se+wid]=0;vm_base[se+0x20+wid]=1;vm_base[se+0x10+wid]=0;vm_write32(se+0x70,0x00008000);vm_write32(se+0x6c,0xFFFFFFFF);vm_base[se+0x77]=0xFF;
 ppu_context c;memset(&c,0xAB,sizeof c);c.gpr[3]=se;c.gpr[4]=wid;c.gpr[5]=info;c.lr=0x123456;c.thread_id=9;
 if(!strcmp(mode,"error"))c.gpr[4]=16;
 if(!strcmp(mode,"invalid"))c.gpr[3]=ppu_vm_size-16;
 if(!strcmp(mode,"wide"))c.gpr[3]=(uint64_t(1)<<32)+se;
 if(!strcmp(mode,"badinfo"))c.gpr[5]=ppu_vm_size-8;
 if(!strcmp(mode,"invalid")||!strcmp(mode,"wide")||!strcmp(mode,"badinfo")){auto before=c;std::vector<uint8_t> old(vm_base,vm_base+ppu_vm_size);errno=EDOM;{SpursWorkloadInfoTrace t(0x4E153E3E,&c);c.gpr[3]=0;}before.gpr[3]=0;assert(!memcmp(&c,&before,sizeof c));assert(!memcmp(old.data(),vm_base,ppu_vm_size));assert(errno==EDOM);return 0;}
 if(!strcmp(mode,"concurrent")){std::vector<std::thread> threads;for(int k=0;k<16;k++){vm_base[info+64*k+0x20]=k;vm_base[info+64*k+0x23]=k+1;}std::vector<uint8_t> old(vm_base,vm_base+ppu_vm_size);for(int k=0;k<16;k++)threads.emplace_back([&,k]{ppu_context x=c;x.gpr[4]=wid;x.gpr[5]=info+64*k;x.thread_id=100+k;auto expected=x;expected.gpr[3]=0;for(int n=0;n<8;n++){x.gpr[3]=se;errno=EDOM;{SpursWorkloadInfoTrace t(0x4E153E3E,&x);x.gpr[3]=0;}assert(errno==EDOM);assert(!memcmp(&x,&expected,sizeof x));}});for(auto&t:threads)t.join();assert(!memcmp(old.data(),vm_base,ppu_vm_size));return 0;}
 unsigned char old_workloads[sizeof s_workloads];memcpy(old_workloads,s_workloads,sizeof s_workloads);auto base_ctx=c;std::vector<uint8_t> initial(vm_base,vm_base+ppu_vm_size);errno=EDOM;original_call(&base_ctx);assert(errno==EDOM);std::vector<uint8_t> expected(vm_base,vm_base+ppu_vm_size);memcpy(vm_base,initial.data(),ppu_vm_size);
 errno=EDOM;observed_call(&c);assert(errno==EDOM);assert(!memcmp(&base_ctx,&c,sizeof c));assert(!memcmp(expected.data(),vm_base,ppu_vm_size));
 if(!strcmp(mode,"cap"))for(int n=0;n<20;n++){c.gpr[3]=se;observed_call(&c);}
 assert(!memcmp(old_workloads,s_workloads,sizeof s_workloads));free(vm_base);return 0;}
'''
def run(red=False):
 sdk=(ROOT/'libs/spurs/cellSpurs.c').read_text();typ=sdk[sdk.index('typedef struct {'):sdk.index('} SpursWorkload;')+len('} SpursWorkload;')];enum=sdk[sdk.index('enum {\n    SPURS_WKL_READY1'):sdk.index('\n};',sdk.index('enum {\n    SPURS_WKL_READY1'))+3]
 body=body_at(sdk,'s32 cellSpursGetWorkloadInfo(')
 with tempfile.TemporaryDirectory(prefix='spurs-info-tests-') as d:
  d=Path(d);helper='struct SpursWorkloadInfoTrace {SpursWorkloadInfoTrace(unsigned,ppu_context*){}};' if red else HEADER.read_text().replace('#pragma once','')
  src=PRE+helper+'\n'+typ+'\nstatic SpursWorkload s_workloads[CELL_SPURS_MAX_WORKLOAD];\n'+enum+'\n'+body+'\n'+MAIN;(d/'test.cpp').write_text(src)
  subprocess.run(['clang++','-std=c++17','-O1','-fsanitize=undefined','-fno-sanitize-recover=all','-pthread','-I'+str(ROOT/'runtime/ppu'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
  results={}
  modes=['enabled','disabled','othernid','error','invalid','wide','badinfo','filtered','lrfiltered','spursfiltered','unarmed','armed','cap','concurrent']
  for mode in modes:
   env=os.environ.copy();env['PS3_SPURS_INFO_TRACE']='0' if mode=='disabled' else '1';env['PS3_SPURS_INFO_TRACE_LIMIT']='64' if mode=='concurrent' else '3'
   if mode=='filtered':env['PS3_SPURS_INFO_TRACE_WID']='7'
   if mode=='lrfiltered':env['PS3_SPURS_INFO_TRACE_LR_MIN']='123457'
   if mode=='spursfiltered':env['PS3_SPURS_INFO_TRACE_SPURS']='1001'
   if mode in ('armed','unarmed'):
    env['PS3_SPURS_INFO_TRACE_ARM_FILE']=str(d/'arm');
    if mode=='armed':(d/'arm').write_text('1')
   p=subprocess.run([str(d/'test'),mode],env=env,text=True,capture_output=True);assert p.returncode==0,(mode,p.stderr)
   lines=p.stderr.splitlines();events=[dict(re.findall(r'(\w+)=([^ ]+)',x)) for x in lines if x.startswith('[spurs-info-trace] ')]
   expected=0 if mode in ['disabled','othernid','filtered','lrfiltered','spursfiltered','unarmed'] else 3 if mode=='cap' else 64 if mode=='concurrent' else 1
   assert len(events)==expected,(mode,'event count',len(events),expected,p.stderr)
   assert len(events)==len(lines),(mode,'non-atomic records',lines)
   if mode in ['enabled','armed','cap']:
    for e in events:assert e['ready']=='0' and e['contention']=='1' and e['min']=='1' and e['max']=='6' and e['entry_lr']=='00123456' and e['tid']=='9' and e['result']=='00000000',e
   if mode=='error':assert events[0]['info_valid']=='0',events
   if mode in ['invalid','wide']:assert events[0]['request_valid']=='0',events
   if mode=='badinfo':assert events[0]['info_valid']=='0',events
   if mode=='concurrent':
    assert {int(e['id']) for e in events}==set(range(64));assert all(100<=int(e['tid'])<116 for e in events);assert all(e['entry_lr']=='00123456' and e['wid']=='2' for e in events);assert all(int(e['contention'])==int(e['tid'])-100 and int(e['ready'])==int(e['tid'])-99 and e['result']=='00000000' for e in events)
   results[mode]={'exit':p.returncode,'events':len(events),'stderr_sha256':hashlib.sha256(p.stderr.encode()).hexdigest()}
  return {'modes':results,'get_info_body_sha256':hashlib.sha256(body.encode()).hexdigest(),'ubsan':True,'asan':'not run; earlier local runtime unavailable'}
if __name__=='__main__':
 a=argparse.ArgumentParser();a.add_argument('--red',action='store_true');a.add_argument('--receipt',type=Path);args=a.parse_args();r=run(args.red)
 if args.receipt:args.receipt.write_text(json.dumps(r,indent=2)+'\n')
 print(json.dumps(r,indent=2))
