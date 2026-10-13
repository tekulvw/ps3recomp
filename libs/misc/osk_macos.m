#import <AppKit/AppKit.h>
#include "osk_native.h"
#include <stdatomic.h>
/* All AppKit objects remain on the main thread. The guest reads only this bit. */
static _Atomic(int) s_available;
static __weak NSWindow* s_parent;
static NSAlert* s_alert;
static NSTextField* s_field;
static uint64_t s_ui_token;
static id s_close_observer;
void osk_native_macos_set_window(void* window){
 NSCAssert([NSThread isMainThread],@"OSK window registration must be on main thread");
 if(s_close_observer)[[NSNotificationCenter defaultCenter] removeObserver:s_close_observer];
 s_parent=(__bridge NSWindow*)window;
 atomic_store(&s_available,s_parent!=nil);
 if(s_parent)s_close_observer=[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:s_parent queue:nil usingBlock:^(NSNotification* n){
  (void)n;atomic_store(&s_available,0);s_parent=nil;
  if(s_alert)[NSApp endSheet:s_alert.window returnCode:NSModalResponseCancel];
 }];
}
int osk_native_available(void){return atomic_load(&s_available);}
static NSString* string16(const uint16_t* p){
 NSUInteger n=0;while(n<512 && p[n])n++;
 return [[NSString alloc] initWithCharacters:p length:n];
}
void osk_native_open(const OskNativeRequest* request){
 OskNativeRequest copy=*request;
 dispatch_async(dispatch_get_main_queue(),^{
  if(!osk_native_is_pending(copy.token))return;
  if(!s_parent || !atomic_load(&s_available)){ /* Parent closed before dispatch. */
   osk_native_host_closed(copy.token);return;
  }
  s_ui_token=copy.token;s_alert=[NSAlert new];
  s_alert.messageText=@"Game text input";
  s_alert.informativeText=string16(copy.message);
  [s_alert addButtonWithTitle:@"OK"];[s_alert addButtonWithTitle:@"Cancel"];
  s_field=(copy.panels&0x20000000u)?[NSSecureTextField new]:[NSTextField new];
  s_field.frame=NSMakeRect(0,0,360,24);s_field.stringValue=string16(copy.initial);
  s_alert.accessoryView=s_field;
  NSAlert* alert=s_alert;NSTextField* field=s_field;NSWindow* parent=s_parent;
  [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){
   NSString* text=field.stringValue;NSUInteger n=MIN(text.length,(NSUInteger)512);
   uint16_t units[513]={0};[text getCharacters:units range:NSMakeRange(0,n)];
   osk_native_did_finish(copy.token,response==NSAlertFirstButtonReturn?0:1,units,(uint32_t)n);
   if(s_ui_token==copy.token){s_alert=nil;s_field=nil;s_ui_token=0;}
   if(!parent.attachedSheet)[parent makeFirstResponder:parent.contentView];
  }];
  [alert.window makeFirstResponder:field];
  osk_native_did_open(copy.token);
 });
}
void osk_native_close(uint64_t token){
 dispatch_async(dispatch_get_main_queue(),^{
  if(s_alert && s_ui_token==token)[NSApp endSheet:s_alert.window returnCode:NSModalResponseCancel];
 });
}

void osk_native_confirm(uint64_t token){
 dispatch_async(dispatch_get_main_queue(),^{
  /* Abort/parent close may race with the guest poll's queued action. */
  if(s_alert && s_ui_token==token && s_alert.window.sheetParent==s_parent
     && s_parent && atomic_load(&s_available) && osk_native_is_open(token))
   [s_alert.buttons[0] performClick:nil];
 });
}
