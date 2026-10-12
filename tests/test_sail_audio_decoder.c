#include "../libs/codec/sail_video_decoder.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#ifndef SAIL_AUDIO_HAS_PCM
typedef struct SailAudioPCM {float* samples;uint32_t channels,rate,count;uint64_t pts;} SailAudioPCM;
__attribute__((weak)) int sail_audio_decode_pcm(FILE*f,uint64_t o,uint64_t n,unsigned s,SailAudioPCM*a){(void)f;(void)o;(void)n;(void)s;(void)a;return SAIL_VIDEO_UNAVAILABLE;}
__attribute__((weak)) void sail_audio_pcm_release(SailAudioPCM*a){(void)a;}
#endif
int main(int argc,char**argv){assert(argc==4);FILE*f=fopen(argv[1],"rb");assert(f);unsigned char h[16];assert(fread(h,1,16,f)==16);uint64_t off=0,len=0;for(int i=8;i<12;i++)off=(off<<8)|h[i];for(int i=12;i<16;i++)len=(len<<8)|h[i];off*=2048;len*=2048;assert(!fseek(f,7,SEEK_SET));SailAudioPCM a={0};
 assert(sail_audio_decode_pcm(f,off,len,0,&a)==0);assert(ftell(f)==7);assert(a.samples&&a.channels==(unsigned)atoi(argv[2])&&a.count==(unsigned)atoi(argv[3])&&a.rate==48000&&a.pts==85470);double sum=0;for(uint64_t i=0;i<(uint64_t)a.count*a.channels;i++){assert(isfinite(a.samples[i]));sum+=fabs(a.samples[i]);}assert(sum>1);printf("PASS actual ATRAC PCM channels=%u count=%u pts=%llu abs_sum=%.9f\n",a.channels,a.count,(unsigned long long)a.pts,sum);sail_audio_pcm_release(&a);assert(!a.samples&&!a.count);
 assert(sail_audio_decode_pcm(f,UINT64_MAX,len,0,&a)<0&&!a.samples);assert(sail_audio_decode_pcm(f,off,len+2048,0,&a)<0&&!a.samples);assert(sail_audio_decode_pcm(f,off,len,10,&a)<0&&!a.samples);assert(sail_audio_decode_pcm(f,off,23,0,&a)<0&&!a.samples);assert(ftell(f)==7);assert(!fclose(f));return 0;}
