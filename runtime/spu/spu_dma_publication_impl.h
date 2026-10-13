/* Compiled once by spu_channels.c; standalone DMA fixtures use the same code. */
#ifndef SPU_DMA_PUBLICATION_IMPL_H
#define SPU_DMA_PUBLICATION_IMPL_H
#include "spu_dma_publication.h"
#include "../platform/win32_compat.h"
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {
    INIT_ONCE once;
    volatile LONG ready, emitted;
    uint64_t lo, hi, match_lo, match_hi;
    uint32_t word, words, limit;
    int span, match;
} s_dma_publication = {INIT_ONCE_STATIC_INIT,0,0,0,0,0,0,0,16,128,0,0};
static int dma_pub_number(const char* s, int base, uint64_t max, uint64_t* out) {
    char* end;
    if (!s || !*s || !(base==16 ? isxdigit((unsigned char)*s) : isdigit((unsigned char)*s))) return 0;
    errno=0; unsigned long long n=strtoull(s,&end,base);
    if (errno || end==s || *end || n>max) return 0;
    *out=n; return 1;
}
static int dma_pub_range(const char* s, uint64_t* lo, uint64_t* hi) {
    char first[32]; const char* dash=s ? strchr(s,'-') : NULL;
    if (!dash || dash==s || (size_t)(dash-s)>=sizeof first) return 0;
    memcpy(first,s,(size_t)(dash-s)); first[dash-s]=0;
    return dma_pub_number(first,16,0xFFFFFFFFull,lo) &&
        dma_pub_number(dash+1,16,0x100000000ull,hi) && *lo<*hi;
}
static BOOL WINAPI dma_pub_init(PINIT_ONCE once,PVOID arg,PVOID* out) {
    (void)once; (void)arg; (void)out;
    const char* raw=getenv("SPU_DMA_PUBLICATION");
    const char* range=getenv("SPU_DMA_PUBLICATION_MATCH_RANGE");
    const char* word=getenv("SPU_DMA_PUBLICATION_MATCH_WORD");
    const char* words=getenv("SPU_DMA_PUBLICATION_WORDS");
    const char* limit=getenv("SPU_DMA_PUBLICATION_LIMIT");
    uint64_t n;
    int valid=1;
    if (words) { valid=dma_pub_number(words,10,16,&n) && n; if(valid) s_dma_publication.words=(uint32_t)n; }
    if (limit) { int ok=dma_pub_number(limit,10,32768,&n) && n; valid=valid&&ok; if(ok) s_dma_publication.limit=(uint32_t)n; }
    if(raw) { int ok=dma_pub_range(raw,&s_dma_publication.lo,&s_dma_publication.hi); valid=valid&&ok; s_dma_publication.span=ok; }
    if(range || word) {
        int ok=dma_pub_range(range,&s_dma_publication.match_lo,&s_dma_publication.match_hi) &&
            dma_pub_number(word,16,0xFFFFFFFFull,&n);
        valid=valid&&ok; s_dma_publication.match=ok; if(ok) s_dma_publication.word=(uint32_t)n;
    }
    if(!valid) s_dma_publication.span=s_dma_publication.match=0;
    InterlockedExchange(&s_dma_publication.ready,1); return TRUE;
}
static int dma_pub_claim(void) {
    LONG n=InterlockedCompareExchange(&s_dma_publication.emitted,0,0);
    while((uint32_t)n<s_dma_publication.limit) {
        LONG prior=InterlockedCompareExchange(&s_dma_publication.emitted,n+1,n);
        if(prior==n) return 1;
        n=prior;
    }
    return 0;
}
static uint32_t dma_pub_read(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void dma_pub_log(const spu_context* s,uint32_t lsa,uint64_t ea,
    uint32_t size,uint32_t cmd,const char* kind,uint64_t sample,
    uint64_t end,uint64_t match) {
    if(!dma_pub_claim()) return;
    uint32_t count=(uint32_t)((end-sample)/4);
    if(count>s_dma_publication.words) count=s_dma_publication.words;
    char payload[16*9+1]; size_t used=0;
    for(uint32_t i=0;i<count;i++) {
        uint32_t v=dma_pub_read(s->ls+lsa+(uint32_t)(sample-ea)+i*4);
        used+=(size_t)snprintf(payload+used,sizeof payload-used,"%s%08X",i?",":"",v);
    }
    fprintf(stderr,"[dma-publication] kind=%s img=%d spu=%u pc=%05X cmd=%02X lsa=%05X ea=%08X size=%u sample_ea=%08X words=%u truncated=%d match_ea=%08X payload=%s\n",
        kind,s->image_id,s->spu_id,s->pc&SPU_LS_MASK,cmd,lsa,(uint32_t)ea,size,
        (uint32_t)sample,count,sample+count*4<end,(uint32_t)match,payload);
}
/* Only outgoing bytes after core transfer validation. No VM reads, writes,
 * callbacks, locks or transfer changes. Snapshot precedes the original copy. */
void spu_dma_publication_observe(const spu_context* s,uint32_t lsa,uint64_t ea,
    uint32_t size,uint32_t cmd) {
    if(!InterlockedCompareExchange(&s_dma_publication.ready,0,0))
        InitOnceExecuteOnce(&s_dma_publication.once,dma_pub_init,NULL,NULL);
    if(!s_dma_publication.span && !s_dma_publication.match) return;
    if(!s || !s->ls || !size || size>16384 || (uint64_t)lsa+size>SPU_LS_SIZE ||
        ea>0xFFFFFFFFull || ea+size>0x100000000ull || !(cmd&0x20) || (cmd&0x40)) return;
    if((uint32_t)InterlockedCompareExchange(&s_dma_publication.emitted,0,0)>=s_dma_publication.limit) return;
    uint64_t transfer_end=ea+size;
    if(s_dma_publication.span) {
        uint64_t lo=ea>s_dma_publication.lo?ea:s_dma_publication.lo;
        uint64_t hi=transfer_end<s_dma_publication.hi?transfer_end:s_dma_publication.hi;
        lo=(lo+3)&~3ull; hi&=~3ull;
        if(lo<hi) dma_pub_log(s,lsa,ea,size,cmd,"span",lo,hi,0);
    }
    if(s_dma_publication.match) {
        uint64_t lo=ea>s_dma_publication.match_lo?ea:s_dma_publication.match_lo;
        uint64_t hi=transfer_end<s_dma_publication.match_hi?transfer_end:s_dma_publication.match_hi;
        lo=(lo+3)&~3ull; hi&=~3ull;
        /* At most the validated 16KiB transfer intersection, never arbitrary LS. */
        for(uint64_t at=lo;at<hi;at+=4) {
            if((uint32_t)InterlockedCompareExchange(&s_dma_publication.emitted,0,0)>=s_dma_publication.limit) break;
            if(dma_pub_read(s->ls+lsa+(uint32_t)(at-ea))==s_dma_publication.word) {
                uint32_t before=s_dma_publication.words/2;
                uint64_t sample=at-lo>before*4?at-before*4:lo;
                dma_pub_log(s,lsa,ea,size,cmd,"match",sample,hi,at);
            }
        }
    }
}
#endif
