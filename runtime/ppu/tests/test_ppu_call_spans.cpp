#include <stdint.h>
#include <atomic>
#include <thread>
#include <vector>
#include "platform/win32_compat.h"
static std::atomic<unsigned> clocks,locks;static volatile LONG* watched;
static LONG counted(volatile LONG* p,LONG a,LONG b){if(p==watched)locks++;return InterlockedCompareExchange(p,a,b);}
#define InterlockedCompareExchange counted
static uint64_t tick=1000000;
static uint64_t now(){clocks++;return tick;}
#define AUDIO_CHRONOLOGY_TEST_CLOCK now
#include "diagnostics/audio_chronology_impl.h"
#include "ppu/ppu_context.h"
#if __has_include("diagnostics/ppu_call_span.h")
#include "diagnostics/ppu_call_span.h"
#endif
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#ifndef _WIN32
extern "C" void SetLastError(DWORD v){(void)v;}
#endif
static thread_local void(*g_trampoline_fn)(void*);
static void dispatch(ppu_context*ctx,uint32_t addr,void(*fn)(ppu_context*)){
const int _tf=0;const uint32_t _toc0=0;
/* ACTUAL_DISPATCH_BODY */
}
static unsigned work;
static void tail2(void*){tick+=300;work++;}
static void tail1(void*){tick+=200;work++;g_trampoline_fn=tail2;}
static void leaf(ppu_context*ctx){ctx->gpr[3]+=7;tick+=100;work++;g_trampoline_fn=tail1;}
static void nested(ppu_context*ctx){dispatch(ctx,0x12000,leaf);tick+=50;g_trampoline_fn=tail1;}
static void thrower(ppu_context*){tick+=123;throw 73;}
static void no_context(ppu_context*){work++;}
static void slow(ppu_context*ctx){ctx->gpr[3]+=7;tick+=s_ac.duration_us+1;}
static unsigned count(){
#ifdef AUDIO_CHRONOLOGY_CALL_SPAN_VERSION
 return s_ac_calls.count;
#else
 return 0;
#endif
}
static void arm(){FILE*f=fopen(getenv("AUDIO_CHRONOLOGY_TRIGGER"),"w");assert(f);fclose(f);tick+=250001;audio_chronology_poll();assert(s_ac.armed);}
int main(int argc,char**argv){assert(argc==2);watched=&s_ac.lock;audio_chronology_poll();const char*mode=argv[1];ppu_context ctx={};ctx.gpr[3]=5;ctx.gpr[5]=256;ctx.gpr[31]=0x23456;ctx.thread_id=0x100000007ULL;
 bool quiet=!strcmp(mode,"disabled")||!strcmp(mode,"pretrigger")||!strcmp(mode,"expired")||!strcmp(mode,"postflush")||!strcmp(mode,"wrong-target")||!strcmp(mode,"no-target")||!strcmp(mode,"invalid-target");
 if(strcmp(mode,"disabled")&&strcmp(mode,"pretrigger"))arm();
 if(!strcmp(mode,"expired")){tick+=s_ac.duration_us+1;}
 if(!strcmp(mode,"postflush")){tick+=s_ac.duration_us+1;audio_chronology_poll();}
 if(quiet){if(!strcmp(mode,"expired"))dispatch((ppu_context*)1,0x12000,no_context);unsigned cc=clocks,ll=locks;errno=EDOM;dispatch((ppu_context*)1,!strcmp(mode,"wrong-target")?0x12004:0x12000,no_context);assert(errno==EDOM);assert(clocks==cc&&locks==ll);assert(count()==0);puts("quiet clocks=0 locks=0 poison-context-safe");return 0;}
 errno=EDOM;
#ifdef AUDIO_CHRONOLOGY_CALL_SPAN_VERSION
 if(!strcmp(mode,"cross-deadline")){dispatch(&ctx,0x12000,slow);assert(count()==1&&s_ac_calls.pending==1);audio_chronology_poll();assert(s_ac.flushed);puts("boundary-end-not-fabricated");return 0;}
 if(!strcmp(mode,"pending-flush")){audio_chronology_call_token t;audio_chronology_call_begin(&t,&ctx,0x12000,0);tick+=s_ac.duration_us+1;audio_chronology_poll();unsigned cc=clocks,ll=locks;audio_chronology_call_end(&t,0);assert(clocks==cc&&locks==ll&&count()==1&&s_ac_calls.pending==1);puts("pending-end-quiet");return 0;}
 if(!strcmp(mode,"ten-second-cap"))assert(s_ac.duration_us==10000000);
#endif
 if(!strcmp(mode,"threads")) {
#ifdef AUDIO_CHRONOLOGY_CALL_SPAN_VERSION
  std::vector<std::thread> ts;for(unsigned n=0;n<8;n++)ts.emplace_back([n] { ppu_context local={};local.thread_id=0x100000000ull+n;for(unsigned j=0;j<64;j++){audio_chronology_call_token t;audio_chronology_call_begin(&t,&local,0x12000,0);audio_chronology_call_end(&t,0);} });
  for(auto& t:ts)t.join();assert(count()==1024&&s_ac_calls.pending==0&&!s_ac_calls.dropped);std::vector<uint64_t> ids;for(unsigned i=0;i<count();i++)if(!s_ac_calls.records[i].phase)ids.push_back(s_ac_calls.records[i].token);for(unsigned i=0;i<ids.size();i++)for(unsigned j=i+1;j<ids.size();j++)assert(ids[i]!=ids[j]);
#endif
  audio_chronology_finish();puts("concurrent-token-pass");return 0;
 }
 if(!strcmp(mode,"unwind")){bool caught=false;try{dispatch(&ctx,0x12000,thrower);}catch(int e){caught=e==73;}assert(caught);}
 else if(!strcmp(mode,"nested"))dispatch(&ctx,0x12000,nested);
 else if(!strcmp(mode,"cap")){for(int i=0;i<8;i++)dispatch(&ctx,0x12000,leaf);}
 else dispatch(&ctx,0x12000,leaf);
 assert(errno==EDOM);
#ifdef AUDIO_CHRONOLOGY_CALL_SPAN_VERSION
 if(!strcmp(mode,"cap")){assert(count()==3&&s_ac_calls.dropped>0);}
 else if(!strcmp(mode,"nested")){assert(count()==4);assert(s_ac_calls.records[0].phase==0&&s_ac_calls.records[1].depth==1);assert(s_ac_calls.records[2].token==s_ac_calls.records[1].token);assert(s_ac_calls.records[3].us-s_ac_calls.records[0].us==1150);}
 else{assert(count()==2);assert(s_ac_calls.records[1].us-s_ac_calls.records[0].us==(!strcmp(mode,"unwind")?123:600));assert(s_ac_calls.records[1].flags==(!strcmp(mode,"unwind")?1u:0u));assert(s_ac_calls.records[0].args[2]==256);assert(s_ac_calls.records[0].thread==0x100000007ULL);}
#else
 assert(count()==2&&"actual dispatcher missing start/end span records");
#endif
 if(strcmp(mode,"unwind")&&strcmp(mode,"cap"))assert(ctx.gpr[3]==12&&work==3+(!strcmp(mode,"nested")?2:0));
 audio_chronology_finish();puts("functional-span-pass");return 0;}
