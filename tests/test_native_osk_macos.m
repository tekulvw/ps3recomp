/* Real AppKit sheet, text field, button routing, and guest poll delivery. */
#import <AppKit/AppKit.h>
#include "../libs/misc/osk_macos.m"
#include "../libs/video/rsx_metal_backend.m"
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <assert.h>
uint8_t* vm_base;uint32_t ppu_vm_size=65536,g_ww_lo,g_ww_hi;int g_resv_store_active;
void ppu_resv_break_store(uint64_t a){}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int z){}
void vm_write32(uint64_t a,uint32_t v){assert(a+4<=65536);v=__builtin_bswap32(v);memcpy(vm_base+a,&v,4);}
void vm_write8(uint64_t a,uint8_t v){assert(a<65536);vm_base[a]=v;}
int spu_coh_is_reserved(uint32_t a){return 0;}void spu_lockline_lock(void){}void spu_lockline_unlock(void){}void spu_coh_notify_write(uint32_t a){}
void cellMsgDialog_tick(void){}void np_psnr_pump(void){}
static unsigned events;static uint32_t codes[20];
static void cb(u32 opd,u64 code,u64 param,u64 user,u64 d,u64 e,u64 f,u64 g,u64 h){codes[events++]=(uint32_t)code;}
static void run_ui(void){for(int i=0;i<100;i++)[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];}
static void put16(uint32_t a,uint16_t v){v=__builtin_bswap16(v);memcpy(vm_base+a,&v,2);}
static uint16_t get16(uint32_t a){uint16_t v;memcpy(&v,vm_base+a,2);return __builtin_bswap16(v);}
static uint32_t get32(uint32_t a){uint32_t v;memcpy(&v,vm_base+a,4);return __builtin_bswap32(v);}
static void result(void){vm_write32(0x1200,0xA5A5A5A5);vm_write32(0x1204,32);vm_write32(0x1208,0x2200);}
int main(void){@autoreleasepool {
 vm_base=calloc(1,65536);assert(vm_base);
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[NSApp finishLaunching];
 s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);
 assert(!create_window_impl("OSK fixture"));NSWindow* window=s_window;window.releasedWhenClosed=NO;assert(osk_native_available());
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);put16(0x2000,'N');put16(0x2002,0);
 g_ps3_guest_caller=cb;cellSysutilRegisterCallback(1,(void*)0x3000,NULL);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));assert(!s_alert);
 run_ui();assert(s_alert && s_alert.window.sheetParent==window && ![s_field isKindOfClass:NSSecureTextField.class]);
 assert(s_alert.window.firstResponder!=window.contentView);assert(events==0);
 s_field.stringValue=@"PΩ🚀";[s_alert.buttons[0] performClick:nil];run_ui();assert(!s_alert && events==0);
 cellSysutilCheckCallback();assert(events==2 && codes[0]==0x502 && codes[1]==0x503);
 result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==0 && get16(0x2200)=='P' && get16(0x2202)==0x03A9 && get16(0x2204)==0xD83D && get16(0x2206)==0xDE80 && !get16(0x2208));
 cellSysutilCheckCallback();assert(events==3 && codes[2]==0x504);run_ui();
 vm_write32(0x1100,0x20000000);assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert([s_field isKindOfClass:NSSecureTextField.class]);
 [s_alert.buttons[1] performClick:nil];run_ui();cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==1);cellSysutilCheckCallback();run_ui();
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));assert(!cellOskDialogAbort());run_ui();assert(!s_alert);cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==2);cellSysutilCheckCallback();run_ui();
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));
 [window close];assert(!osk_native_available());run_ui();assert(!s_alert);
 cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==2);cellSysutilCheckCallback();run_ui();free(vm_base);
 puts("Real macOS OSK sheet / Unicode OK / secure cancel / pre-open abort / deferred sysutil checks passed");
}return 0;}
