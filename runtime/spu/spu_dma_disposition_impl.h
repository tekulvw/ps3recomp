/* Compiled once alongside the publication observer; no gameplay state writes. */
#ifndef SPU_DMA_DISPOSITION_IMPL_H
#define SPU_DMA_DISPOSITION_IMPL_H
#include "spu_dma_disposition.h"
#include "../platform/win32_compat.h"
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <time.h>
#endif
static struct {
    INIT_ONCE once;
    volatile LONG ready, emitted, armed;
    volatile LONG64 next_poll;
    uint64_t lo, hi;
    uint32_t words, limit;
    int enabled;
    char trigger[129];
} s_dma_disposition = {INIT_ONCE_STATIC_INIT,0,0,0,0,0,0,16,128,0,{0}};
static int dma_disp_number(const char* s,int base,uint64_t max,uint64_t* out) {
    char* end;
    if(!s || !*s || !(base==16?isxdigit((unsigned char)*s):isdigit((unsigned char)*s))) return 0;
    errno=0; unsigned long long n=strtoull(s,&end,base);
    if(errno || end==s || *end || n>max) return 0;
    *out=n; return 1;
}
static BOOL WINAPI dma_disp_init(PINIT_ONCE once,PVOID arg,PVOID* out) {
    (void)once; (void)arg; (void)out;
    const char* span=getenv("SPU_DMA_DISPOSITION");
    const char* words=getenv("SPU_DMA_DISPOSITION_WORDS");
    const char* limit=getenv("SPU_DMA_DISPOSITION_LIMIT");
    const char* trigger=getenv("SPU_DMA_DISPOSITION_TRIGGER");
    uint64_t n; int valid=0; char first[32]; const char* dash=span?strchr(span,'-'):NULL;
    if(dash && dash!=span && (size_t)(dash-span)<sizeof first) {
        memcpy(first,span,(size_t)(dash-span)); first[dash-span]=0;
        valid=dma_disp_number(first,16,0xFFFFFFFFull,&s_dma_disposition.lo) &&
            dma_disp_number(dash+1,16,0x100000000ull,&s_dma_disposition.hi) && s_dma_disposition.lo<s_dma_disposition.hi;
    }
    if(words) { int ok=dma_disp_number(words,10,16,&n)&&n; valid=valid&&ok; if(ok)s_dma_disposition.words=(uint32_t)n; }
    if(limit) { int ok=dma_disp_number(limit,10,32768,&n)&&n; valid=valid&&ok; if(ok)s_dma_disposition.limit=(uint32_t)n; }
    if(trigger) { size_t len=strlen(trigger); if(!len || len>128)valid=0; else memcpy(s_dma_disposition.trigger,trigger,len+1); }
    else InterlockedExchange(&s_dma_disposition.armed,1);
    s_dma_disposition.enabled=valid;
    InterlockedExchange(&s_dma_disposition.ready,1); return TRUE;
}
static uint64_t dma_disp_milliseconds(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
static int dma_disp_armed(void) {
    if(InterlockedCompareExchange(&s_dma_disposition.armed,0,0))return 1;
    LONG64 now=(LONG64)dma_disp_milliseconds();
    LONG64 next=InterlockedCompareExchange64(&s_dma_disposition.next_poll,0,0);
    if(now<next || InterlockedCompareExchange64(&s_dma_disposition.next_poll,now+250,next)!=next)return 0;
#ifdef _WIN32
    struct _stat64 st;
    int exists=!_stat64(s_dma_disposition.trigger,&st) && (st.st_mode&_S_IFMT)==_S_IFREG;
#else
    struct stat st;
    int exists=!stat(s_dma_disposition.trigger,&st) && S_ISREG(st.st_mode);
#endif
    if(exists)InterlockedExchange(&s_dma_disposition.armed,1);
    return exists;
}
void spu_dma_disposition_begin(spu_dma_disposition_token* t,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd) {
    int saved=errno;
#ifdef _WIN32
    DWORD last=GetLastError();
#endif
    t->seq=0;
    if(!InterlockedCompareExchange(&s_dma_disposition.ready,0,0))InitOnceExecuteOnce(&s_dma_disposition.once,dma_disp_init,NULL,NULL);
    if(s_dma_disposition.enabled && ea<=0xFFFFFFFFull && (uint64_t)size<=0x100000000ull-ea &&
       ea<s_dma_disposition.hi && ea+size>s_dma_disposition.lo &&
       (uint32_t)InterlockedCompareExchange(&s_dma_disposition.emitted,0,0)<s_dma_disposition.limit && dma_disp_armed()) {
        LONG n=InterlockedCompareExchange(&s_dma_disposition.emitted,0,0);
        while((uint32_t)n<s_dma_disposition.limit) {
            LONG prior=InterlockedCompareExchange(&s_dma_disposition.emitted,n+1,n);
            if(prior==n) { t->seq=(uint32_t)n+1; t->request_lsa=lsa; t->request_ea=ea; t->request_size=size; t->request_cmd=cmd; t->count=0; t->offset=0; break; }
            n=prior;
        }
    }
    errno=saved;
#ifdef _WIN32
    SetLastError(last);
#endif
}
static uint32_t dma_disp_read(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
void spu_dma_disposition_before(spu_dma_disposition_token* t,const uint8_t* src,uint64_t ea,uint32_t size) {
    if(!t->seq)return;
    t->count=0;
    if(!size || size>SPU_LS_SIZE || ea>0xFFFFFFFFull || size>0x100000000ull-ea)return;
    uint64_t lo=ea>s_dma_disposition.lo?ea:s_dma_disposition.lo;
    uint64_t hi=ea+size<s_dma_disposition.hi?ea+size:s_dma_disposition.hi;
    lo=(lo+3)&~3ull; hi&=~3ull;
    if(lo>=hi)return;
    t->offset=(uint32_t)(lo-ea); t->count=(uint32_t)((hi-lo)/4);
    if(t->count>s_dma_disposition.words)t->count=s_dma_disposition.words;
    for(uint32_t i=0;i<t->count;i++)t->before[i]=dma_disp_read(src+t->offset+i*4);
}
void spu_dma_disposition_after(spu_dma_disposition_token* t,const uint8_t* dst) {
    if(!t->seq)return;
    for(uint32_t i=0;i<t->count;i++)t->after[i]=dma_disp_read(dst+t->offset+i*4);
}
int spu_dma_disposition_finish(const spu_dma_disposition_token* t,const spu_context* s,uint32_t lsa,uint64_t ea,uint32_t size,uint32_t cmd,int rc,unsigned outcome) {
    if(!t->seq)return rc;
    int saved=errno;
#ifdef _WIN32
    DWORD last=GetLastError();
#endif
    const char* names[]={"no-copy","copied-get","copied-put","zeroed-ea","zeroed-ls","rejected","malformed-no-copy"};
    char before[145]={0}, after[145]={0}; size_t a=0,b=0;
    for(uint32_t i=0;i<t->count;i++) {
        a+=(size_t)snprintf(before+a,sizeof before-a,"%s%08X",i?",":"",t->before[i]);
        b+=(size_t)snprintf(after+b,sizeof after-b,"%s%08X",i?",":"",t->after[i]);
    }
    fprintf(stderr,"[dma-disposition] seq=%u ctx=%p img=%d spu=%u pc=%05X r0=%08X r1=%08X request_lsa=%05X request_ea=%016llX request_size=%u request_cmd=%X lsa=%05X ea=%016llX size=%u cmd=%X rc=%d disposition=%s copied=%u sample_offset=%u words=%u before=%s after=%s\n",
        t->seq,(const void*)s,s->image_id,s->spu_id,s->pc&SPU_LS_MASK,s->gpr[0]._u32[0],s->gpr[1]._u32[0],t->request_lsa,(unsigned long long)t->request_ea,t->request_size,t->request_cmd,lsa,(unsigned long long)ea,size,cmd,rc,outcome<7?names[outcome]:"unknown",outcome==SPU_DMA_DISPOSITION_GET || outcome==SPU_DMA_DISPOSITION_PUT,t->offset,t->count,before,after);
    errno=saved;
#ifdef _WIN32
    SetLastError(last);
#endif
    return rc;
}
#endif
