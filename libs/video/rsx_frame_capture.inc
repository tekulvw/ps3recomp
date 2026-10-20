/* Finite completed-surface diagnostic, deliberately outside rendering timing.
 * Prefix/request must be absolute. Request contains token1..4. A new token
 * discards the current partial frame and captures the next complete interval.
 * Programs are the actual emitted RSX FP tokens, never guessed frame owners. */
#include "rsx_frame_capture_gate.h"
#include "../../runtime/diagnostics/critical_trace.h"
static struct {
 int configured,enabled;
 const char*prefix,*request,*session;
 uint64_t begin_ns,deadline_ns;
 rsx_frame_capture_gate gate;
} s_frame_capture;
static int frame_capture_enabled(void)
{
 if(!s_frame_capture.configured){
  s_frame_capture.configured=1;
  s_frame_capture.prefix=getenv("RSX_ENGINE_FRAME_CAPTURE");
  s_frame_capture.request=getenv("RSX_ENGINE_FRAME_CAPTURE_REQUEST");
  s_frame_capture.session=getenv("RSX_ENGINE_FRAME_CAPTURE_SESSION");
  int valid_session=s_frame_capture.session&&*s_frame_capture.session&&strlen(s_frame_capture.session)<=128;
  if(valid_session)for(const char*p=s_frame_capture.session;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='-'))valid_session=0;
  s_frame_capture.enabled=s_frame_capture.prefix&&s_frame_capture.prefix[0]=='/'&&
   s_frame_capture.request&&s_frame_capture.request[0]=='/'&&valid_session;
 }
 return s_frame_capture.enabled;
}
static void frame_capture_draw(uint32_t program,int fixed)
{if(frame_capture_enabled())rsx_frame_capture_draw(&s_frame_capture.gate,program,fixed);}
static void frame_capture_present_impl(uint32_t buffer,uint32_t target)
{
 if(!frame_capture_enabled())return;
 unsigned request=0;char tail;
 FILE*f=fopen(s_frame_capture.request,"r");
 if(f){if(fscanf(f,"%u %c",&request,&tail)!=1)request=0;fclose(f);}
 rsx_frame_capture_gate*c=&s_frame_capture.gate;
 const uint64_t boundary_ns=ps3_critical_now_ns();
 if(c->pending&&boundary_ns>=s_frame_capture.deadline_ns){
  fprintf(stderr,"[rsx frame capture] request=%u timeout=1 diagnostic_only=1\n",c->pending);
  rsx_frame_capture_expire(c);
 }
 unsigned previous=c->last;
 if(!rsx_frame_capture_step(c,request)){
  if(c->last!=previous)s_frame_capture.deadline_ns=boundary_ns+5000000000ull;
  /* Repeated presentations with no emitted draws do not consume a request.
   * Advance the interval boundary so a later inventory is one complete frame. */
  if(c->last!=previous||(c->pending&&!c->draws))s_frame_capture.begin_ns=boundary_ns;
  return;
 }
 const uint64_t end_ns=ps3_critical_now_ns();
 char pixels[2048],metadata[2048],pixel_meta[2048];
 if(snprintf(pixels,sizeof pixels,"%s-%u.rgba",s_frame_capture.prefix,request)>=(int)sizeof pixels||
    snprintf(metadata,sizeof metadata,"%s-%u.json",s_frame_capture.prefix,request)>=(int)sizeof metadata||
    snprintf(pixel_meta,sizeof pixel_meta,"%s.meta",pixels)>=(int)sizeof pixel_meta){rsx_frame_capture_clear(c);return;}
 /* Reserve the receipt first, then both backend paths. Never overwrite a
  * previous capture tuple, even if its JSON or association file is stale. */
 f=fopen(metadata,"wx");
 if(!f){rsx_frame_capture_clear(c);return;}
 FILE*reserved=fopen(pixels,"wx");int paths_reserved=reserved!=NULL;
 if(reserved)fclose(reserved);
 if(paths_reserved){reserved=fopen(pixel_meta,"wx");paths_reserved=reserved!=NULL;if(reserved)fclose(reserved);}
 /* A fixed fallback shader or overflowing inventory cannot certify this frame. */
 int ok=0;
 if(paths_reserved&&c->draws&&!c->fixed&&!c->overflow&&g.be->capture_texture_rgba8)
  ok=g.be->capture_texture_rgba8(g.be->user,g.surfaces[target].handle,pixels);
 const uint64_t readback_ns=ps3_critical_now_ns();
 if(f){
  fprintf(f,"{\"schema\":1,\"pid\":%d,\"session\":\"%s\",\"request\":%u,\"frame\":%u,\"buffer\":%u,\"surface\":%u,\"handle\":%u,\"location\":%u,\"offset\":%u,\"width\":%u,\"height\":%u,\"begin_ns\":%llu,\"end_ns\":%llu,\"readback_end_ns\":%llu,\"saved\":%s,\"draws\":%u,\"fixed_shader_draws\":%u,\"inventory_overflow\":%s,\"programs\":[",
   getpid(),s_frame_capture.session,request,g.frames,buffer,target,g.surfaces[target].handle,g.surfaces[target].location,g.surfaces[target].offset,
   g.surfaces[target].w,g.surfaces[target].h,(unsigned long long)s_frame_capture.begin_ns,
   (unsigned long long)end_ns,(unsigned long long)readback_ns,ok?"true":"false",c->draws,c->fixed,c->overflow?"true":"false");
  for(unsigned i=0;i<c->unique;i++)fprintf(f,"%s{\"token\":%u,\"draws\":%u}",i?",":"",c->programs[i],c->counts[i]);
  fprintf(f,"]}\n");fclose(f);
 }
 fprintf(stderr,"[rsx frame capture] request=%u frame=%u saved=%d draws=%u fixed=%u overflow=%u diagnostic_wait=1\n",request,g.frames,ok,c->draws,c->fixed,c->overflow);
 rsx_frame_capture_clear(c);
}

static void frame_capture_present(uint32_t buffer,uint32_t target)
{const int saved=errno;frame_capture_present_impl(buffer,target);errno=saved;}
