/* Exercises the real NP handlers and sysutil pump. Catches shared-account
 * tickets, wrong guest endianness, early/duplicate callbacks and stale state. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "../libs/network/sceNp.h"
#include "../libs/network/np_psnr.h"
#include "../libs/system/cellSysutil.h"
#include "../runtime/ppu/ppu_memory.h"
#include "ps3emu/guest_call.h"
void edat_add_klicensee(const uint8_t klic[16]) {(void)klic;}
uint8_t* vm_base;
uint32_t ppu_vm_size = 65536;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}
void cellOskDialog_tick(void){}
void cellMsgDialog_tick(void){}
static void *request_handler,*get_handler;
void ppu_hle_register_all(void);
void ps3_hle_register(unsigned nid,const char* name,void* fn){
 (void)name;if(nid==0x8297F1EC)request_handler=fn;
 if(nid==0x0968AA36)get_handler=fn;
}
typedef s32(*request_fn)(const SceNpId*,const void*,const char*,const void*,u32,const char*,u32);
typedef s32(*get_fn)(void*,u32*);
static request_fn request;
static get_fn get;
static unsigned callbacks;
static unsigned online_callbacks;
static jmp_buf escape_frame;
static int escape_callback;
static const char *wanted;
static void callback(u32 opd,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f,u64 g,u64 h){
 assert(opd==0x1234);assert(c==0x5678);
 if(a==3){assert(b==0);online_callbacks++;return;}
 assert(a==255);assert(b==strlen(wanted));
 if(escape_callback)longjmp(escape_frame,1);
 assert(!(d|e|f|g|h));callbacks++;
 assert((u32)request((void*)0x100,(void*)0x300,(void*)0x400,0,0,0,0)==0x8002a00a);
 vm_write32(0x500,32);assert(get((void*)0x600,(void*)0x500)==0);
 assert(vm_read32(0x500)==strlen(wanted));assert(!memcmp(vm_base+0x600,wanted,strlen(wanted)));
 cellSysutilCheckCallback(); /* Reentrant poll must not duplicate completion. */
}
static s32 req(void){return request((void*)0x100,(void*)0x300,(void*)0x400,0,0,0,0);}
int main(void){
 ppu_hle_register_all();assert(request_handler && get_handler);
 request=(request_fn)request_handler;get=(get_fn)get_handler;
 vm_base=calloc(1,ppu_vm_size);assert(vm_base);
 memcpy(vm_base+0x400,"MAG-service",12);vm_write16(0x300,2);vm_write16(0x302,1);
 assert((u32)req()==0x8002aa01);assert((u32)get(0,(void*)0x500)==0x8002aa01);
 setenv("PS3_NP_ONLINE_ID","PlayerRetail",1);
 assert(sceNpInit(131072,(void*)0x1000)==0);
 assert((u32)req()==0x8002aa0c); /* Default remains offline. */
 setenv("PS3RECOMP_NP_LOCAL_TICKET","1",1);
 assert(sceNpManagerGetNpId((void*)0x100)==0);
 assert(!memcmp(vm_base+0x100,"PlayerRetail",12));
 assert(sceNpManagerGetStatus((void*)0x500)==0);assert(vm_read32(0x500)==3);
 assert((u32)req()==0x8002aa0a); /* No callback cannot produce a completion. */
 g_ps3_guest_caller=callback;
 assert(sceNpManagerRegisterCallback((void*)0x1234,(void*)0x5678)==0);
 /* Online state callback is allowed separately from ticket callback. */
 g_ps3_guest_caller=0;cellSysutilCheckCallback();
 wanted="playerretail";
 assert(req()==0);assert(callbacks==0);
 assert((u32)req()==0x8002a00a);
 assert((u32)get((void*)0x600,(void*)0x500)==0x8002aa0a);
 cellSysutilCheckCallback();assert(callbacks==0);
 g_ps3_guest_caller=callback;cellSysutilCheckCallback();assert(online_callbacks==1 && callbacks==0);
 cellSysutilCheckCallback();assert(callbacks==1);
 cellSysutilCheckCallback();assert(callbacks==1);
 assert(get(0,(void*)0x500)==0);assert(vm_read32(0x500)==12);
 memset(vm_base+0x600,0xaa,32);vm_write32(0x500,11);
 assert((u32)get((void*)0x600,(void*)0x500)==0x8002aa0f);
 assert(vm_read32(0x500)==12);assert(vm_base[0x600]==0xaa);
 vm_write32(0x500,32);assert(get((void*)0x600,(void*)0x500)==0);
 assert(!memcmp(vm_base+0x600,"playerretail",12));assert(vm_base[0x60c]==0xaa);
 assert((u32)get(0,0)==0x8002aa03);
 /* A mismatched identity may not mint the configured account's ticket. */
 memset(vm_base+0x100,0,sizeof(SceNpId));memcpy(vm_base+0x100,"Player",6);
 assert((u32)req()==0x8002aa16);
 sceNpManagerGetNpId((void*)0x100);
 assert((u32)request((void*)0x100,0,0,0,0,0,0)==0x8002a015);
 assert((u32)request((void*)0x100,0,(void*)0x400,0,1025,0,0)==0x8002a015);
 assert(req()==0);sceNpManagerUnregisterCallback();cellSysutilCheckCallback();assert(callbacks==1);
 assert((u32)get(0,(void*)0x500)==0x8002aa0a);
 sceNpManagerRegisterCallback((void*)0x1234,(void*)0x5678);
 assert(req()==0);assert(sceNpTerm()==0);cellSysutilCheckCallback();assert(callbacks==1 && online_callbacks==1);
 assert(sceNpInit(131072,(void*)0x1000)==0);
 assert((u32)get(0,(void*)0x500)==0x8002aa0a);
 /* Resolve another real NP identity after term; no stale PlayerRetail ticket. */
 np_psnr_setup("Player",NULL);wanted="player";
 sceNpManagerGetNpId((void*)0x100);sceNpManagerRegisterCallback((void*)0x1234,(void*)0x5678);
 assert(req()==0);cellSysutilCheckCallback();cellSysutilCheckCallback();assert(callbacks==2);
 /* sys_ppu_thread_exit can unwind a callback without returning to the pump. */
 escape_callback=1;
 if(!setjmp(escape_frame)){assert(req()==0);cellSysutilCheckCallback();assert(0);}
 escape_callback=0;assert(sceNpTerm()==0);assert(sceNpInit(131072,(void*)0x1000)==0);
 sceNpManagerRegisterCallback((void*)0x1234,(void*)0x5678);
 assert(req()==0);cellSysutilCheckCallback();cellSysutilCheckCallback();assert(callbacks==3);
 setenv("PS3RECOMP_NP_LOCAL_TICKET","0",1);
 assert((u32)get(0,(void*)0x500)==0x8002aa0c);
 setenv("PS3RECOMP_NP_LOCAL_TICKET","1",1);
 assert((u32)get(0,(void*)0x500)==0x8002aa0a);
 assert(req()==0);
 setenv("PS3RECOMP_NP_LOCAL_TICKET","0",1);
 cellSysutilCheckCallback();assert(callbacks==3);
 assert((u32)req()==0x8002aa0c);assert((u32)get(0,(void*)0x500)==0x8002aa0c);
 sceNpTerm();free(vm_base);
 puts("NP local ticket: registration, account binding, deferred completion, retrieval and lifecycle PASS");
}
