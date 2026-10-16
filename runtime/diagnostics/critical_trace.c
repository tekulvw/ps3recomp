#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "critical_trace.h"
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
static _Atomic uint64_t capture;
static uint64_t started,duration;
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
int ps3_critical_begin(uint64_t id,uint64_t ns){int saved=errno;pthread_mutex_lock(&control_lock);if(ps3_critical_enabled()){pthread_mutex_unlock(&control_lock);return -1;}while(atomic_load(&writers))sched_yield();atomic_store(&reserved,0);atomic_store(&dropped,0);atomic_store(&capture,id);started=ps3_critical_now_ns();duration=ns;atomic_store_explicit(&enabled,1,memory_order_seq_cst);pthread_mutex_unlock(&control_lock);errno=saved;return 0;}
int ps3_critical_finish(const char *path){int saved=errno;pthread_mutex_lock(&control_lock);atomic_store_explicit(&enabled,0,memory_order_seq_cst);while(atomic_load_explicit(&writers,memory_order_seq_cst))sched_yield();uint64_t ended=ps3_critical_now_ns();FILE *f=fopen(path,"w");if(!f){pthread_mutex_unlock(&control_lock);return -1;}unsigned n=atomic_load(&reserved);if(n>CAPACITY)n=CAPACITY;
 fprintf(f,"{\"schema\":1,\"clock\":\"monotonic_ns\",\"capture_id\":%llu,\"start_ns\":%llu,\"end_ns\":%llu,\"requested_duration_ns\":%llu,\"dropped\":%u,\"unpublished\":0,\"events\":[",(unsigned long long)atomic_load(&capture),(unsigned long long)started,(unsigned long long)ended,(unsigned long long)duration,atomic_load(&dropped));
 for(unsigned i=0;i<n;i++){Event *e=events+i;fprintf(f,"%s{\"id\":%u,\"ns\":%llu,\"kind\":%u,\"thread\":%llu,\"owner\":%llu,\"span\":%llu,\"words\":[",i?",":"",i,(unsigned long long)e->ns,e->kind,(unsigned long long)e->thread,(unsigned long long)e->owner,(unsigned long long)e->span);for(unsigned j=0;j<e->count;j++)fprintf(f,"%s%llu",j?",":"",(unsigned long long)e->words[j]);fputs("]}",f);}fputs("]}\n",f);int rc=fclose(f);pthread_mutex_unlock(&control_lock);errno=saved;return rc;}
static void pause_ms(unsigned ms){struct timespec ts={ms/1000,(long)(ms%1000)*1000000};nanosleep(&ts,NULL);}
static void *control_main(void *arg){char *path=arg;uint64_t last=0;for(;;){FILE *f=fopen(path,"r");unsigned long long id=0,ms=0;char output[2048];int got=f?fscanf(f,"%llu %llu %2047s",&id,&ms,output):0;if(f)fclose(f);if(got==3 && id && id!=last && ms && ms<=30000){last=id;if(!ps3_critical_begin(id,ms*1000000)){while(ps3_critical_now_ns()-started<ms*1000000)pause_ms(10);ps3_critical_finish(output);fprintf(stderr,"[critical trace] capture=%llu complete output=%s\n",id,output);}}pause_ms(50);}return NULL;}
void ps3_critical_control_start(void){const char *p=getenv("PS3RECOMP_CRITICAL_TRACE_CONTROL");if(!p||!*p)return;pthread_t t;char *copy=strdup(p);if(copy && !pthread_create(&t,NULL,control_main,copy))pthread_detach(t);else free(copy);}
