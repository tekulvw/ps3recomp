#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include "../libs/misc/osk_native.h"
uint8_t memory[65536]; uint8_t* vm_base=memory; uint32_t ppu_vm_size=sizeof(memory);
uint32_t g_ww_lo,g_ww_hi; int g_resv_store_active;
void ppu_resv_break_store(uint64_t a){(void)a;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int z){(void)a;(void)v;(void)z;}
void vm_write32(uint64_t a,uint32_t v){assert(a+4<=sizeof(memory));v=__builtin_bswap32(v);memcpy(memory+a,&v,4);}
void vm_write8(uint64_t a,uint8_t v){assert(a<sizeof(memory));memory[a]=v;}
int spu_coh_is_reserved(uint32_t a){return 0;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void spu_coh_notify_write(uint32_t a){}
void cellMsgDialog_tick(void){}
void np_psnr_pump(void){}
static void put16(uint32_t a,uint16_t v){v=__builtin_bswap16(v);memcpy(memory+a,&v,2);}
static OskNativeRequest request; static unsigned opens,closes,events;static uint32_t statuses[100];
static int available=1;
int osk_native_available(void){return available;}
void osk_native_open(const OskNativeRequest* r){request=*r;opens++;}
void osk_native_close(uint64_t token){assert(token==request.token);closes++;}
static void cb(u32 opd,u64 status,u64 param,u64 user,u64 d,u64 e,u64 f,u64 g,u64 h){
 assert(opd==0x3000 && user==0xDEAD && !param && !d && !e && !f && !g && !h);
 statuses[events++]=(uint32_t)status;
}
static uint32_t read32(uint32_t a){uint32_t x;memcpy(&x,memory+a,4);return __builtin_bswap32(x);}
static uint16_t read16(uint32_t a){uint16_t x;memcpy(&x,memory+a,2);return __builtin_bswap16(x);}
static int32_t load(void){return cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000);}
static void result_at(uint32_t capacity){vm_write32(0x1200,0xA5A5A5A5);vm_write32(0x1204,capacity);vm_write32(0x1208,0x2200);}
static void* background_finish(void* text){osk_native_did_finish(request.token,0,text,4);return NULL;}
int main(void){
 memset(memory,0xA5,sizeof(memory));
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);
 put16(0x2000,'N');put16(0x2002,0);put16(0x2100,0);
 memset(memory+0x1100,0,20);
 int32_t rc=cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000);
 if(rc){fprintf(stderr,"FAIL: valid retail LoadAsync without artificial Init returned %08X\n",(uint32_t)rc);return 1;}
 assert(opens==1 && request.message[0]=='N' && request.limit==32);
 assert(load()==(int32_t)0x8002B105);
 g_ps3_guest_caller=cb;assert(!cellSysutilRegisterCallback(1,(void*)0x3000,(void*)0xDEAD));
 cellSysutilCheckCallback();assert(events==0); // no fabricated loaded or completion
 osk_native_did_open(request.token);assert(events==0);
 cellSysutilCheckCallback();assert(events==1 && statuses[0]==0x502);
 for(int i=0;i<10;i++)cellSysutilCheckCallback();assert(events==1);
 uint16_t text[]={'P',0x03A9,0xD83D,0xDE80,0};
 pthread_t worker;assert(!pthread_create(&worker,NULL,background_finish,text));assert(!pthread_join(worker,NULL));assert(events==1);
 cellSysutilCheckCallback();assert(events==2 && statuses[1]==0x503);
 result_at(2);assert(!cellOskDialogUnloadAsync((void*)0x1200));
 assert(read32(0x1200)==0 && read32(0x1204)==2);
 assert(read16(0x2200)=='P' && read16(0x2202)==0x03A9 && read16(0x2204)==0 && memory[0x2206]==0xA5);
 assert(events==2);cellSysutilCheckCallback();assert(events==3 && statuses[2]==0x504);
 assert(!load());uint64_t token=request.token;
 osk_native_did_open(token-1);cellSysutilCheckCallback();assert(events==3);
 osk_native_did_open(token);cellSysutilCheckCallback();assert(statuses[events-1]==0x502);
 assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B105);
 osk_native_did_finish(token,1,NULL,0);cellSysutilCheckCallback();
 result_at(32);memory[0x2200]=0xA5;assert(!cellOskDialogUnloadAsync((void*)0x1200));
 assert(read32(0x1200)==1 && memory[0x2200]==0xA5);cellSysutilCheckCallback();
 assert(!load());token=request.token;assert(!cellOskDialogAbort());
 osk_native_did_open(token);osk_native_did_finish(token,0,text,4);
 cellSysutilCheckCallback();assert(statuses[events-1]==0x503);
 result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(read32(0x1200)==2);
 cellSysutilCheckCallback();
 available=0;assert(load()==(int32_t)0x8002B503);available=1;
 vm_write32(0x1008,513);assert(load()==(int32_t)0x8002B504);vm_write32(0x1008,32);
 vm_write32(0x1004,0xFFFFFFFF);assert(load()==(int32_t)0x8002B504);vm_write32(0x1004,0x2100);
 assert(!load());osk_native_did_open(request.token);cellSysutilCheckCallback();
 osk_native_did_finish(request.token,0,text,4);cellSysutilCheckCallback();
 result_at(3);assert(!cellOskDialogUnloadAsync((void*)0x1200)); // never split surrogate
 assert(read16(0x2204)==0);cellSysutilCheckCallback();
 assert(!load());osk_native_did_open(request.token);cellSysutilCheckCallback();
 osk_native_did_finish(request.token,0,text,4);cellSysutilCheckCallback();
 result_at(32);vm_write32(0x1208,65534);assert(cellOskDialogUnloadAsync((void*)0x1200)==(int32_t)0x8002B504);
 assert(read32(0x1200)==0xA5A5A5A5);vm_write32(0x1208,0x2200);
 assert(!cellOskDialogUnloadAsync((void*)0x1200));cellSysutilCheckCallback();
 vm_write32(0x1008,1);put16(0x2100,0xD83D);put16(0x2102,0xDE80);put16(0x2104,0);
 assert(!load());assert(request.initial[0]==0); // initial text must not split a surrogate pair
 assert(!cellOskDialogAbort());cellSysutilCheckCallback();result_at(32);assert(!cellOskDialogUnloadAsync((void*)0x1200));cellSysutilCheckCallback();
 puts("Native OSK load / real sysutil lifecycle / Unicode / cancellation / stale token / bounded guest result checks passed");return 0;
}
