/* Default-off diagnostic request lifecycle; renderer-thread owned. */
#ifndef RSX_FRAME_CAPTURE_GATE_H
#define RSX_FRAME_CAPTURE_GATE_H
#include <stdint.h>
typedef struct {
 unsigned last,pending,attempts,draws,unique,fixed,overflow;
 uint32_t programs[1024],counts[1024];
} rsx_frame_capture_gate;
static inline void rsx_frame_capture_clear(rsx_frame_capture_gate*g)
{g->draws=g->unique=g->fixed=g->overflow=0;}
/* A new token only starts tracking at a present boundary. The following
 * boundary owns a complete interval. Stale/out-of-budget tokens never arm. */
static inline int rsx_frame_capture_step(rsx_frame_capture_gate*g,unsigned request)
{
 if(request && request==g->pending){g->pending=0;++g->attempts;return 1;}
 if(request>g->last && request<=4 && g->attempts<4){
  g->last=request;g->pending=request;rsx_frame_capture_clear(g);
 }else if(g->pending){g->pending=0;rsx_frame_capture_clear(g);}
 return 0;
}
static inline void rsx_frame_capture_draw(rsx_frame_capture_gate*g,uint32_t program,int fixed)
{
 if(!g->pending)return;
 ++g->draws;if(fixed)++g->fixed;
 for(unsigned i=0;i<g->unique;i++)if(g->programs[i]==program){++g->counts[i];return;}
 if(g->unique==1024){g->overflow=1;return;}
 g->programs[g->unique]=program;g->counts[g->unique++]=1;
}
#endif
