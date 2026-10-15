#include <atomic>
#include <thread>
#include <vector>
#include "platform/win32_compat.h"
static std::atomic<unsigned> clocks,locks;static volatile LONG*watched;
static LONG counted(volatile LONG*p,LONG a,LONG b){if(p==watched)locks++;return InterlockedCompareExchange(p,a,b);}
#define InterlockedCompareExchange counted
static uint64_t tick=1000000;static uint64_t now(){clocks++;return tick;}
#define AUDIO_CHRONOLOGY_TEST_CLOCK now
#include "diagnostics/audio_chronology_impl.h"
#include "diagnostics/ppu_call_span.h"
#include <cassert>
#include <cstring>
#include <cstdio>
extern "C" void SetLastError(DWORD){}
#ifndef AUDIO_CHRONOLOGY_SCALAR_NOTES_VERSION
static uint64_t audio_chronology_scalar_scope(){return 0;}
static uint64_t audio_chronology_scalar(uint64_t,uint32_t,uint64_t,uint64_t,uint64_t,uint64_t){return 0;}
#endif
static void arm(){FILE*f=fopen(getenv("AUDIO_CHRONOLOGY_TRIGGER"),"w");assert(f);fclose(f);tick+=250001;audio_chronology_poll();assert(s_ac.armed);}
static uint64_t send(){uint64_t t=audio_chronology_scalar(0,11,0,0xffffffffffffffffull,0x8000000000000000ull,7);return audio_chronology_scalar(t,12,1,2,3,4);}
int main(int argc,char**argv){assert(argc==2);const char*m=argv[1];watched=&s_ac.lock;audio_chronology_poll();ppu_context ctx={};ctx.thread_id=99;
if(strcmp(m,"disabled")&&strcmp(m,"pretrigger"))arm();
bool quiet=!strcmp(m,"disabled")||!strcmp(m,"pretrigger")||!strcmp(m,"option-off")||!strcmp(m,"outside")||!strcmp(m,"postflush")||!strcmp(m,"invalid-option")||!strcmp(m,"no-target")||!strcmp(m,"notes-off");
if(!strcmp(m,"postflush")){tick+=s_ac.duration_us+1;audio_chronology_poll();}
if(quiet){if(!strcmp(m,"outside")){unsigned c=clocks,l=locks;errno=EDOM;assert(audio_chronology_scalar_scope()==0&&send()==0);assert(clocks==c&&locks==l&&errno==EDOM&&s_ac_notes.count==0);}else{ppu_call_span_scope scope(&ctx,0x12000);unsigned c=clocks,l=locks;errno=EDOM;assert(audio_chronology_scalar_scope()==0&&send()==0);assert(clocks==c&&locks==l&&errno==EDOM&&s_ac_notes.count==0);scope.complete();}puts("quiet scalar paths PASS");return 0;}
{ppu_call_span_scope outer(&ctx,0x12000);uint64_t id=audio_chronology_scalar_scope();if(!id){fputs("missing selected scalar observation\n",stderr);return 1;}
 if(!strcmp(m,"nested")){send();{ppu_call_span_scope inner(&ctx,0x12000);assert(audio_chronology_scalar_scope()!=id);send();inner.complete();}assert(audio_chronology_scalar_scope()==id);send();assert(s_ac_notes.records[0].span==1&&s_ac_notes.records[2].span==2&&s_ac_notes.records[4].span==1);}
 else if(!strcmp(m,"unwind")){try{ppu_call_span_scope inner(&ctx,0x12000);send();throw 1;}catch(int){}assert(audio_chronology_scalar_scope()==id);send();}
 else if(!strcmp(m,"threads")){std::vector<std::thread>ts;for(unsigned n=0;n<8;n++)ts.emplace_back([n]{ppu_context local={};local.thread_id=100+n;ppu_call_span_scope scope(&local,0x12000);for(unsigned j=0;j<8;j++)send();scope.complete();});for(auto&t:ts)t.join();assert(s_ac_notes.count==128);}
 else if(!strcmp(m,"cap")){for(unsigned i=0;i<5;i++)send();assert(s_ac_notes.count==3&&s_ac_notes.dropped==7);}
 else if(!strcmp(m,"expiry")){tick+=s_ac.duration_us+1;unsigned cached_clocks=clocks;assert(audio_chronology_scalar_scope()==id&&clocks==cached_clocks);assert(send()==0);unsigned c=clocks,l=locks;assert(send()==0&&audio_chronology_scalar_scope()==0);assert(clocks==c&&locks==l&&s_ac_notes.count==0);}
 else {errno=EDOM;assert(send()>0&&errno==EDOM);assert(s_ac_notes.count==2);const auto&a=s_ac_notes.records[0];const auto&b=s_ac_notes.records[1];assert(a.kind==3&&b.kind==3&&a.phase==11&&b.phase==12&&a.rc==0&&a.token==b.token&&a.token);assert(a.span==id&&a.ctx==(uint64_t)(uintptr_t)&ctx&&a.thread==99);assert(a.arg0==0&&a.arg1==~0ull&&a.arg2==0x8000000000000000ull&&a.arg3==7);assert(b.arg0==1&&b.arg1==2&&b.arg2==3&&b.arg3==4);}
 outer.complete();}assert(audio_chronology_scalar_scope()==0);audio_chronology_finish();puts("scalar notes PASS");}
