/* Default-off diagnostic request lifecycle; renderer-thread owned. */
#ifndef RSX_FRAME_CAPTURE_GATE_H
#define RSX_FRAME_CAPTURE_GATE_H
#include <stdint.h>
typedef struct {
 unsigned last,pending,attempts,draws,unique,fixed,overflow,limit;
 uint32_t programs[1024],counts[1024];
} rsx_frame_capture_gate;
static inline void rsx_frame_capture_clear(rsx_frame_capture_gate*g)
{g->draws=g->unique=g->fixed=g->overflow=0;}
/* Strict finite diagnostic override; absent preserves the original four. */
static inline unsigned rsx_frame_capture_limit(const char*text)
{
 if(!text)return 4;
 if(!*text||*text=='0')return 0;
 unsigned value=0;
 for(const char*p=text;*p;p++){
  if(*p<'0'||*p>'9')return 0;
  value=value*10u+(unsigned)(*p-'0');if(value>12)return 0;
 }
 return value;
}
/* A new token only starts tracking at a present boundary. The following
 * boundary owns a complete interval. Stale/out-of-budget tokens never arm. */
static inline int rsx_frame_capture_step(rsx_frame_capture_gate*g,unsigned request)
{
 const unsigned limit=g->limit?g->limit:4;
 if(limit>12)return 0;
 if(request && request==g->pending){if(!g->draws)return 0;g->pending=0;++g->attempts;return 1;}
 if(request>g->last && request<=limit && g->attempts<limit){
  g->last=request;g->pending=request;rsx_frame_capture_clear(g);
 }else if(g->pending){g->pending=0;rsx_frame_capture_clear(g);}
 return 0;
}
static inline void rsx_frame_capture_expire(rsx_frame_capture_gate*g)
{if(g->pending){g->pending=0;++g->attempts;rsx_frame_capture_clear(g);}}
static inline void rsx_frame_capture_draw(rsx_frame_capture_gate*g,uint32_t program,int fixed)
{
 if(!g->pending)return;
 ++g->draws;if(fixed)++g->fixed;
 for(unsigned i=0;i<g->unique;i++)if(g->programs[i]==program){++g->counts[i];return;}
 if(g->unique==1024){g->overflow=1;return;}
 g->programs[g->unique]=program;g->counts[g->unique++]=1;
}
#endif
