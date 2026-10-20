#include <assert.h>
#include <stdio.h>
#include "rsx_frame_capture_gate.h"
int main(void) {
 rsx_frame_capture_gate g={0};
 assert(!rsx_frame_capture_step(&g,0));
 assert(!rsx_frame_capture_step(&g,5));
 assert(!rsx_frame_capture_step(&g,1)); /* arm never captures a partial frame */
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
 puts("frame capture: whole-frame arm, stale/invalid/replaced requests, exact programs, overflow and finite budget PASS");
}
