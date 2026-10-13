/* Single-window OSK lifecycle. Host completions never touch guest memory or
 * the sysutil event queue; the guest's normal CheckCallback poll does that. */
#include "osk_native.h"
#ifdef PS3RECOMP_NATIVE_OSK
#include "cellOskDialog.h"
#include "../system/cellSysutil.h"
#include "../../runtime/ppu/ppu_memory.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
extern uint32_t ppu_vm_size; /* zero denotes the whole 32-bit mapping */
#define OSK_PARAM ((int32_t)0x8002B504)
#define OSK_UNKNOWN ((int32_t)0x8002B503)
#define OSK_BUSY ((int32_t)0x8002B105)
#define OSK_NOT_OPEN ((int32_t)0x8002B310)
enum {FREE,LOADING,OPEN,FINISHED,UNLOADING};
static atomic_flag s_lock=ATOMIC_FLAG_INIT;
static struct {
 int state, initial_text_used; uint64_t token; uint32_t limit, prohibit, events;
 int32_t result; uint32_t length; uint16_t text[513];
} s;
static void lock(void){while(atomic_flag_test_and_set_explicit(&s_lock,memory_order_acquire)){} }
static void unlock(void){atomic_flag_clear_explicit(&s_lock,memory_order_release);}
static int valid(uint32_t ea,uint64_t bytes){
 uint64_t end=(uint64_t)ea+bytes;
 return vm_base && ea && end<=0x100000000ull && (!ppu_vm_size || end<=ppu_vm_size);
}
static int guest_text(uint32_t ea,uint16_t* out,uint32_t cap){
 if(!ea) return 0;
 for(uint32_t i=0;i<cap;i++){
  if(!valid(ea+i*2,2)) return 0;
  out[i]=vm_read16(ea+i*2);if(!out[i]) return 1;
 }
 out[cap]=0;return 1;
}
/* The explicit host default is UTF-8. Validate the whole value before copying
 * any of it, including a suffix beyond the guest's UTF-16 content limit. */
static void host_initial_text(uint16_t* initial,uint32_t limit){
 const unsigned char* p=(const unsigned char*)getenv("PS3RECOMP_OSK_INITIAL_TEXT");
 if(!p)return;
 uint16_t text[513]={0};uint32_t n=0;int truncated=0;
 while(*p){
  uint32_t cp=*p++,minimum=0;unsigned remaining=0;
  if(cp<0x80){}
  else if(cp>=0xC2 && cp<=0xDF){cp&=0x1F;remaining=1;minimum=0x80;}
  else if(cp>=0xE0 && cp<=0xEF){cp&=0x0F;remaining=2;minimum=0x800;}
  else if(cp>=0xF0 && cp<=0xF4){cp&=7;remaining=3;minimum=0x10000;}
  else return;
  for(unsigned i=0;i<remaining;i++){
   if((*p&0xC0)!=0x80)return;
   cp=(cp<<6)|(*p++&0x3F);
  }
  if(cp<minimum || cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF))return;
  uint32_t units=cp>0xFFFF?2:1;
  if(truncated || n+units>limit){truncated=1;continue;}
  if(units==2){cp-=0x10000;text[n++]=(uint16_t)(0xD800+(cp>>10));text[n++]=(uint16_t)(0xDC00+(cp&0x3FF));}
  else text[n++]=(uint16_t)cp;
 }
 memcpy(initial,text,(n+1)*sizeof(text[0]));
}
int32_t osk_native_load(uint32_t param,uint32_t input){
 if(!valid(param,20)||!valid(input,12)) return OSK_PARAM;
 OskNativeRequest request={0};
 uint32_t limit=vm_read32(input+8);
 if(limit>512 || !guest_text(vm_read32(input),request.message,512)
    || !guest_text(vm_read32(input+4),request.initial,512)) return OSK_PARAM;
 request.limit=limit<512?limit:511;
 request.panels=vm_read32(param);request.prohibit=vm_read32(param+16);
 uint32_t initial_end=request.limit;
 if(initial_end && request.initial[initial_end-1]>=0xD800 && request.initial[initial_end-1]<=0xDBFF)initial_end--;
 request.initial[initial_end]=0;
 if(!osk_native_available()) return OSK_UNKNOWN;
 lock();if(s.state!=FREE){unlock();return OSK_BUSY;}
 /* One attempt per process, at the first accepted non-password dialog. The
  * ordinary editable UI and confirmation lifecycle remain responsible for
  * returning text; neither secure nor subsequent requests are overridden. */
 if(!s.initial_text_used && !(request.panels&0x20000000u)){
  s.initial_text_used=1;host_initial_text(request.initial,request.limit);
 }
 request.token=++s.token;s.state=LOADING;s.limit=request.limit;s.prohibit=request.prohibit;
 s.events=0;s.result=3;s.length=0;memset(s.text,0,sizeof(s.text));unlock();
 printf("[cellOskDialog] LoadAsync native token=%llu limit=%u panels=0x%X\n",
  (unsigned long long)request.token,request.limit,request.panels);
 osk_native_open(&request);return 0;
}
int osk_native_is_pending(uint64_t token){
 lock();int yes=token==s.token && s.state==LOADING;unlock();return yes;
}
void osk_native_did_open(uint64_t token){
 lock();if(token==s.token && s.state==LOADING){s.state=OPEN;s.events|=1;}unlock();
}
void osk_native_did_finish(uint64_t token,int32_t result,const uint16_t* text,uint32_t length){
 lock();
 if(token==s.token && s.state==OPEN && (result==0||result==1)){
  s.result=result;s.length=0;
  if(result==0 && text){
   uint32_t n=length<s.limit?length:s.limit;
   /* Avoid splitting a UTF-16 surrogate pair when enforcing the length. */
   if(n && n<length && text[n-1]>=0xD800 && text[n-1]<=0xDBFF) n--;
   for(uint32_t i=0;i<n && text[i];i++){
    if((s.prohibit&1)&&text[i]==' ') continue;
    if((s.prohibit&2)&&(text[i]=='\n'||text[i]=='\r')) continue;
    s.text[s.length++]=text[i];
   }
  }
  s.text[s.length]=0;s.state=FINISHED;s.events|=2;
 }
 unlock();
}
int osk_native_has_ui_result(uint64_t token){
 lock();int yes=token==s.token && (s.result==0 || s.result==1)
  && (s.state==FINISHED || s.state==UNLOADING || s.state==FREE);unlock();return yes;
}
static int32_t result_text(uint32_t ea,int unloading,uint64_t* closing_token){
 if(!valid(ea,12)) return OSK_PARAM;
 uint32_t capacity=vm_read32(ea+4),str=vm_read32(ea+8);
 if(capacity>0x7FFFFFFFu) return OSK_PARAM;
 lock();
 if(s.state==FREE || s.state==UNLOADING){unlock();return OSK_NOT_OPEN;}
 if(unloading && s.state!=FINISHED){unlock();return OSK_BUSY;}
 /* Normal single-window GetInputText has no committed text until unload. */
 int32_t result=unloading?s.result:3;
 uint32_t n=s.length<capacity?s.length:capacity;
 if(n>511)n=511;
 if(result==0 && str && !valid(str,(uint64_t)(n+1)*2)){unlock();return OSK_PARAM;}
 if(result==0 && str){
  if(n && n<s.length && s.text[n-1]>=0xD800 && s.text[n-1]<=0xDBFF)n--;
  for(uint32_t i=0;i<n;i++) vm_write16(str+i*2,s.text[i]);
  vm_write16(str+n*2,0);
 }
 vm_write32(ea,(uint32_t)result); /* numChars is input capacity, left intact */
 if(unloading){s.state=UNLOADING;s.events|=4;*closing_token=s.token;}
 unlock();return 0;
}
int32_t osk_native_get_text(uint32_t ea){return result_text(ea,0,NULL);}
int32_t osk_native_unload(uint32_t ea){
 uint64_t token=0;
 int32_t result=result_text(ea,1,&token);
 if(!result)osk_native_close(token);
 return result;
}
void osk_native_host_closed(uint64_t token){
 lock();
 if(token==s.token && (s.state==LOADING || s.state==OPEN)){
  s.result=2;s.length=0;s.text[0]=0;s.state=FINISHED;s.events|=2;
 }
 unlock();
}
int32_t osk_native_abort(void){
 lock();if(s.state!=LOADING && s.state!=OPEN){unlock();return OSK_NOT_OPEN;}
 uint64_t token=s.token;s.result=2;s.length=0;s.text[0]=0;s.state=FINISHED;s.events|=2;unlock();
 osk_native_close(token);return 0;
}
void cellOskDialog_tick(void){
 lock();uint32_t events=s.events;s.events=0;
 if(events&4)s.state=FREE;
 unlock();
 const uint32_t codes[]={0x502,0x503,0x504};
 for(unsigned e=0;e<3;e++) if(events&(1u<<e)){
  printf("[cellOskDialog] sysutil status=0x%X\n",codes[e]);
  for(int slot=0;slot<CELL_SYSUTIL_MAX_CALLBACKS;slot++)cellSysutilQueueEvent(slot,codes[e],0);
 }
}
#else
void cellOskDialog_tick(void){}
#endif
