#include "critical_trace.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static void *writer(void *unused) { (void)unused; for(unsigned i=0;i<80000;i++){uint64_t w[2]={i,~(uint64_t)i};ps3_critical_emit(99,7,i,w,2);}return NULL; }
int main(int argc,char **argv){assert(argc==2);errno=EDOM;ps3_critical_emit(1,0,0,NULL,0);assert(errno==EDOM);assert(!ps3_critical_begin(1,1000000000));pthread_t t[4];for(int i=0;i<4;i++)assert(!pthread_create(t+i,NULL,writer,NULL));for(int i=0;i<4;i++)pthread_join(t[i],NULL);assert(!ps3_critical_finish(argv[1]));assert(errno==EDOM);char first[4096];snprintf(first,sizeof(first),"%s.1",argv[1]);assert(!rename(argv[1],first));assert(!ps3_critical_begin(2,1000000000));ps3_critical_emit_capture(1,77,0,0,NULL,0);ps3_critical_emit(88,0,0,NULL,0);assert(!ps3_critical_finish(argv[1]));puts("critical trace fixture PASS");}

uint32_t spurs_policy_service_identity(const void *ctx){(void)ctx;return 0;}
