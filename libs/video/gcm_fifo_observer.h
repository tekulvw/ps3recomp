/* Opt-in, bounded observations. Called under the existing FIFO drain lock.
 * Payload samples are diagnostic reads, not an atomic packet snapshot; header
 * is the original word consumed by the decoder. No guest state is written. */
#ifndef PS3RECOMP_GCM_FIFO_OBSERVER_H
#define PS3RECOMP_GCM_FIFO_OBSERVER_H
#include <errno.h>
#include <inttypes.h>
#include <ctype.h>

typedef struct {
    int initialized, enabled, selected, last_park;
    uint32_t lo, hi, limit, words, emitted, last_io, last_header, last_put;
    uint32_t zero_io, zero_words, zero_first_put, zero_last_put;
    uint64_t zero_end;
} gcm_fifo_observer_state;
static gcm_fifo_observer_state s_fifo_observer;

static int gcm_observer_number(const char* text, uint64_t* value, char** end)
{
    if (!text || !isxdigit((unsigned char)*text)) return 0;
    errno=0; *value=strtoull(text,end,0);
    return !errno && *end!=text;
}
static int gcm_observer_option(const char* name, uint32_t fallback,
                               uint32_t maximum, uint32_t* value)
{
    const char* text=getenv(name); uint64_t n; char* end;
    if (!text) { *value=fallback; return 1; }
    if (!gcm_observer_number(text,&n,&end) || *end || !n || n>maximum) return 0;
    *value=(uint32_t)n; return 1;
}
static void gcm_observer_init(void)
{
    gcm_fifo_observer_state* s=&s_fifo_observer;
    if (s->initialized) return;
    s->initialized=1;
    const char* range=getenv("GCM_FIFO_OBSERVE"); uint64_t lo,hi; char *end,*tail;
    if (!gcm_observer_number(range,&lo,&end) || *end!='-' ||
        !gcm_observer_number(end+1,&hi,&tail) || *tail || lo>=hi || hi>UINT32_MAX ||
        !gcm_observer_option("GCM_FIFO_OBSERVE_LIMIT",128,32768,&s->limit) ||
        !gcm_observer_option("GCM_FIFO_OBSERVE_WORDS",16,32,&s->words)) return;
    s->lo=(uint32_t)lo; s->hi=(uint32_t)hi; s->enabled=1;
}
/* Only headers actually loaded as zero by the decoder enter these runs.
 * Packet payload zeroes are not included. Flush at each drain boundary. */
static void gcm_observer_flush_zeros(void)
{
    gcm_fifo_observer_state* s=&s_fifo_observer;
    if (!s->zero_words) return;
    if (s->emitted<s->limit) {
        ++s->emitted;
        fprintf(stderr,"[fifo-observe] io=%08X header=00000000 kind=zeros end=%09" PRIX64 " zero_words=%u first_put=%08X last_put=%08X\n",
            s->zero_io,s->zero_end,s->zero_words,s->zero_first_put,s->zero_last_put);
    }
    s->zero_words=0;
}
static void gcm_observer_packet(uint32_t io, uint32_t header, uint32_t put)
{
    gcm_observer_init();
    gcm_fifo_observer_state* s=&s_fifo_observer; s->selected=0;
    if (!s->enabled || s->emitted>=s->limit) return;
    if (!header && io>=s->lo && io<s->hi) {
        s->last_park=0;
        if (s->zero_words && s->zero_end!=io) gcm_observer_flush_zeros();
        if (s->emitted>=s->limit) return;
        if (!s->zero_words) { s->zero_io=io;s->zero_first_put=put; }
        ++s->zero_words;s->zero_end=(uint64_t)io+4;s->zero_last_put=put;
        return;
    }
    gcm_observer_flush_zeros();
    if (s->emitted>=s->limit) return;
    uint32_t type=header>>29, count=(header>>18)&0x7ffu;
    const char* kind="unknown"; uint64_t end=(uint64_t)io+4;
    uint32_t target=0;
    if ((header&0xffff0000u)==0xfead0000u) kind="flip";
    else if (type==1) { target=header&0x1ffffffcu;kind=target==io?"park":"jump"; }
    else if ((header&3u)==2u) { kind="call";target=header&0x1ffffffcu; }
    else if (header==0x00020000u) kind="return";
    else if (type==0 || type==2) { kind="packet";end+=(uint64_t)count*4; }
    if (!(type==1 && target==io)) s->last_park=0;
    if (end<=s->lo || io>=s->hi) return;
    if (type==1 && target==io && s->last_park && s->last_io==io &&
        s->last_header==header && s->last_put==put) return;
    s->last_park=(type==1 && target==io);s->last_io=io;s->last_header=header;s->last_put=put;
    s->selected=1; ++s->emitted;
    uint64_t first=(uint64_t)io+4;
    if (first<s->lo) first=((uint64_t)s->lo+3)&~UINT64_C(3);
    uint64_t last=end<s->hi?end:s->hi;
    uint64_t available=last>first?(last-first)/4:0;
    uint32_t sample=(uint32_t)(available<s->words?available:s->words);
    if (strcmp(kind,"packet")) sample=0;
    fprintf(stderr,"[fifo-observe] io=%08X header=%08X put=%08X kind=%s type=%u count=%u method=%04X subch=%u end=%09" PRIX64 " target=%08X sample_io=%08" PRIX64 " sampled=%u truncated=%u payload=",
        io,header,put,kind,type,count,header&0x1ffcu,(header>>13)&7u,end,target,first,sample,
        (unsigned)(available>sample));
    for (uint32_t i=0;i<sample;i++) {
        uint64_t at=first+(uint64_t)i*4;
        uint32_t ea=at<=UINT32_MAX?gcm_io2ea((uint32_t)at):0;
        if (i) fputc(',',stderr);
        if (ea) fprintf(stderr,"%08X",vm_read32(ea)); else fputs("unmapped",stderr);
    }
    fputc('\n',stderr);
}
static void gcm_observer_event(uint32_t io, const char* event, uint32_t value)
{
    gcm_fifo_observer_state* s=&s_fifo_observer;
    if (!s->selected || s->emitted>=s->limit) return;
    ++s->emitted;
    fprintf(stderr,"[fifo-observe] io=%08X event=%s argument=%08X\n",io,event,value);
}
static void gcm_observer_branch(uint32_t io, const char* decision, uint32_t target)
{
    gcm_fifo_observer_state* s=&s_fifo_observer;
    if (!s->selected || s->emitted>=s->limit) return;
    ++s->emitted;
    fprintf(stderr,"[fifo-observe] io=%08X decision=%s target=%08X\n",io,decision,target);
}
#endif
