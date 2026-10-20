#ifndef PAD_FILE_ANALOG_H
#define PAD_FILE_ANALOG_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
typedef struct { uint64_t sequence,deadline_ms; unsigned reports; uint8_t axes[4]; } PadAnalogCommand;
/* A1 NONCE SEQUENCE REPORTS ABSOLUTE_MONOTONIC_DEADLINE_MS LX LY RX RY.
 * Decimal only; tagged lines never fall through to the legacy digital parser.
 * REPORTS=0 is an explicit neutral release, valid only with four neutral axes. */
static int pad_analog_parse(const char*line,const char*nonce,uint64_t now,PadAnalogCommand*out){
    if(!line || strncmp(line,"A1",2) || (line[2]&&!isspace((unsigned char)line[2])))return 0;
    if(!nonce || !*nonce || strlen(nonce)>64 || !out)return -1;
    const char*p=line+2;while(isspace((unsigned char)*p))++p;
    const char*begin=p;while(*p&&!isspace((unsigned char)*p))++p;
    if((size_t)(p-begin)!=strlen(nonce)||memcmp(begin,nonce,(size_t)(p-begin)))return -1;
    uint64_t v[7];
    for(unsigned i=0;i<7;i++){
        while(isspace((unsigned char)*p))++p;
        if(*p<'0'||*p>'9')return -1;
        uint64_t n=0;
        do{unsigned d=(unsigned)(*p++-'0');if(n>(UINT64_MAX-d)/10)return -1;n=n*10+d;}while(*p>='0'&&*p<='9');
        if(*p&&!isspace((unsigned char)*p))return -1;
        v[i]=n;
    }
    while(isspace((unsigned char)*p))++p;
    if(*p||!v[0]||v[1]>120||v[2]<=now||v[2]-now>10000)return -1;
    for(unsigned i=3;i<7;i++)if(v[i]>255||(!v[1]&&v[i]!=128))return -1;
    PadAnalogCommand c={v[0],v[2],(unsigned)v[1],{0,0,0,0}};
    for(unsigned i=0;i<4;i++)c.axes[i]=(uint8_t)v[i+3];
    *out=c;return 1;
}
typedef struct {
    PadAnalogCommand command;uint64_t last_sequence;unsigned delivered;
    int phase; /* 0 idle,1 hold,2 neutral release awaiting real report */
    int expired;
} PadAnalogState;
static int pad_analog_admit(PadAnalogState*s,const PadAnalogCommand*c){
    if(!s||!c||c->sequence<=s->last_sequence)return 0;
    /* A queued new hold cannot silently supersede an active hold. */
    if(s->phase&&c->reports)return 0;
    s->command=*c;s->last_sequence=c->sequence;s->delivered=0;
    s->phase=c->reports?1:2;s->expired=0;return 1;
}
static int pad_analog_axes(PadAnalogState*s,uint64_t now,uint8_t out[4]){
    if(!s->phase)return 0;
    if(s->phase==1&&now>=s->command.deadline_ms){s->phase=2;s->expired=1;}
    for(unsigned i=0;i<4;i++)out[i]=s->phase==1?s->command.axes[i]:128;
    return 1;
}
/* Call only AFTER the exact guest packet is emitted. A zero-len drain call
 * never consumes a hold or certifies neutral release. Return1 requests ACK. */
static int pad_analog_emitted(PadAnalogState*s,int positive_len){
    if(!positive_len||!s->phase)return 0;
    if(s->phase==2){s->phase=0;return 1;}
    if(++s->delivered==s->command.reports)s->phase=2;
    return 0;
}
#endif
