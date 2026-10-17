#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "job_io_recorder.h"
#include "../spu/spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#define MAX_BYTES (96u<<20)
void *ps3_jobio_active;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static uint8_t *buffer;static size_t used,capacity;static int truncated;
static uint8_t *entry_ls,*entry_gpr;static uint32_t run_tag,run_pc;static uint64_t run_start;
static char run_directory[PATH_MAX];static unsigned sequence;
static uint64_t now_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
static void release(void){free(buffer);free(entry_ls);free(entry_gpr);buffer=entry_ls=entry_gpr=NULL;used=capacity=0;truncated=0;}
int ps3_jobio_begin(struct spu_context *c,uint32_t tag,const char *directory){
 if(!c||!directory||!*directory||strlen(directory)>=sizeof run_directory)return 1;
 pthread_mutex_lock(&lock);
 if(__atomic_load_n(&ps3_jobio_active,__ATOMIC_ACQUIRE)){pthread_mutex_unlock(&lock);return 2;}
 release();
 entry_ls=malloc(SPU_LS_SIZE);entry_gpr=malloc(sizeof c->gpr);
 if(!entry_ls||!entry_gpr){release();pthread_mutex_unlock(&lock);return 3;}
 memcpy(entry_ls,c->ls,SPU_LS_SIZE);memcpy(entry_gpr,c->gpr,sizeof c->gpr);
 strcpy(run_directory,directory);run_tag=tag;run_pc=(uint32_t)c->pc;run_start=now_ns();
 __atomic_store_n(&ps3_jobio_active,c,__ATOMIC_RELEASE);
 pthread_mutex_unlock(&lock);return 0;
}
void ps3_jobio_event(struct spu_context *c,uint32_t kind,uint32_t cmd,uint32_t lsa,uint64_t ea,uint32_t size,const uint8_t *bytes,uint32_t extra){
 if(!ps3_jobio_is(c))return;
 pthread_mutex_lock(&lock);
 if(__atomic_load_n(&ps3_jobio_active,__ATOMIC_ACQUIRE)==c&&!truncated){
  size_t need=28+(bytes?size:0);
  if(used+need>MAX_BYTES)truncated=1;
  else{
   if(used+need>capacity){size_t next=capacity?capacity*2:(1u<<20);while(next<used+need)next*=2;uint8_t *grown=realloc(buffer,next);if(!grown){truncated=1;pthread_mutex_unlock(&lock);return;}buffer=grown;capacity=next;}
   uint32_t head[7]={kind,cmd,lsa,(uint32_t)ea,size,(uint32_t)c->pc,extra};
   memcpy(buffer+used,head,28);used+=28;if(bytes&&size){memcpy(buffer+used,bytes,size);used+=size;}
  }
 }
 pthread_mutex_unlock(&lock);
}
int ps3_jobio_end(struct spu_context *c){
 if(!ps3_jobio_is(c))return -1;
 pthread_mutex_lock(&lock);
 __atomic_store_n(&ps3_jobio_active,NULL,__ATOMIC_RELEASE);
 int result=-1;char path[PATH_MAX+64],temporary[PATH_MAX+80];
 snprintf(path,sizeof path,"%s/job-%04u.jobio",run_directory,sequence);snprintf(temporary,sizeof temporary,"%s.tmp",path);
 FILE *f=fopen(temporary,"wb");
 if(f){
  uint32_t head[8]={0x4a4f4249u,1,run_tag,run_pc,SPU_LS_SIZE,(uint32_t)sizeof c->gpr,truncated,(uint32_t)used};
  uint64_t times[2]={run_start,now_ns()};
  int ok=fwrite(head,sizeof head,1,f)==1&&fwrite(times,sizeof times,1,f)==1&&fwrite(entry_ls,SPU_LS_SIZE,1,f)==1&&fwrite(entry_gpr,sizeof c->gpr,1,f)==1&&(!used||fwrite(buffer,used,1,f)==1)&&fwrite(c->ls,SPU_LS_SIZE,1,f)==1&&fwrite(c->gpr,sizeof c->gpr,1,f)==1;
  if(fclose(f)==0&&ok&&rename(temporary,path)==0){result=0;++sequence;}else remove(temporary);
 }
 release();pthread_mutex_unlock(&lock);return result;
}
