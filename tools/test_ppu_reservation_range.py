#!/usr/bin/env python3
"""Differentially test the production bulk helper against exact word invalidation."""
import os,re,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'runtime/ppu/ppu_loader.cpp').read_text()
assert 'void ppu_resv_break_store_range(' in source, 'missing bulk reservation invalidation API'
def function(name):
 m=re.search(r'extern "C" void '+name+r'\([^\n]+\)\s*\{',source); assert m,name
 start=m.start(); pos=m.end(); depth=1
 while depth:
  depth += (source[pos]=='{')-(source[pos]=='}');pos+=1
 return source[start:pos]
actual='''#include "ppu_context.h"
#include <cstdint>
#define PPU_RESV_MAX 128
#define PPU_RESV_INVALID 0x100000000ull
static ppu_context* g_resv_ctxs[PPU_RESV_MAX];
static long g_resv_ctx_n;
'''+function('ppu_resv_break_store')+'\n'+function('ppu_resv_break_store_range')+'''
extern "C" void set_registry(ppu_context** p,long n){g_resv_ctx_n=n;for(int i=0;i<PPU_RESV_MAX;i++)g_resv_ctxs[i]=p[i];}
'''
driver=r'''
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include "ppu_memory.h"
extern "C" void set_registry(ppu_context**,long);
int main(){
 std::array<ppu_context,128> old{},now{};std::array<ppu_context*,128> ptr{};
 std::mt19937_64 random(0x52455356);unsigned cases=0;
 auto check=[&](uint32_t ea,uint64_t bytes,long count){
  for(unsigned i=0;i<128;i++){uint64_t a;
   switch(i%8){case 0:a=uint64_t(ea)-4;break;case 1:a=ea;break;case 2:a=uint64_t(ea)+bytes-1;break;case 3:a=uint64_t(ea)+bytes;break;case 4:a=0x100000000ull;break;case 5:a=~uint64_t{0};break;default:a=random();}
   old[i].reserve_addr=now[i].reserve_addr=a;ptr[i]=(i%11==0)?nullptr:&old[i];}
  set_registry(ptr.data(),count);
  if(bytes&&bytes<=0x100000000ull-uint64_t(ea)){
   uint64_t last=(uint64_t(ea)+bytes-1)&~uint64_t{3};
   for(uint64_t w=ea&~uint32_t{3};w<=last;w+=4)ppu_resv_break_store(w);}
  for(unsigned i=0;i<128;i++)ptr[i]=(i%11==0)?nullptr:&now[i];
  set_registry(ptr.data(),count);ppu_resv_break_store_range(ea,bytes);
  for(unsigned i=0;i<128;i++)assert(old[i].reserve_addr==now[i].reserve_addr);
  cases++;
 };
 for(uint32_t ea: {0u,1u,2u,3u,4u,0x7cu,0x7fu,0x80u,0xfffffff8u,0xfffffffcu,0xffffffffu})
  for(uint64_t bytes: {0ull,1ull,2ull,3ull,4ull,5ull,127ull,128ull,129ull,16384ull,0x100000001ull,~0ull})
   for(long n: {0l,1l,9l,64l,128l,129l})check(ea,bytes,n);
 for(unsigned j=0;j<5000;j++)check(random(),random()%16385,random()%130);
 // Full-space range must invalidate every valid guest word without looping 2^30 times.
 for(unsigned i=0;i<128;i++){now[i].reserve_addr=i?uint64_t(i)*0x2010101:0xffffffffull;ptr[i]=&now[i];}
 set_registry(ptr.data(),128);ppu_resv_break_store_range(0,0x100000000ull);
 for(auto& c:now)assert(c.reserve_addr==0x100000000ull);
 printf("PASS %u differential cases plus full4GiB range, exact four-byte coverage, null/invalid slots, registry cap and no wrap\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='ppu-resv-range-') as tmp:
 p=Path(tmp);(p/'actual.cpp').write_text(actual);(p/'driver.cpp').write_text(driver)
 subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-O2','-I'+str(root/'runtime/ppu'),'-I'+str(root/'include'),str(p/'actual.cpp'),str(p/'driver.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,timeout=20)
