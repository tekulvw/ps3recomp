#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "critical_trace.h"
#define PS3_CRITICAL_WRITE_IMPLEMENTATION
#include "critical_write_trace.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sched.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#define CAPACITY 262144u
typedef struct {uint64_t ns,thread,owner,span,words[8];uint32_t kind,count;} Event;
static Event events[CAPACITY];
static _Atomic unsigned enabled,writers,reserved,dropped;
static _Atomic uint64_t capture,finalized_capture;
static uint64_t started,duration;
uint64_t ps3_critical_watch_range;
uint32_t ps3_critical_arrival_pc=0xFFFFFFFFu;
void (*ps3_critical_arrival_callback)(void*,uint32_t,uint32_t);
static uint32_t watch_container,watch_array;
static _Atomic uint64_t write_sequence;
static _Atomic uint64_t watch_snapshot;
static pthread_mutex_t control_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t clock_once=PTHREAD_ONCE_INIT;
#ifdef __APPLE__
static mach_timebase_info_data_t timebase;
static void init_clock(void){mach_timebase_info(&timebase);}
#else
static void init_clock(void){}
#endif
uint64_t ps3_critical_now_ns(void){pthread_once(&clock_once,init_clock);
#ifdef __APPLE__
 return (uint64_t)(((__uint128_t)mach_absolute_time()*timebase.numer)/timebase.denom);
#else
 struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000000+ts.tv_nsec;
#endif
}
int ps3_critical_enabled(void){return atomic_load_explicit(&enabled,memory_order_acquire)!=0;}
uint64_t ps3_critical_capture_id(void){return atomic_load_explicit(&capture,memory_order_acquire);}
uint64_t ps3_critical_finalized_capture_id(void){return atomic_load_explicit(&finalized_capture,memory_order_acquire);}
void ps3_critical_emit(uint32_t kind,uint64_t owner,uint64_t span,const uint64_t *words,uint32_t count){
 if(!ps3_critical_enabled())return;
 ps3_critical_emit_capture(ps3_critical_capture_id(),kind,owner,span,words,count);
}
void ps3_critical_emit_capture(uint64_t expected,uint32_t kind,uint64_t owner,uint64_t span,const uint64_t *words,uint32_t count){
 if(!ps3_critical_enabled())return;
 int saved=errno;
 atomic_fetch_add_explicit(&writers,1,memory_order_seq_cst);
 if(!atomic_load_explicit(&enabled,memory_order_seq_cst)||expected!=atomic_load_explicit(&capture,memory_order_acquire)){atomic_fetch_sub_explicit(&writers,1,memory_order_seq_cst);errno=saved;return;}
 unsigned slot=atomic_fetch_add_explicit(&reserved,1,memory_order_relaxed);
 if(slot<CAPACITY){Event *e=events+slot;e->ns=ps3_critical_now_ns();
#ifdef __APPLE__
 uint64_t tid=0;pthread_threadid_np(NULL,&tid);e->thread=tid;
#else
 e->thread=(uint64_t)(uintptr_t)pthread_self();
#endif
 e->kind=kind;e->owner=owner;e->span=span;e->count=count>8?8:count;
 memset(e->words,0,sizeof(e->words));if(e->count)memcpy(e->words,words,e->count*sizeof(uint64_t));
 }else atomic_fetch_add_explicit(&dropped,1,memory_order_relaxed);
 atomic_fetch_sub_explicit(&writers,1,memory_order_seq_cst);errno=saved;
}
int ps3_critical_begin(uint64_t id,uint64_t ns){int saved=errno;pthread_mutex_lock(&control_lock);if(ps3_critical_enabled()){pthread_mutex_unlock(&control_lock);return -1;}while(atomic_load(&writers))sched_yield();atomic_store(&reserved,0);atomic_store(&dropped,0);atomic_store(&capture,id);const char *wc=getenv("PS3RECOMP_CRITICAL_WATCH_CONTAINER"),*wa=getenv("PS3RECOMP_CRITICAL_WATCH_ARRAY");watch_container=wc?(uint32_t)strtoul(wc,NULL,16):0;watch_array=wa?(uint32_t)strtoul(wa,NULL,0):0;__atomic_store_n(&ps3_critical_watch_range,0,__ATOMIC_RELEASE);atomic_store(&watch_snapshot,0);started=ps3_critical_now_ns();duration=ns;atomic_store_explicit(&enabled,1,memory_order_seq_cst);pthread_mutex_unlock(&control_lock);errno=saved;return 0;}
int ps3_critical_finish(const char *path){int saved=errno;pthread_mutex_lock(&control_lock);atomic_store_explicit(&enabled,0,memory_order_seq_cst);while(atomic_load_explicit(&writers,memory_order_seq_cst))sched_yield();__atomic_store_n(&ps3_critical_watch_range,0,__ATOMIC_RELEASE);uint64_t ended=ps3_critical_now_ns();FILE *f=fopen(path,"w");if(!f){pthread_mutex_unlock(&control_lock);return -1;}unsigned n=atomic_load(&reserved);if(n>CAPACITY)n=CAPACITY;
 fprintf(f,"{\"schema\":1,\"clock\":\"monotonic_ns\",\"capture_id\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"requested_duration_ns\":%llu,\"dropped\":%u,\"unpublished\":0,\"events\":[",(unsigned long long)atomic_load(&capture),(unsigned long long)started,(unsigned long long)ended,(unsigned long long)duration,atomic_load(&dropped));
 for(unsigned i=0;i<n;i++){Event *e=events+i;fprintf(f,"%s{\"id\":%u,\"ns\":%llu,\"kind\":%u,\"thread\":%llu,\"owner\":%llu,\"span\":%llu,\"words\":[",i?",":"",i,(unsigned long long)e->ns,e->kind,(unsigned long long)e->thread,(unsigned long long)e->owner,(unsigned long long)e->span);for(unsigned j=0;j<e->count;j++)fprintf(f,"%s%llu",j?",":"",(unsigned long long)e->words[j]);fputs("]}",f);}fputs("]}\n",f);int failed=ferror(f);int rc=fclose(f);if(failed)rc=-1;if(!rc)atomic_store_explicit(&finalized_capture,atomic_load(&capture),memory_order_release);pthread_mutex_unlock(&control_lock);errno=saved;return rc;}
static void pause_ms(unsigned ms){struct timespec ts={ms/1000,(long)(ms%1000)*1000000};nanosleep(&ts,NULL);}
__attribute__((weak)) int ps3_spu_atomic_observe_control_request(const char* line){(void)line;return 0;}
static void *control_main(void *arg){char *path=arg;uint64_t last=0;for(;;){FILE *f=fopen(path,"r");if(f){char request[4096];if(fgets(request,sizeof(request),f)&&(!strncmp(request,"spu-atomic",10))){int complete=strchr(request,'\n')!=NULL||feof(f);fclose(f);if(complete)ps3_spu_atomic_observe_control_request(request);pause_ms(50);continue;}rewind(f);}unsigned long long id=0,ms=0;char output[2048];int got=f?fscanf(f,"%llu %llu %2047s",&id,&ms,output):0;if(f)fclose(f);if(got==3 && id && id!=last && ms && ms<=30000){last=id;if(!ps3_critical_begin(id,ms*1000000)){while(ps3_critical_now_ns()-started<ms*1000000)pause_ms(10);ps3_critical_finish(output);fprintf(stderr,"[critical trace] capture=%llu complete output=%s\n",id,output);}}pause_ms(50);}return NULL;}
void ps3_critical_control_start(void){const char *p=getenv("PS3RECOMP_CRITICAL_TRACE_CONTROL");if(!p||!*p)return;pthread_t t;char *copy=strdup(p);if(copy && !pthread_create(&t,NULL,control_main,copy))pthread_detach(t);else free(copy);}

static uint16_t critical_be16(const unsigned char *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}
void ps3_critical_watch_bind(uint64_t id,uint64_t owner,uint32_t container,uint32_t array,uint32_t base){
 if(!ps3_critical_enabled())return;
 atomic_fetch_add_explicit(&writers,1,memory_order_seq_cst);
 if(atomic_load_explicit(&enabled,memory_order_seq_cst)&&id==ps3_critical_capture_id()&&watch_container&&container==watch_container&&array==watch_array&&base<=0xfffffff0u){
  uint64_t next=((uint64_t)(base+16)<<32)|base;
  uint64_t old=__atomic_exchange_n(&ps3_critical_watch_range,next,__ATOMIC_ACQ_REL);
  if(old!=next){atomic_store(&watch_snapshot,0);uint64_t w[4]={container,array,base,old};ps3_critical_emit_capture(id,11,owner,0,w,4);}
 }
 atomic_fetch_sub_explicit(&writers,1,memory_order_seq_cst);
}
void ps3_critical_write_begin(ps3_critical_write_token *t,uint64_t owner,uint64_t ea,uint32_t size,const void *destination,const void *source,uint32_t kind,uint32_t cmd,uint32_t pc,uint32_t link,uint32_t generation,uint32_t coherent){
 t->capture=0;if(!ps3_critical_enabled()||!ps3_critical_write_watched(ea,size))return;
 uint64_t range=__atomic_load_n(&ps3_critical_watch_range,__ATOMIC_ACQUIRE);uint64_t lo=(uint32_t)range,hi=range>>32;
 if(lo<ea)lo=ea;if(hi>ea+size)hi=ea+size;lo=(lo+1)&~1ull;hi&=~1ull;if(lo>=hi)return;
 t->capture=ps3_critical_capture_id();t->owner=owner;t->span=atomic_fetch_add(&write_sequence,1)+1;t->ea=ea;t->size=size;t->kind=kind;t->cmd=cmd;t->pc=pc;t->link=link;t->generation=generation;t->coherent=coherent;t->offset=(uint32_t)(lo-ea);t->count=(uint32_t)((hi-lo)/2);if(t->count>8)t->count=8;
 for(unsigned i=0;i<t->count;i++){unsigned at=t->offset+i*2;t->before[i]=coherent?critical_be16((const unsigned char*)destination+at):0;t->source[i]=critical_be16((const unsigned char*)source+at);}
 if(coherent&&t->count==8&&atomic_exchange(&watch_snapshot,t->capture)!=t->capture){for(unsigned i=0;i<8;i++){uint64_t w[8]={t->ea+t->offset+i*2,((uint64_t)t->before[i]<<32)|t->before[i],t->cmd,((uint64_t)t->pc<<32)|t->link,t->generation,((uint64_t)t->offset<<32)|t->size,1,t->ea};ps3_critical_emit_capture(t->capture,12,t->owner,t->span,w,8);}}
}
void ps3_critical_write_end(const ps3_critical_write_token *t,const void *destination){
 if(!t->capture||!ps3_critical_enabled()||t->capture!=ps3_critical_capture_id())return;
 for(unsigned i=0;i<t->count;i++){uint16_t after=t->coherent?critical_be16((const unsigned char*)destination+t->offset+i*2):t->source[i];if(t->coherent&&after==t->before[i])continue;
 uint64_t w[8]={t->ea+t->offset+i*2,((uint64_t)t->before[i]<<32)|after,t->cmd,((uint64_t)t->pc<<32)|t->link,t->generation,((uint64_t)t->offset<<32)|t->size,t->coherent,((uint64_t)t->source[i]<<32)|t->ea};ps3_critical_emit_capture(t->capture,t->kind,t->owner,t->span,w,8);}
}

struct spu_context;
extern uint32_t spurs_policy_service_identity(const struct spu_context*);
uint32_t ps3_critical_context_generation(const void *ctx){return spurs_policy_service_identity((const struct spu_context*)ctx);}
