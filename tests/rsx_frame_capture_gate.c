#include <assert.h>
#include <stdio.h>
#include "rsx_frame_capture_gate.h"
int main(void) {
 assert(rsx_frame_capture_limit(NULL)==4);
 assert(rsx_frame_capture_limit("1")==1&&rsx_frame_capture_limit("4")==4&&rsx_frame_capture_limit("12")==12);
 const char*bad[]={"","0","00","04","13","999999999999","-1","+12","12 "," 12","12x"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++)assert(!rsx_frame_capture_limit(bad[i]));
 rsx_frame_capture_gate extended={0};extended.limit=12;
 for(unsigned token=1;token<=12;token++){
  assert(!rsx_frame_capture_step(&extended,token));assert(extended.pending==token);
  assert(!rsx_frame_capture_step(&extended,token)); /* Empty intervals retain request. */
  if(token==5)rsx_frame_capture_expire(&extended);
  else {rsx_frame_capture_draw(&extended,0x102,0);assert(rsx_frame_capture_step(&extended,token));rsx_frame_capture_clear(&extended);}
  assert(extended.attempts==token&&!extended.pending);
 }
 assert(!rsx_frame_capture_step(&extended,13)&&!extended.pending);
 assert(!rsx_frame_capture_step(&extended,1)&&!extended.pending);
 rsx_frame_capture_gate invalid={0};invalid.limit=13;assert(!rsx_frame_capture_step(&invalid,1)&&!invalid.pending);

 rsx_frame_capture_gate g={0};
 assert(!rsx_frame_capture_step(&g,0));
 assert(!rsx_frame_capture_step(&g,5));
 assert(!rsx_frame_capture_step(&g,1)); /* arm never captures a partial frame */
 for(unsigned empty=0;empty<3;empty++){assert(!rsx_frame_capture_step(&g,1));assert(g.pending==1&&!g.attempts);}
 rsx_frame_capture_draw(&g,0x102,0);rsx_frame_capture_draw(&g,0x102,0);rsx_frame_capture_draw(&g,0x202,1);
 assert(g.draws==3 && g.unique==2 && g.fixed==1 && g.counts[0]==2);
 assert(rsx_frame_capture_step(&g,1));assert(g.attempts==1);
 rsx_frame_capture_clear(&g); assert(!rsx_frame_capture_step(&g,1));
 assert(!rsx_frame_capture_step(&g,2));
 assert(!rsx_frame_capture_step(&g,3)); /* replacement discards the incomplete request */
 rsx_frame_capture_clear(&g);rsx_frame_capture_draw(&g,0x302,0);
 assert(rsx_frame_capture_step(&g,3));rsx_frame_capture_clear(&g);
 assert(!rsx_frame_capture_step(&g,2)); /* stale tokens cannot re-arm */
 assert(!rsx_frame_capture_step(&g,4));
 for(unsigned i=0;i<1025;i++)rsx_frame_capture_draw(&g,i*4+2,0);
 assert(g.overflow && g.unique==1024 && g.draws==1025);
 assert(rsx_frame_capture_step(&g,4));rsx_frame_capture_clear(&g);
 assert(!rsx_frame_capture_step(&g,4)); assert(!rsx_frame_capture_step(&g,99));
 rsx_frame_capture_gate cancelled={0};
 assert(!rsx_frame_capture_step(&cancelled,1));
 rsx_frame_capture_draw(&cancelled,0x102,0);
 assert(!rsx_frame_capture_step(&cancelled,0));assert(!cancelled.pending && !cancelled.draws);
 assert(!rsx_frame_capture_step(&cancelled,1));assert(!cancelled.pending);
 rsx_frame_capture_gate timeout={0};assert(!rsx_frame_capture_step(&timeout,1));
 rsx_frame_capture_expire(&timeout);assert(!timeout.pending&&timeout.attempts==1);
 rsx_frame_capture_draw(&timeout,0x102,0);assert(!timeout.draws);
 assert(!rsx_frame_capture_step(&timeout,1));
 puts("frame capture: whole-frame arm, stale/invalid/replaced requests, exact programs, overflow and finite budget PASS");
}
