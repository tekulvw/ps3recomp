#include <stdint.h>
#include <atomic>
#include <thread>
#include <vector>
#include "platform/win32_compat.h"
static std::atomic<unsigned> clocks,locks;static volatile LONG* watched;
static LONG counted(volatile LONG*p,LONG a,LONG b){if(p==watched)locks++;return InterlockedCompareExchange(p,a,b);}
#define InterlockedCompareExchange counted
static uint64_t tick=1000000;
static uint64_t now(){clocks++;return tick;}
#define AUDIO_CHRONOLOGY_TEST_CLOCK now
#include "diagnostics/audio_chronology_impl.h"
#include "diagnostics/ppu_call_span.h"
#include <cassert>
#include <cstring>
#include <unistd.h>
extern "C" void SetLastError(DWORD){}
#ifndef AUDIO_CHRONOLOGY_WAIT_NOTES_VERSION
static unsigned note_count(){return 0;}
#else
static unsigned note_count(){return s_ac_notes.count;}
#endif
static unsigned sleeps;static int sleep_rc;
static int fake_sleep(const timespec*ts,timespec*){sleeps++;tick+=(uint64_t)ts->tv_sec*1000000+ts->tv_nsec/1000+70;if(sleep_rc)errno=EINTR;return sleep_rc;}
#define nanosleep fake_sleep
static int actual_sleep(uint64_t usec){
/* ACTUAL_SLEEP */
return 0;
}
#undef nanosleep
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using s32=int32_t;
static constexpr u32 CELL_OK=0,CELL_SPURS_CORE_ERROR_NULL_POINTER=0x80410711,CELL_SPURS_CORE_ERROR_INVAL=0x80410702,CELL_SPURS_MAX_WORKLOAD=16,CELL_SPURS_MAX_SPU=8;
/* ACTUAL_CONSTANTS */
struct SpursWorkload{bool in_use;u64 data;u8 priority[8];void*pm;u32 sizePm,name_ea,minContention,maxContention;};
static SpursWorkload s_workloads[16];static u8 ram[4096],*vm_base=ram;
static void vm_write32(u32 a,u32 v){for(unsigned i=0;i<4;i++)ram[a+i]=(u8)(v>>(24-8*i));}
static void vm_write64(u32 a,u64 v){vm_write32(a,v>>32);vm_write32(a+4,v);}
static u32 vm_read32(u32 a){return (u32)ram[a]<<24|(u32)ram[a+1]<<16|(u32)ram[a+2]<<8|ram[a+3];}
/* ACTUAL_WORKLOAD */
static void arm(){FILE*f=fopen(getenv("AUDIO_CHRONOLOGY_TRIGGER"),"w");assert(f);fclose(f);tick+=250001;audio_chronology_poll();assert(s_ac.armed);}
int main(int argc,char**argv){assert(argc==2);const char*m=argv[1];watched=&s_ac.lock;audio_chronology_poll();ppu_context ctx={};ctx.thread_id=99;ctx.gpr[8]=2;
if(strcmp(m,"disabled")&&strcmp(m,"pretrigger"))arm();
bool quiet=!strcmp(m,"disabled")||!strcmp(m,"pretrigger")||!strcmp(m,"option-off")||!strcmp(m,"outside-span")||!strcmp(m,"postflush")||!strcmp(m,"invalid-option")||!strcmp(m,"no-target");
if(!strcmp(m,"postflush")){tick+=s_ac.duration_us+1;audio_chronology_poll();}
if(quiet){
 if(!strcmp(m,"option-off")){ppu_call_span_scope scope(&ctx,0x12000);unsigned c=clocks,l=locks;actual_sleep(1000);assert(clocks==c&&locks==l&&note_count()==0);scope.complete();puts("option-off quiet inside span");return 0;}
 unsigned c=clocks,l=locks;errno=EDOM;actual_sleep(1000);assert(errno==EDOM&&sleeps==1);assert(note_count()==0&&clocks==c&&locks==l);puts("quiet locks=0 clocks=0");return 0;}
{ppu_call_span_scope outer(&ctx,0x12000);
 if(!strcmp(m,"nested")){actual_sleep(1000);{ppu_call_span_scope inner(&ctx,0x12000);actual_sleep(2000);inner.complete();}actual_sleep(3000);}
 else if(!strcmp(m,"api")||!strcmp(m,"alias")){s_workloads[3].in_use=true;s_workloads[3].data=0x123456789abcULL;s_workloads[3].minContention=1;s_workloads[3].maxContention=5;ram[0x100+SPURS_WKL_CURCONT+3]=4;ram[0x100+SPURS_WKL_READY1+3]=7;ram[0x100+SPURS_WKL_IDLE2+3]=2;assert(cellSpursGetWorkloadInfo(0x100,3,0x300)==0);assert(ram[0x320]==4&&ram[0x323]==7&&ram[0x324]==2&&vm_read32(0x300)==0x1234);u8 before[sizeof ram];memcpy(before,ram,sizeof ram);assert(cellSpursGetWorkloadInfo(0,3,0x300)==(s32)CELL_SPURS_CORE_ERROR_NULL_POINTER);assert(cellSpursGetWorkloadInfo(0x100,16,0x300)==(s32)CELL_SPURS_CORE_ERROR_INVAL);assert(!memcmp(before,ram,sizeof ram));if(!strcmp(m,"alias"))assert(cellSpursGetWorkloadInfo(0x100,3,0x100)==0);u64 hash=1469598103934665603ULL;for(u8 b:ram)hash=(hash^b)*1099511628211ULL;printf("functional-ram=%llu\n",(unsigned long long)hash);}
 else if(!strcmp(m,"unwind")){try{ppu_call_span_scope inner(&ctx,0x12000);actual_sleep(1000);throw 7;}catch(int e){assert(e==7);}actual_sleep(1000);}
 else if(!strcmp(m,"cross-deadline")){actual_sleep(s_ac.duration_us+1);unsigned c=clocks,l=locks;actual_sleep(0);assert(clocks==c&&locks==l);}
 else if(!strcmp(m,"threads")){std::vector<std::thread> ts;for(unsigned n=0;n<8;n++)ts.emplace_back([n]{ppu_context local={};local.thread_id=100+n;for(unsigned j=0;j<32;j++){ppu_call_span_scope scope(&local,0x12000);
#ifdef AUDIO_CHRONOLOGY_WAIT_NOTES_VERSION
audio_chronology_workload(0,n,0,-1,0,0,0);
#endif
scope.complete();}});for(auto&t:ts)t.join();}
 else if(!strcmp(m,"expiry")){tick+=s_ac.duration_us+1;unsigned c=clocks,l=locks;actual_sleep(0);unsigned c2=clocks,l2=locks;actual_sleep(0);assert(c2<=c+1&&l2<=l+1&&clocks==c2&&locks==l2&&note_count()==0);}
 else if(!strcmp(m,"cap")){for(unsigned i=0;i<8;i++)actual_sleep(1000);}
 else {sleep_rc=!strcmp(m,"eintr")?-1:0;errno=EDOM;assert(actual_sleep(!strcmp(m,"zero")?0:1000)==0);assert(errno==(sleep_rc?EINTR:EDOM));}
 outer.complete();}
#if defined(AUDIO_CHRONOLOGY_WAIT_NOTES_VERSION) && !defined(WAIT_FUNCTIONAL_ONLY)
if(!strcmp(m,"nested")){assert(note_count()==6);assert(s_ac_notes.records[0].span==1&&s_ac_notes.records[2].span==2&&s_ac_notes.records[4].span==1);}
else if(!strcmp(m,"api")||!strcmp(m,"alias")){assert(note_count()==(!strcmp(m,"alias")?4u:3u));const auto&r=s_ac_notes.records[0];assert(r.kind==2&&r.phase==2&&r.arg0==0x100&&r.arg1==3&&r.arg2==0x300&&r.arg3==(4u|(7u<<8)|(2u<<16)));assert(s_ac_notes.records[1].rc==(s32)CELL_SPURS_CORE_ERROR_NULL_POINTER&&s_ac_notes.records[1].arg3==~(u64)0);}
else if(!strcmp(m,"unwind")){assert(note_count()==4&&s_ac_notes.records[0].span==2&&s_ac_notes.records[2].span==1);}
else if(!strcmp(m,"threads")){assert(note_count()==256);for(unsigned i=0;i<note_count();i++){const auto&r=s_ac_notes.records[i];assert(r.thread>=100&&r.thread<108&&r.span!=1);}}
else if(!strcmp(m,"cross-deadline")){assert(note_count()==1&&s_ac_notes.pending==1);}
else if(!strcmp(m,"expiry")){}
else if(!strcmp(m,"cap")){assert(note_count()==3&&s_ac_notes.dropped==7&&s_ac_notes.pending==1);}
else {assert(note_count()==2);const auto&a=s_ac_notes.records[0];const auto&b=s_ac_notes.records[1];assert(a.span==1&&a.run==s_ac.start_us&&a.ctx==(uint64_t)(uintptr_t)&ctx&&a.thread==99);assert(b.us-a.us==(!strcmp(m,"zero")?0:1070)&&b.rc==sleep_rc);assert(b.arg1==(!strcmp(m,"eintr")?EINTR:0));assert(s_ac_notes.pending==0);}
#elif !defined(WAIT_FUNCTIONAL_ONLY)
assert(note_count()>0&&"missing actual sleep/workload notes");
#endif
audio_chronology_finish();puts("wait-notes-pass");}
