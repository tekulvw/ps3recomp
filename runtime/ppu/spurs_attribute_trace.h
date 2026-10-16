/* Bounded optional observation of the original ninth stack argument. */
#pragma once
#include "spurs_workload_info_trace.h"
namespace {
struct SpursAttributeTrace {
 ppu_context *ctx=nullptr;uint64_t attr=0,data=0,sp=0,lr=0,arg9=0;unsigned id=0;
 explicit SpursAttributeTrace(uint32_t nid,ppu_context *c){
  if(nid!=0xEFEB2679u||!c)return;int saved=errno;
  static const bool enabled=[](){const char *e=getenv("PS3_SPURS_ATTRIBUTE_TRACE");return e&&!strcmp(e,"1");}();
  static std::atomic<unsigned> admitted{0};
  if(enabled){unsigned n=admitted.fetch_add(1,std::memory_order_relaxed);
   if(n<12&&SpursWorkloadInfoTrace::range(c->gpr[1]+0x70,8)&&SpursWorkloadInfoTrace::range(c->gpr[3],0x28)){
    ctx=c;attr=c->gpr[3];data=c->gpr[8];sp=c->gpr[1];lr=c->lr;id=n;
    for(unsigned i=0;i<8;i++)arg9=(arg9<<8)|vm_base[sp+0x70+i];
   }
  }errno=saved;
 }
 ~SpursAttributeTrace(){if(!ctx)return;int saved=errno;uint32_t actual=0;for(unsigned i=0;i<4;i++)actual=(actual<<8)|vm_base[attr+0x24+i];
  fprintf(stderr,"[spurs-attribute-request] id=%u attr=0x%llX data=0x%llX sp=0x%llX lr=0x%llX stack9_be64=0x%llX stack9_low32=%u written_max=%u result=0x%llX\n",id,(unsigned long long)attr,(unsigned long long)data,(unsigned long long)sp,(unsigned long long)lr,(unsigned long long)arg9,(uint32_t)arg9,actual,(unsigned long long)ctx->gpr[3]);errno=saved;
 }
};
}
