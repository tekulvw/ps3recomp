/* Real AppKit sheet, text field, button routing, and guest poll delivery. */
#import <AppKit/AppKit.h>
#include "../libs/misc/osk_macos.m"
#include "../libs/video/rsx_metal_backend.m"
#include "../libs/misc/cellOskDialog.h"
#include "../libs/system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <assert.h>
#include <unistd.h>
#include "../libs/input/pad_macos.h"
#include "../libs/input/cellPad.h"
uint8_t* vm_base;uint32_t ppu_vm_size=65536,g_ww_lo,g_ww_hi;int g_resv_store_active;
void ppu_resv_break_store(uint64_t a){}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int z){}
void vm_write32(uint64_t a,uint32_t v){assert(a+4<=65536);v=__builtin_bswap32(v);memcpy(vm_base+a,&v,4);}
void vm_write8(uint64_t a,uint8_t v){assert(a<65536);vm_base[a]=v;}
int spu_coh_is_reserved(uint32_t a){return 0;}void spu_lockline_lock(void){}void spu_lockline_unlock(void){}void spu_coh_notify_write(uint32_t a){}
void cellMsgDialog_tick(void){}void np_psnr_pump(void){}
void ps3_np_ticket_tick(void){}
static unsigned events;static uint32_t codes[128];
static void cb(u32 opd,u64 code,u64 param,u64 user,u64 d,u64 e,u64 f,u64 g,u64 h){assert(events<128);codes[events++]=(uint32_t)code;}
static void run_ui(void){for(int i=0;i<100;i++){
 for(int e=0;e<16;e++){
  NSEvent* event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
  if(!event)break;[NSApp sendEvent:event];
 }
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
}}
static void put16(uint32_t a,uint16_t v){v=__builtin_bswap16(v);memcpy(vm_base+a,&v,2);}
static uint16_t get16(uint32_t a){uint16_t v;memcpy(&v,vm_base+a,2);return __builtin_bswap16(v);}
static uint32_t get32(uint32_t a){uint32_t v;memcpy(&v,vm_base+a,4);return __builtin_bswap32(v);}
static void result(void){vm_write32(0x1200,0xA5A5A5A5);vm_write32(0x1204,32);vm_write32(0x1208,0x2200);}
static void control_command(uint64_t token){
 const char* path=getenv("OSK_NATIVE_CONTROL_FILE");assert(path);FILE* f=fopen(path,"w");assert(f);
 assert(fprintf(f,"confirm %llu\n",(unsigned long long)token)>0);assert(!fclose(f));
}
static void wait_sheet_closed(void){
 for(int i=0;i<500 && s_alert;i++)[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
 assert(!s_alert);
}
static void game_keys(NSWindow* window){
 assert(NSApp.isActive && window.isKeyWindow && NSApp.keyWindow==window);
 assert(window.firstResponder==window.contentView);
 NSEvent* down=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:7];
 [NSApp sendEvent:down];assert(ps3recomp_pad_macos_buttons()==CELL_PAD_CTRL_CROSS);
 NSEvent* up=[NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:window.windowNumber context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:7];
 [NSApp sendEvent:up];assert(!ps3recomp_pad_macos_buttons());
}
static void wait_game_focus(NSWindow* window){
 for(int i=0;i<500 && (!window.isKeyWindow || window.firstResponder!=window.contentView);i++)
  [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.002]];
 game_keys(window);
}
static void focus_game(NSWindow* window){
 if(@available(macOS 14.0,*))[NSApp activate];else [NSApp activateIgnoringOtherApps:YES];[window makeKeyAndOrderFront:nil];[window makeFirstResponder:window.contentView];for(int i=0;i<20 && (!NSApp.isActive || !window.isKeyWindow);i++)run_ui();game_keys(window);
}
static void guest_finish(int expected){
 cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==expected);cellSysutilCheckCallback();run_ui();
}
@interface FocusSentinelView : NSView
@end
@implementation FocusSentinelView
- (BOOL)acceptsFirstResponder{return YES;}
@end
static void focus_races(NSWindow* window){
 vm_write32(0x1100,0);
 NSWindow* other=[[NSWindow alloc] initWithContentRect:NSMakeRect(20,20,160,90) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];other.releasedWhenClosed=NO;
 NSTextField* otherField=[NSTextField new];other.contentView=otherField;
 FocusSentinelView* sentinel=[FocusSentinelView new];[window.contentView addSubview:sentinel];
 // Choosing another window while a real OSK is open must retain that choice.
 focus_game(window);assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert(s_alert);
 [other makeKeyAndOrderFront:nil];[other makeFirstResponder:otherField];run_ui();assert(other.isKeyWindow);NSResponder* selected=other.firstResponder;
 [s_alert.buttons[0] performClick:nil];run_ui();assert(other.isKeyWindow && other.firstResponder==selected && !window.isKeyWindow);guest_finish(0);
 // Even if the other selected window disappears, its earlier selection matters.
 focus_game(window);assert([window makeFirstResponder:sentinel]);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();
 [other makeKeyAndOrderFront:nil];run_ui();assert(other.isKeyWindow);[other orderOut:nil];run_ui();
 [s_alert.buttons[1] performClick:nil];run_ui();assert(window.firstResponder==sentinel);guest_finish(1);
 // A queued follow-up sheet keeps its own keyboard ownership.
 focus_game(window);assert([window makeFirstResponder:sentinel]);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();
 NSWindow* follow=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,160,80) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];follow.releasedWhenClosed=NO;
 [window beginSheet:follow completionHandler:^(NSModalResponse response){(void)response;}];
 [s_alert.buttons[0] performClick:nil];wait_sheet_closed();run_ui();
 assert(window.attachedSheet==follow && follow.isKeyWindow && window.firstResponder==sentinel);guest_finish(0);
 [window endSheet:follow];run_ui();[follow close];
 // Re-registering the host window invalidates restoration to the old parent.
 focus_game(window);assert([window makeFirstResponder:sentinel]);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();
 osk_native_macos_set_window((__bridge void*)other);
 [s_alert.buttons[0] performClick:nil];wait_sheet_closed();run_ui();assert(window.firstResponder==sentinel);guest_finish(0);
 osk_native_macos_set_window((__bridge void*)window);
 // Relinquish actual application ownership while the sheet is open.
 focus_game(window);assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();
 const char* other_app=getenv("OSK_FIXTURE_OTHER_APP");assert(other_app);
 if(@available(macOS 14.0,*))[NSApp yieldActivationToApplicationWithBundleIdentifier:@"org.ps3recomp.tests.nativeoskother"];
 __block NSRunningApplication* selected_app=nil;__block NSError* launch_error=nil;__block BOOL launched=NO;
 NSWorkspaceOpenConfiguration* config=[NSWorkspaceOpenConfiguration configuration];config.createsNewApplicationInstance=YES;config.arguments=@[@"--focus-other"];
 [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:other_app]] configuration:config completionHandler:^(NSRunningApplication* app,NSError* error){selected_app=app;launch_error=error;launched=YES;}];
 for(int i=0;i<20 && !launched;i++)run_ui();assert(launched && selected_app && !launch_error);
 for(int i=0;i<20 && NSApp.isActive;i++)run_ui();assert(!NSApp.isActive && selected_app.active);
 [s_alert.buttons[0] performClick:nil];run_ui();assert(!NSApp.isActive && !window.isKeyWindow);guest_finish(0);
 for(int i=0;i<20 && !selected_app.terminated;i++)run_ui();assert(selected_app.terminated);
 // A host completion caused by guest abort must not refocus a cancelled session.
 focus_game(window);assert([window makeFirstResponder:sentinel]);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert(s_alert);
 assert(!cellOskDialogAbort());run_ui();assert(!s_alert && window.firstResponder==sentinel);guest_finish(2);
 [sentinel removeFromSuperview];[other close];focus_game(window);
}
static void run_fixture(void){@autoreleasepool {
 int controlled=getenv("OSK_NATIVE_CONTROL_FILE") && *getenv("OSK_NATIVE_CONTROL_FILE");
 vm_base=calloc(1,65536);assert(vm_base);
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[NSApp finishLaunching];
 s_dev=MTLCreateSystemDefaultDevice();assert(s_dev);
 assert(!create_window_impl("OSK fixture"));NSWindow* window=s_window;window.releasedWhenClosed=NO;assert(osk_native_available());
 if(@available(macOS 14.0,*))[NSApp activate];else [NSApp activateIgnoringOtherApps:YES];[window makeKeyAndOrderFront:nil];for(int i=0;i<15 && (!NSApp.isActive || !window.isKeyWindow);i++)run_ui();fprintf(stderr,"FOCUS initial active=%d key=%d\n",NSApp.isActive,window.isKeyWindow);game_keys(window);
 vm_write32(0x1000,0x2000);vm_write32(0x1004,0x2100);vm_write32(0x1008,32);put16(0x2000,'N');put16(0x2002,0);
 const uint16_t original[]={'P','l','a','y','e','r',0};
 for(unsigned i=0;i<7;i++)put16(0x2100+i*2,original[i]);
 assert(!setenv("PS3RECOMP_OSK_INITIAL_TEXT","PlayerRetail",1));
 g_ps3_guest_caller=cb;cellSysutilRegisterCallback(1,(void*)0x3000,NULL);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));assert(!s_alert);
 if(controlled)control_command(1); // guessed pre-ready command must be discarded
 run_ui();assert(s_alert && s_alert.window.sheetParent==window && ![s_field isKindOfClass:NSSecureTextField.class]);
 assert([s_field.stringValue isEqualToString:@"PlayerRetail"] && s_field.editable);
 assert(s_alert.window.firstResponder!=window.contentView);assert(events==0);
 fprintf(stderr,"FOCUS PREOK active=%d parent=%d alert=%d\n",NSApp.isActive,window.isKeyWindow,s_alert.window.isKeyWindow);
 s_field.stringValue=@"PΩ🚀";
 if(controlled){
  assert(access(getenv("OSK_NATIVE_CONTROL_FILE"),F_OK)); // pre-ready command discarded
  uint64_t token=s_ui_token;control_command(token+1);cellOskDialog_tick();run_ui();assert(s_alert);
  control_command(token);cellOskDialog_tick();assert(s_alert && events==0);
  control_command(token);cellOskDialog_tick(); // repeated command cannot click twice
 }else [s_alert.buttons[0] performClick:nil];
 wait_sheet_closed();assert(!s_alert && events==0);wait_game_focus(window);fprintf(stderr,"FOCUS OK active=%d key=%d first=%d\n",NSApp.isActive,window.isKeyWindow,window.firstResponder==window.contentView);game_keys(window);
 cellSysutilCheckCallback();assert(events==2 && codes[0]==0x502 && codes[1]==0x503);
 result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==0 && get16(0x2200)=='P' && get16(0x2202)==0x03A9 && get16(0x2204)==0xD83D && get16(0x2206)==0xDE80 && !get16(0x2208));
 cellSysutilCheckCallback();assert(events==3 && codes[2]==0x504);run_ui();
 vm_write32(0x1100,0x20000000);assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert([s_field isKindOfClass:NSSecureTextField.class]);
 assert([s_field.stringValue isEqualToString:@"Player"]);
 [s_alert.buttons[1] performClick:nil];wait_sheet_closed();wait_game_focus(window);cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==1);cellSysutilCheckCallback();run_ui();
 if(controlled){
  assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert(s_alert);
  uint64_t cancelled=s_ui_token;control_command(cancelled);cellOskDialog_tick();assert(!cellOskDialogAbort());
  run_ui();assert(!s_alert);cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==2);cellSysutilCheckCallback();run_ui();
  assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert(s_alert);
  control_command(cancelled);cellOskDialog_tick();run_ui();assert(s_alert); // stale token must not affect next sheet
  assert(!cellOskDialogAbort());run_ui();cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==2);cellSysutilCheckCallback();run_ui();
 }
 focus_races(window);
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));assert(!cellOskDialogAbort());run_ui();assert(!s_alert);cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==2);cellSysutilCheckCallback();run_ui();
 assert(!cellOskDialogLoadAsync(-1,(void*)0x1100,(void*)0x1000));run_ui();assert(s_alert);
 [window close];assert(!window.isVisible);assert(!osk_native_available());run_ui();assert(!s_alert);
 cellSysutilCheckCallback();result();assert(!cellOskDialogUnloadAsync((void*)0x1200));assert(get32(0x1200)==1);cellSysutilCheckCallback();run_ui();assert(!window.isKeyWindow && !window.isVisible);free(vm_base);
 if(controlled)puts("Opt-in command file drove real NSAlert OK / Unicode result / queued abort / stale next-sheet checks passed");
 puts("Real native OK/Cancel restore key ownership and game keyboard; other-window/queued-sheet/replaced-parent/deactivate/abort/parent-close guards passed");
 puts("Real macOS OSK sheet / Unicode OK / secure cancel / pre-open abort / deferred sysutil checks passed");
const char* receipt=getenv("OSK_FIXTURE_RECEIPT");if(receipt){FILE* done=fopen(receipt,"w");assert(done);assert(fputs("passed\n",done)>=0);assert(!fclose(done));}
}}
int main(int argc,char** argv){@autoreleasepool {
 if(argc==2 && !strcmp(argv[1],"--focus-other")){
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  NSWindow* selected=[[NSWindow alloc] initWithContentRect:NSMakeRect(30,30,160,80) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];selected.releasedWhenClosed=NO;
  [selected makeKeyAndOrderFront:nil];
  [NSTimer scheduledTimerWithTimeInterval:.05 repeats:NO block:^(NSTimer* timer){(void)timer;if(@available(macOS 14.0,*))[NSApp activate];else [NSApp activateIgnoringOtherApps:YES];}];
  [NSTimer scheduledTimerWithTimeInterval:1.5 repeats:NO block:^(NSTimer* timer){(void)timer;if(@available(macOS 14.0,*))[NSApp yieldActivationToApplicationWithBundleIdentifier:@"org.ps3recomp.tests.nativeosk"];[NSApp terminate:nil];}];
  [NSApp run];return 0;
 }
 [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
 [NSTimer scheduledTimerWithTimeInterval:.05 repeats:NO block:^(NSTimer* timer){(void)timer;[NSApp stop:nil];run_fixture();[NSApp terminate:nil];}];
 [NSApp run];
}return 0;}
