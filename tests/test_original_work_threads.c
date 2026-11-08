#ifdef NDEBUG
#undef NDEBUG
#endif
#include "original_work_observer.h"
#include "original_work_threads.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static int seen,go,exit_directly;
static ps3_original_identity identity={11,2,13};
static int resolve(const void *context,uint64_t pc,unsigned route,const char *name,ps3_original_identity *out) {
  (void)pc;(void)route;assert(name);*out=*(const ps3_original_identity *)context;return 0;
}
static void *worker(void *arg) {
  assert(arg==&identity);
  PS3_ORIGINAL_STEP(arg,16,PS3_ORIGINAL_LIFTED);
  assert(!pthread_mutex_lock(&mutex));seen=1;assert(!pthread_cond_broadcast(&condition));
  while(!go)assert(!pthread_cond_wait(&condition,&mutex));
  assert(!pthread_mutex_unlock(&mutex));
  PS3_ORIGINAL_STEP(arg,20,PS3_ORIGINAL_LIFTED); // Actual late work through drain.
  if(exit_directly)pthread_exit((void *)(uintptr_t)0x77);
  return (void *)(uintptr_t)0x77;
}
int main(int argc,char **argv) {
  assert(argc==2);exit_directly=!strcmp(argv[1],"exit");
  ps3_original_source source={.id=11,.generation=2,.provider_mask=1,.code_sha256={1}};
  ps3_original_provider provider={.id=13,.route_mask=2,.source_sha256={2}};
  ps3_original_target target={.id=31,.source=11,.low=16,.high=32,.route_mask=2};
  ps3_original_config config={.struct_size=sizeof(config),.abi_version=1,.source_count=1,
    .provider_count=1,.target_count=1,.covered_from=100,.counter_limit=UINT64_MAX,
    .manifest_sha256={3},.sources=&source,.providers=&provider,.targets=&target,.resolve=resolve};
  assert(!ps3_original_arm(&config));
  pthread_t thread;assert(!ps3_original_pthread_create(&thread,NULL,worker,&identity));
  assert(!pthread_mutex_lock(&mutex));while(!seen)assert(!pthread_cond_wait(&condition,&mutex));
  assert(!pthread_mutex_unlock(&mutex));
  ps3_original_live_report live={.struct_size=sizeof(live),.abi_version=1};
  assert(!ps3_original_live_status(&live)&&live.workers==1&&live.faults==0);
  uint64_t count=0;assert(!ps3_original_live_total(31,PS3_ORIGINAL_LIFTED,&count)&&count==1);
  assert(!ps3_original_begin_drain(150));
  ps3_original_report report={.struct_size=sizeof(report),.abi_version=1};
  assert(ps3_original_seal(&report)==-4);
  assert(!pthread_mutex_lock(&mutex));go=1;assert(!pthread_cond_broadcast(&condition));
  assert(!pthread_mutex_unlock(&mutex));
  void *result=NULL;assert(!pthread_join(thread,&result)&&result==(void *)(uintptr_t)0x77);
  assert(!ps3_original_seal(&report)&&report.faults==0);
  assert(!ps3_original_live_total(31,PS3_ORIGINAL_LIFTED,&count)&&count==2);
  printf("REAL_THREAD mode=%s live1_unsealed_then_late2_joined_and_retired\n",argv[1]);
}
