/* Actual owner synchronization primitives; no native graphics API/device. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_native_owner_gate.h"
#include "../../../runtime/platform/win32_compat.h"
#include <assert.h>
#include <stdio.h>
static rsx_native_owner_gate gate = RSX_NATIVE_OWNER_GATE_INIT;
static rsx_native_owner_thread main_thread;
static HANDLE entered,finish;
static DWORD WINAPI original_worker(void *unused) {
    (void)unused;rsx_native_owner_thread thread={0};
    assert(rsx_owner_original_begin(&gate,&thread)==0);
    rsx_owner_dirty(&gate,&thread);SetEvent(entered);
    assert(WaitForSingleObject(finish,3000)==WAIT_OBJECT_0);
    rsx_owner_original_end(&gate,&thread);return 0;
}
int main(void) {
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    assert(rsx_owner_generation(&gate,&main_thread,7)==0);
    rsx_owner_original_end(&gate,&main_thread);
    assert(rsx_owner_retain(&gate,&main_thread,6)==-3);
    assert(rsx_owner_retain(&gate,&main_thread,7)==0);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-4); /* dirty */
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    assert(rsx_owner_original_begin(&gate,&main_thread)==0); /* nested effect */
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-4);
    rsx_owner_submitted(&gate,&main_thread,1);
    rsx_owner_original_end(&gate,&main_thread);
    rsx_owner_original_end(&gate,&main_thread);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==0);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-4); /* no recursive native lease */
    rsx_owner_native_end(&gate,&main_thread);
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    assert(rsx_owner_generation(&gate,&main_thread,8)==-4); /* retained owner pins life */
    rsx_owner_original_end(&gate,&main_thread);
    entered=CreateEventA(NULL,FALSE,FALSE,NULL);finish=CreateEventA(NULL,FALSE,FALSE,NULL);assert(entered&&finish);
    HANDLE worker=CreateThread(NULL,0,original_worker,NULL,0,NULL);assert(worker);
    assert(WaitForSingleObject(entered,3000)==WAIT_OBJECT_0);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-4); /* another encoder holds lock */
    SetEvent(finish);assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-4); /* record remains dirty */
    CloseHandle(worker);CloseHandle(entered);CloseHandle(finish);
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    rsx_owner_submitted(&gate,&main_thread,0); /* failed boundary is sticky */
    rsx_owner_submitted(&gate,&main_thread,1);
    rsx_owner_original_end(&gate,&main_thread);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-6);
    rsx_owner_release(&gate,&main_thread,7);
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    assert(rsx_owner_generation(&gate,&main_thread,8)==0);
    rsx_owner_submitted(&gate,&main_thread,1);
    rsx_owner_original_end(&gate,&main_thread);
    assert(rsx_owner_native_begin(&gate,&main_thread,7)==-3);
    assert(rsx_owner_retain(&gate,&main_thread,8)==0);
    assert(rsx_owner_native_begin(&gate,&main_thread,8)==0);
    assert(rsx_owner_original_begin(&gate,&main_thread)==-6); /* no original reentry during native ownership */
    rsx_owner_native_end(&gate,&main_thread);
    assert(rsx_owner_native_begin(&gate,&main_thread,8)==-6);
    rsx_owner_release(&gate,&main_thread,8);
    assert(rsx_owner_original_begin(&gate,&main_thread)==0);
    assert(rsx_owner_generation(&gate,&main_thread,9)==0);
    rsx_owner_submitted(&gate,&main_thread,1);gate.closing=1;
    rsx_owner_original_end(&gate,&main_thread);
    assert(rsx_owner_retain(&gate,&main_thread,9)==-3);
    puts("Native owner gate: dirty, contention, nesting, pinning, stale, and sticky failure passed");
    return 0;
}
