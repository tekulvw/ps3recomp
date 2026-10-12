#include "../libs/codec/sail_video_decoder.h"
#include <assert.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
#include <stdlib.h>
/* Baseline-compatible API declarations: a missing streaming decoder fails
 * behavioral availability before any output is accepted. */
#ifndef SAIL_VIDEO_HAS_STREAMING
#define SAIL_VIDEO_EOF 1
typedef struct SailVideoDecoder SailVideoDecoder;
__attribute__((weak)) int sail_video_open(FILE*f,uint64_t o,uint64_t n,unsigned s,SailVideoDecoder**d){(void)f;(void)o;(void)n;(void)s;(void)d;return SAIL_VIDEO_UNAVAILABLE;}
__attribute__((weak)) int sail_video_next(SailVideoDecoder*d,SailVideoFrame*f){(void)d;(void)f;return SAIL_VIDEO_UNAVAILABLE;}
__attribute__((weak)) void sail_video_close(SailVideoDecoder*d){(void)d;}
#endif
int main(int argc,char**argv) {
 assert(argc==5);FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,7,SEEK_SET));
 uint64_t off=strtoull(argv[2],0,0),len=strtoull(argv[3],0,0);int expected=atoi(argv[4]);SailVideoDecoder*d=0;
 if(expected==-2){assert(sail_video_open(f,off,len,0x1e1,&d)==SAIL_VIDEO_NOT_FOUND&&!d);assert(ftell(f)==7);assert(!fclose(f));return 0;}
 if(expected==-3){
  assert(sail_video_open(f,UINT64_MAX,len,0x1e0,&d)==SAIL_VIDEO_INVALID&&!d);
  assert(sail_video_open(f,off,(512ull<<20)+1,0x1e0,&d)==SAIL_VIDEO_INVALID&&!d);
  assert(sail_video_open(f,off,len+1,0x1e0,&d)==SAIL_VIDEO_INVALID&&!d);
  assert(sail_video_open(f,off,len,0,&d)==SAIL_VIDEO_INVALID&&!d);
  assert(sail_video_next(NULL,&(SailVideoFrame){0})==SAIL_VIDEO_INVALID);
  sail_video_close(NULL);assert(ftell(f)==7);assert(!fclose(f));return 0;
 }
 int opened=sail_video_open(f,off,len,0x1e0,&d);
 if(expected==-1 && opened<0){assert(!d);assert(ftell(f)==7);assert(!fclose(f));return 0;}
 assert(opened==SAIL_VIDEO_OK && d);
 SailVideoFrame out={0};int rc,count=0;
 while((rc=sail_video_next(d,&out))==SAIL_VIDEO_OK){
  assert(out.rgba&&out.ordinal==(unsigned)++count);assert(out.rate_num==30000&&out.rate_den==1001);
  assert(out.time_base_num==1&&out.time_base_den==90000);if(expected>0)assert(out.pts_valid==(count<expected));
  struct AVSHA*sha=av_sha_alloc();assert(sha);assert(!av_sha_init(sha,256));unsigned char sum[32];av_sha_update(sha,out.rgba,out.pitch*out.height);av_sha_final(sha,sum);av_free(sha);
  printf("%u %lld %d",out.ordinal,(long long)out.pts,out.pts_valid);
#ifdef SAIL_VIDEO_HAS_STREAMING
  printf(" %lld ",(long long)out.duration);assert(out.duration==3003);
#endif
  for(unsigned i=0;i<32;i++)printf("%02x",sum[i]);puts("");sail_video_frame_release(&out);
 }
 if(expected>0){assert(rc==SAIL_VIDEO_EOF&&count==expected&&!out.rgba);assert(sail_video_next(d,&out)==SAIL_VIDEO_EOF);}
 else{assert(rc<0 && !out.rgba);assert(sail_video_next(d,&out)==rc);}
 sail_video_close(d);assert(ftell(f)==7);assert(!fclose(f));
}
