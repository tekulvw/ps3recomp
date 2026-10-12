/* Catches missing NID registration, immediate/wrong-ABI completion, stale
 * busy state, duplicate/reentrant delivery, and lost abort status. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "ps3emu/guest_call.h"
#include "../libs/system/cellSysutil.h"
static void *check_handler,*abort_handler,*show_handler;
void ppu_hle_register_all(void);
void ps3_hle_register(unsigned nid,const char* name,void* fn) {
 (void)name;
 if(nid==0x59D1629A)check_handler=fn;
 if(nid==0x5EC05AD8)abort_handler=fn;
 if(nid==0x6599500D)show_handler=fn;
}
typedef int32_t (*check_fn)(uint32_t,uint32_t,uint64_t,uint32_t,uint32_t);
typedef int32_t (*abort_fn)(void);
typedef int32_t (*show_fn)(uint32_t,uint64_t,uint32_t,uint32_t);
static check_fn check;static abort_fn abort_eula;static show_fn show;
static unsigned calls,status_wanted=2;
static void guest(uint32_t opd,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h) {
 assert(opd==0x1ADED50);assert(a==status_wanted);assert(b==0);assert(c==1);assert(d==0x1FC2440);assert(!(e|f|g|h));calls++;
 assert(check(0xFEF58A4,0,0,0x1ADED50,0x1FC2440)==(int32_t)0x8002E503);
 cellSysutilCheckCallback(); /* Cannot redeliver the in-flight request. */
}
void cellOskDialog_tick(void){}
void cellMsgDialog_tick(void){}
void np_psnr_pump(void){}
void ps3_np_ticket_tick(void){}
uint8_t* vm_base;
void vm_write8(uint64_t a,uint8_t v){(void)a;(void)v;}
void vm_write32(uint64_t a,uint32_t v){(void)a;(void)v;}
int main(void) {
 ppu_hle_register_all();assert(check_handler && abort_handler && show_handler);
 check=(check_fn)check_handler;abort_eula=(abort_fn)abort_handler;show=(show_fn)show_handler;
 assert(abort_eula()==(int32_t)0x8002E502);
 assert(check(0,0,0,0x1ADED50,0)==(int32_t)0x8002E501);
 assert(check(0xFEF58A4,0,0,0,0)==(int32_t)0x8002E501);
 assert(check(0xFEF58A4,0,0,0x1ADED50,0x1FC2440)==0);assert(calls==0);
 assert(check(0xFEF58A4,0,0,0x1ADED50,0)==(int32_t)0x8002E503);
 assert(show(0xFEF58A4,0,0x1ADED50,0)==(int32_t)0x8002E503);
 cellSysutilCheckCallback();assert(calls==0); /* No guest caller: retain. */
 g_ps3_guest_caller=guest;cellSysutilCheckCallback();assert(calls==1);
 cellSysutilCheckCallback();assert(calls==1);assert(abort_eula()==(int32_t)0x8002E502);
 assert(check(0xFEF58A4,0,0,0x1ADED50,0x1FC2440)==0);
 assert(abort_eula()==0);status_wanted=4;cellSysutilCheckCallback();assert(calls==2);
 status_wanted=2;assert(check(0xFEF58A4,0,0,0x1ADED50,0x1FC2440)==0);cellSysutilCheckCallback();assert(calls==3);
 assert(show(0,0,0x1ADED50,0)==(int32_t)0x8002E501);
 assert(show(0xFEF58A4,0,0x1ADED50,0)==(int32_t)0x8002E500); /* UI unsupported, explicit failure. */
 assert(abort_eula()==(int32_t)0x8002E502);
 puts("NP EULA registration, deferred ABI, busy, abort and repeat lifecycle PASS");
}
