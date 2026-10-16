/* Actual production GCM configuration, publication and process-lifetime
 * auto-reset event. Reuses the existing unrelated FIFO/backend fixture stubs. */
#define main checked_reference_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
static HANDLE handles[8];
static atomic_int waiter_ready;
static void* wait_for_event(void*unused){(void)unused;atomic_store(&waiter_ready,1);return (void*)(uintptr_t)WaitForSingleObject(handles[0],1000);}
static void* create_handle(void*arg){handles[(uintptr_t)arg]=(HANDLE)cellGcm_fifo_kick_event();return NULL;}
int main(int argc,char**argv){
 assert(argc==2);int on=atoi(argv[1]);
 setenv("MAG_NATIVE_CONSUMER",argv[1],1);setenv("PS3RECOMP_GCM_CHECKED_REF","0",1);unsetenv("GCM_DRAINDBG");
 cellGcm_native_consumer_init();assert(cellGcm_native_consumer_enabled()==on);assert(g_gcm_consumer_put_wake_ea==(on?GCM_CONTROL_GUEST_ADDR:0));
 assert(!gcm_checked_ref_mode()&&!gcm_drain_debug());
 setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);setenv("GCM_DRAINDBG","",1);
 assert(gcm_checked_ref_mode()==!on&&gcm_drain_debug()==!on);
 pthread_t threads[8];for(uintptr_t n=0;n<8;n++)assert(!pthread_create(&threads[n],NULL,create_handle,(void*)n));
 for(unsigned n=0;n<8;n++){assert(!pthread_join(threads[n],NULL));assert(handles[n]&&handles[n]==handles[0]);}
 assert(WaitForSingleObject(handles[0],0)==WAIT_TIMEOUT);
 cellGcm_fifo_kick_notify();assert(WaitForSingleObject(handles[0],0)==(on?WAIT_OBJECT_0:WAIT_TIMEOUT));
 assert(WaitForSingleObject(handles[0],0)==WAIT_TIMEOUT);
 cellGcm_native_consumer_init();assert((HANDLE)cellGcm_fifo_kick_event()==handles[0]);
 if(on){
  cellGcm_fifo_kick_notify();cellGcm_fifo_kick_wait(4);assert(WaitForSingleObject(handles[0],0)==WAIT_TIMEOUT);
  pthread_t waiter;assert(!pthread_create(&waiter,NULL,wait_for_event,NULL));while(!atomic_load(&waiter_ready))sched_yield();cellGcm_fifo_kick_notify();void*result;assert(!pthread_join(waiter,&result));assert((uintptr_t)result==WAIT_OBJECT_0);
 }
 vm_base=mmap(NULL,UINT64_C(0x100000000),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(vm_base!=MAP_FAILED);s_deferred_reports=0;
 cellGcm_syscall_set_fifo(0x1234,0x5678);assert(vm_read32(GCM_CONTROL_GUEST_ADDR)==0x1234&&vm_read32(GCM_CONTROL_GUEST_ADDR+4)==0x5678&&s_fifo_getoff==0x5678);
 assert(WaitForSingleObject(handles[0],0)==(on?WAIT_OBJECT_0:WAIT_TIMEOUT));assert(!munmap(vm_base,UINT64_C(0x100000000)));
 return 0;
}
