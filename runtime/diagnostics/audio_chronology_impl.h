/* One definition alongside the existing accepted-copy observer. C and C++. */
#ifndef PS3RECOMP_AUDIO_CHRONOLOGY_IMPL_H
#define PS3RECOMP_AUDIO_CHRONOLOGY_IMPL_H
#include "audio_chronology.h"
#include "../ppu/ppu_context.h"
#include "../platform/win32_compat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define AC_PORTS 8
#define AC_MAX 32768
#define AC_UNKNOWN (~(uint64_t)0)
enum { AC_PORT=1, AC_CONSUME=2, AC_CLEAR_BEGIN=3, AC_CLEAR_END=4,
       AC_CURSOR=5, AC_DMA=6, AC_BACKEND=7, AC_NOTIFY=8, AC_API=9 };
typedef struct ac_port {
    uint64_t epoch, serial[32];
    uint32_t ea, block_bytes, blocks, channels, phase, writers[32];
    int active;
} ac_port;
typedef struct ac_record {
    uint64_t us, begin_us, copy_us, epoch, index, ctx, arg0, arg1, value;
    uint32_t kind, port, ea, size, lsa, cmd, spu, pc, mask, nz, words, flags;
    int rc;
} ac_record;
static struct {
    INIT_ONCE once;
    volatile LONG ready, lock, window_active, finished;
    volatile LONG64 next_poll;
    int enabled, armed, flushed;
    uint32_t limit, count, dropped, api_count[2], min_queue, zero_queue;
    uint64_t duration_us, start_us, previous_submit, max_interval;
    char trigger[129];
    ac_port ports[AC_PORTS];
    ac_record records[AC_MAX];
} s_ac = { INIT_ONCE_STATIC_INIT };
typedef struct ac_call_record {
    uint64_t us, run, token, ctx, thread, args[9];
    uint32_t target, depth, phase, flags;
} ac_call_record;
static struct {
    uint32_t target, count, dropped, pending;
    uint64_t serial;
    ac_call_record records[AC_MAX];
} s_ac_calls;
/* Scalar TLS cannot dangle after a non-C++ longjmp. Such pending spans are
 * incomplete: consumers must reject notes without a matched normal span end. */
#if defined(__cplusplus)
#define AC_TLS thread_local
#elif defined(_MSC_VER)
#define AC_TLS __declspec(thread)
#else
#define AC_TLS _Thread_local
#endif
static AC_TLS uint64_t ac_note_scope[4]; /* run, span, ctx, guest thread */
typedef struct ac_note_record {
    uint64_t us, run, span, token, ctx, thread, arg0, arg1, arg2, arg3;
    uint32_t kind, phase;
    int32_t rc;
} ac_note_record;
static struct {
    int enabled, scalar_enabled;
    uint32_t count, dropped, pending;
    uint64_t serial;
    ac_note_record records[AC_MAX];
} s_ac_notes;
static void ac_notes_dump_locked(void);
static void ac_calls_dump_locked(void);
/* Preserve host error state for every public hook, including disabled paths. */
#ifdef _WIN32
#define AC_SAVE int ac_errno=errno; DWORD ac_last=GetLastError()
#define AC_RESTORE do { errno=ac_errno; SetLastError(ac_last); } while(0)
#else
#define AC_SAVE int ac_errno=errno
#define AC_RESTORE do { errno=ac_errno; } while(0)
#endif
static uint64_t ac_now(void) {
#ifdef AUDIO_CHRONOLOGY_TEST_CLOCK
    return AUDIO_CHRONOLOGY_TEST_CLOCK();
#elif defined(_WIN32)
    LARGE_INTEGER freq, count;
    if(!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&count) || freq.QuadPart<=0)return 0;
    return (uint64_t)(count.QuadPart/freq.QuadPart)*1000000ull + (uint64_t)(count.QuadPart%freq.QuadPart)*1000000ull/(uint64_t)freq.QuadPart;
#else
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000ull+(uint64_t)t.tv_nsec/1000ull;
#endif
}
static void ac_lock(void) { while(InterlockedCompareExchange(&s_ac.lock,1,0)) { /* metadata only */ } }
static void ac_unlock(void) { InterlockedExchange(&s_ac.lock,0); }
static int ac_number(const char* s,uint32_t max,uint32_t* out) {
    char* end; if(!s || !*s || *s<'0' || *s>'9')return 0;
    errno=0; unsigned long n=strtoul(s,&end,10);
    if(errno || *end || n>max || !n)return 0; *out=(uint32_t)n;return 1;
}
static BOOL WINAPI ac_init(PINIT_ONCE once,PVOID arg,PVOID* out) {
    (void)once;(void)arg;(void)out;
    const char* enable=getenv("AUDIO_CHRONOLOGY");
    const char* trigger=getenv("AUDIO_CHRONOLOGY_TRIGGER");
    const char* limit=getenv("AUDIO_CHRONOLOGY_LIMIT");
    const char* duration=getenv("AUDIO_CHRONOLOGY_MS");
    uint32_t ms=10000; s_ac.limit=32768; s_ac.min_queue=~0u;
    int valid=enable && !strcmp(enable,"1") && trigger && *trigger && strlen(trigger)<=128;
    if(limit)valid=ac_number(limit,AC_MAX,&s_ac.limit)&&valid;
    if(duration)valid=ac_number(duration,30000,&ms)&&valid;
    if(valid)memcpy(s_ac.trigger,trigger,strlen(trigger)+1);
    s_ac.duration_us=(uint64_t)ms*1000;
    s_ac.enabled=valid;
    const char* selected=getenv("AUDIO_CHRONOLOGY_PPU_TARGET");
    if(valid && selected && *selected>='0' && *selected<='9') {
        char* end;errno=0;unsigned long long target=strtoull(selected,&end,0);
        if(!errno && !*end && target && target<=0xFFFFFFFCull && !(target&3))s_ac_calls.target=(uint32_t)target;
    }
    /* The paired call-span option is capped at ten seconds on the SAME window. */
    if(s_ac_calls.target && s_ac.duration_us>10000000ull)s_ac.duration_us=10000000ull;
    const char* notes=getenv("AUDIO_CHRONOLOGY_PPU_WAIT_NOTES");
    s_ac_notes.enabled=s_ac_calls.target && notes && !strcmp(notes,"1");
    const char* scalar=getenv("AUDIO_CHRONOLOGY_PPU_SCALAR_NOTES");
    s_ac_notes.scalar_enabled=s_ac_notes.enabled && scalar && !strcmp(scalar,"1");
    InterlockedExchange(&s_ac.ready,1); return TRUE;
}
static int ac_enabled(void) {
    if(!InterlockedCompareExchange(&s_ac.ready,0,0))InitOnceExecuteOnce(&s_ac.once,ac_init,NULL,NULL);
    return s_ac.enabled;
}
/* All registry/record accesses are serialized. This lock NEVER protects audio
 * PCM, transfer execution, notification queues, or native scheduling state. */
static void ac_record_locked(ac_record* r) {
    if(s_ac.count<s_ac.limit)s_ac.records[s_ac.count++]=*r;
    else if(s_ac.dropped!=~0u)s_ac.dropped++;
}
static void ac_dump_locked(const char* reason) {
    if(s_ac.flushed || !s_ac.armed)return;
    InterlockedExchange(&s_ac.window_active,0);
    s_ac.flushed=1;
    InterlockedExchange(&s_ac.finished,1);
    uint64_t inflight=0;for(unsigned p=0;p<AC_PORTS;p++)for(unsigned b=0;b<32;b++)inflight+=s_ac.ports[p].writers[b];
    fprintf(stderr,"[audio-chronology-summary] reason=%s count=%u dropped=%u limit=%u start_us=%llu duration_us=%llu queue_min=%u queue_zero=%u max_submit_interval_us=%llu inflight_at_flush=%llu coverage=SPU-DMA-only overlap_is_ambiguous=1\n",
        reason,s_ac.count,s_ac.dropped,s_ac.limit,(unsigned long long)s_ac.start_us,(unsigned long long)s_ac.duration_us,s_ac.min_queue,s_ac.zero_queue,(unsigned long long)s_ac.max_interval,(unsigned long long)inflight);
    ac_calls_dump_locked();
    for(uint32_t i=0;i<s_ac.count;i++) {
        const ac_record* r=&s_ac.records[i];
        fprintf(stderr,"[audio-chronology] seq=%u kind=%u us=%llu begin_us=%llu copy_us=%llu port=%u epoch=%llu index=%llu ea=%08X size=%u lsa=%05X cmd=%X ctx=%llX spu=%u pc=%05X mask=%08X nz=%u words=%u flags=%u rc=%d arg0=%llu arg1=%llu value=%llu\n",
            i+1,r->kind,(unsigned long long)r->us,(unsigned long long)r->begin_us,(unsigned long long)r->copy_us,r->port,(unsigned long long)r->epoch,(unsigned long long)r->index,r->ea,r->size,r->lsa,r->cmd,(unsigned long long)r->ctx,r->spu,r->pc,r->mask,r->nz,r->words,r->flags,r->rc,(unsigned long long)r->arg0,(unsigned long long)r->arg1,(unsigned long long)r->value);
    }
}
/* Only audio_chronology_poll admits the trigger or prints a deadline flush.
 * Hot hooks use atomic admission; no clock/lock/PCM work outside the window. */
static int ac_active(void) {
    return ac_enabled() && InterlockedCompareExchange(&s_ac.window_active,0,0)!=0;
}
static int ac_window_locked(uint64_t now) {
    if(!now || !InterlockedCompareExchange(&s_ac.window_active,0,0))return 0;
    if(now<s_ac.start_us || now-s_ac.start_us>=s_ac.duration_us) {
        InterlockedExchange(&s_ac.window_active,0);return 0;
    }
    return 1;
}
static void ac_poll_locked(uint64_t now) {
    if(!now || s_ac.flushed)return;
    if(s_ac.armed) {
        if(now>=s_ac.start_us && now-s_ac.start_us>=s_ac.duration_us)ac_dump_locked("window");
        return;
    }
    LONG64 next=InterlockedCompareExchange64(&s_ac.next_poll,0,0);
    if(now<(uint64_t)next)return;
    InterlockedExchange64(&s_ac.next_poll,(LONG64)(now+250000));
#ifdef _WIN32
    struct _stat64 st; int found=!_stat64(s_ac.trigger,&st) && (st.st_mode&_S_IFMT)==_S_IFREG;
#else
    struct stat st; int found=!stat(s_ac.trigger,&st) && S_ISREG(st.st_mode);
#endif
    if(!found)return;
    s_ac.armed=1;s_ac.start_us=now;
    for(unsigned i=0;i<AC_PORTS;i++)if(s_ac.ports[i].active) {
        ac_port* p=&s_ac.ports[i]; ac_record r={0};
        r.kind=AC_PORT;r.us=now;r.port=i;r.epoch=p->epoch;r.ea=p->ea;r.size=p->block_bytes;r.cmd=p->blocks;r.lsa=p->channels;r.flags=1;
        ac_record_locked(&r);
    }
    InterlockedExchange(&s_ac.window_active,1);
}
static uint32_t ac_nonzero(const uint8_t* data,uint32_t bytes) {
    uint32_t nz=0;
    for(uint32_t i=0;i+4<=bytes;i+=4) { uint32_t w=((uint32_t)data[i]<<24)|((uint32_t)data[i+1]<<16)|((uint32_t)data[i+2]<<8)|data[i+3];nz+=(w&0x7FFFFFFFu)!=0; }
    return nz;
}
int audio_chronology_enabled(void) { AC_SAVE;int yes=ac_active();AC_RESTORE;return yes; }
void audio_chronology_port(unsigned port,uint32_t ea,uint32_t channels,uint32_t blocks,int active) {
    AC_SAVE;
    if(ac_enabled() && port<AC_PORTS) {
        ac_lock();ac_port* p=&s_ac.ports[port];uint64_t epoch=p->epoch+1;memset(p,0,sizeof *p);p->epoch=epoch;
        if(active && (channels==2 || channels==8) && (blocks==8 || blocks==16 || blocks==32) && ea && (uint64_t)ea+(uint64_t)channels*256*4*blocks<=0x100000000ull) {
            p->ea=ea;p->block_bytes=channels*256*4;p->blocks=blocks;p->channels=channels;p->active=1;
        }
        uint64_t now=ac_active()?ac_now():0;if(ac_window_locked(now)) { ac_record r={0};r.kind=AC_PORT;r.us=now;r.port=port;r.epoch=epoch;r.ea=p->ea;r.size=p->block_bytes;r.cmd=p->blocks;r.lsa=p->channels;r.flags=p->active;ac_record_locked(&r); }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_consume(unsigned port,unsigned stage,uint64_t index,const uint8_t* data,uint32_t bytes,uint64_t guest_index) {
    AC_SAVE;
    if(ac_active() && port<AC_PORTS && stage<4) {
        ac_lock();uint64_t now=ac_now();ac_port* p=&s_ac.ports[port];
        if(ac_window_locked(now) && p->active && bytes==p->block_bytes) {
            uint32_t slot=(uint32_t)(index%p->blocks),mask=1u<<slot;
            if(stage==0) { p->serial[slot]++;p->phase|=mask; }
            ac_record r={0};r.kind=AC_CONSUME+stage;r.us=now;r.port=port;r.epoch=p->epoch;r.index=index;r.ea=p->ea+slot*p->block_bytes;r.size=bytes;r.mask=mask;r.arg0=guest_index;
            r.flags=p->writers[slot]?1:0;if(stage==0 && data) { r.nz=ac_nonzero(data,bytes);r.words=bytes/4; }
            ac_record_locked(&r);if(stage==3)p->phase&=~mask;
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_dma_begin(audio_chronology_dma_token* t,uint64_t ea,uint32_t size,uint32_t cmd) {
    AC_SAVE;t->port=-1;
    if(ac_active() && (cmd&0x20) && !(cmd&0x40) && size && ea<=0xFFFFFFFFull && size<=0x100000000ull-ea) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now))for(unsigned i=0;i<AC_PORTS;i++) {
            ac_port* p=&s_ac.ports[i];uint64_t end=(uint64_t)p->ea+p->block_bytes*p->blocks;
            if(!p->active || ea>=end || ea+size<=p->ea)continue;
            memset(t,0,sizeof *t);t->port=(int)i;t->epoch=p->epoch;t->begin_us=now;t->ea=ea;t->size=size;
            uint64_t lo=ea>p->ea?ea:p->ea,hi=ea+size<end?ea+size:end;
            t->offset=(uint32_t)(lo-ea);t->bytes=(uint32_t)(hi-lo);
            unsigned first=(unsigned)((lo-p->ea)/p->block_bytes),last=(unsigned)((hi-1-p->ea)/p->block_bytes);
            for(unsigned slot=first;slot<=last;slot++) { t->slot_mask|=1u<<slot;t->serial+=p->serial[slot];p->writers[slot]++; }
            t->overlap=(p->phase&t->slot_mask)!=0;break;
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_dma_after(audio_chronology_dma_token* t,const uint8_t* destination) {
    AC_SAVE;
    if(t->port>=0 && ac_active()) {
        uint64_t now=ac_now();
        if(now<s_ac.start_us || now-s_ac.start_us>=s_ac.duration_us) {
            InterlockedExchange(&s_ac.window_active,0);AC_RESTORE;return;
        }
        t->copy_us=now;
        uint32_t align=(uint32_t)(-(t->ea+t->offset))&3u;
        t->words=t->bytes>=align?(t->bytes-align)/4:0;
        if(t->words)t->nonzero=ac_nonzero(destination+t->offset+align,t->words*4);
    }
    AC_RESTORE;
}
void audio_chronology_dma_end(const audio_chronology_dma_token* t,const void* ctx,uint32_t spu,uint32_t pc,uint32_t lsa,uint32_t cmd,int rc,unsigned copied) {
    AC_SAVE;
    if(t->port>=0 && t->port<AC_PORTS && ac_active()) {
        ac_lock();uint64_t now=ac_now();
        if(!ac_window_locked(now)) { ac_unlock();AC_RESTORE;return; }
        ac_port* p=&s_ac.ports[t->port];uint64_t serial=0;unsigned mismatch=p->epoch!=t->epoch;
        if(!mismatch)for(unsigned slot=0;slot<p->blocks;slot++)if(t->slot_mask&(1u<<slot)) { serial+=p->serial[slot];if(p->writers[slot])p->writers[slot]--; }
        unsigned ambiguous=t->overlap || mismatch || serial!=t->serial || (p->phase&t->slot_mask)!=0;
        {
            ac_record r={0};r.kind=AC_DMA;r.us=now;r.begin_us=t->begin_us;r.copy_us=t->copy_us;r.port=(uint32_t)t->port;r.epoch=t->epoch;r.index=serial;r.ea=(uint32_t)t->ea;r.size=t->size;r.lsa=lsa;r.cmd=cmd;r.ctx=(uint64_t)(uintptr_t)ctx;r.spu=spu;r.pc=pc;r.mask=t->slot_mask;r.nz=t->nonzero;r.words=t->words;r.flags=(ambiguous?1u:0u)|(mismatch?2u:0u)|(copied?4u:0u);r.rc=rc;r.arg0=t->offset;r.arg1=t->bytes;r.value=p->epoch;ac_record_locked(&r);
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_backend(uint32_t before,uint32_t after,int rc,uint32_t frames) {
    AC_SAVE;if(ac_active()) { ac_lock();uint64_t now=ac_now();if(ac_window_locked(now)) {
        ac_record r={0};r.kind=AC_BACKEND;r.us=now;r.port=~0u;r.arg0=before;r.arg1=after;r.size=frames;r.rc=rc;
        r.value=s_ac.previous_submit?now-s_ac.previous_submit:0;s_ac.previous_submit=now;if(r.value>s_ac.max_interval)s_ac.max_interval=r.value;
        if(before<s_ac.min_queue)s_ac.min_queue=before;if(!before)s_ac.zero_queue++;ac_record_locked(&r);
    }ac_unlock(); }AC_RESTORE;
}
void audio_chronology_notify(uint32_t queue,uint64_t key,int rc) {
    AC_SAVE;if(ac_active()) { ac_lock();uint64_t now=ac_now();if(ac_window_locked(now)) { ac_record r={0};r.kind=AC_NOTIFY;r.us=now;r.port=~0u;r.arg0=queue;r.arg1=key;r.rc=rc;ac_record_locked(&r); }ac_unlock(); }AC_RESTORE;
}
void audio_chronology_api(unsigned kind,unsigned port,uint64_t requested,uint64_t counter,uint64_t result,int rc) {
    AC_SAVE;if(ac_active() && kind<2) { ac_lock();uint64_t now=ac_now();if(ac_window_locked(now) && s_ac.api_count[kind]<16) {
        s_ac.api_count[kind]++;ac_record r={0};r.kind=AC_API;r.us=now;r.port=port;r.cmd=kind;r.arg0=requested;r.arg1=counter;r.value=result;r.rc=rc;
        if(port<AC_PORTS) { ac_port* p=&s_ac.ports[port];r.epoch=p->epoch;r.ea=p->ea;r.size=p->block_bytes;if(p->blocks && counter!=AC_UNKNOWN)r.index=counter%p->blocks; }
        ac_record_locked(&r);
    }ac_unlock(); }AC_RESTORE;
}
/* A separate prefix retains the existing audio record schema unchanged. Raw
 * register arguments never authorize guest-memory or PCM dereferences. */
static void ac_calls_dump_locked(void) {
    if(!s_ac_calls.target)return;
    fprintf(stderr,"[ppu-call-span-summary] run_id=%llu target=%08X start_us=%llu duration_us=%llu count=%u dropped=%u limit=%u pending_recorded_brackets=%u completion=PPU-fn-and-trampoline-only\n",
        (unsigned long long)s_ac.start_us,s_ac_calls.target,(unsigned long long)s_ac.start_us,(unsigned long long)s_ac.duration_us,s_ac_calls.count,s_ac_calls.dropped,s_ac.limit,s_ac_calls.pending);
    for(uint32_t i=0;i<s_ac_calls.count;i++) {
        const ac_call_record* r=&s_ac_calls.records[i];
        fprintf(stderr,"[ppu-call-span] seq=%u run_id=%llu token=%llu phase=%u us=%llu ctx=%llX thread=%llu target=%08X depth=%u flags=%u r3=%llu r4=%llu r5=%llu r6=%llu r7=%llu r8=%llu r9=%llu r10=%llu r31=%llu\n",
            i+1,(unsigned long long)r->run,(unsigned long long)r->token,r->phase,(unsigned long long)r->us,(unsigned long long)r->ctx,(unsigned long long)r->thread,r->target,r->depth,r->flags,
            (unsigned long long)r->args[0],(unsigned long long)r->args[1],(unsigned long long)r->args[2],(unsigned long long)r->args[3],(unsigned long long)r->args[4],(unsigned long long)r->args[5],(unsigned long long)r->args[6],(unsigned long long)r->args[7],(unsigned long long)r->args[8]);
    }
    ac_notes_dump_locked();
}
static void ac_call_drop(void) { if(s_ac_calls.dropped!=~0u)s_ac_calls.dropped++; }
void audio_chronology_call_begin(audio_chronology_call_token* t,const void* context,uint32_t target,uint32_t depth) {
    AC_SAVE;t->serial=0;
    if(ac_active() && s_ac_calls.target && target==s_ac_calls.target) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now)) {
            if(s_ac_calls.count>=s_ac.limit || s_ac_calls.serial==~(uint64_t)0)ac_call_drop();
            else {
                const ppu_context* ctx=(const ppu_context*)context;
                memset(t,0,sizeof *t);t->serial=++s_ac_calls.serial;t->run=s_ac.start_us;t->ctx=(uint64_t)(uintptr_t)context;t->thread=ctx->thread_id;t->target=target;t->depth=depth;
                for(unsigned i=0;i<8;i++)t->args[i]=ctx->gpr[i+3];t->args[8]=ctx->gpr[31];
                ac_call_record r={0};r.us=now;r.run=t->run;r.token=t->serial;r.ctx=t->ctx;r.thread=t->thread;r.target=target;r.depth=depth;memcpy(r.args,t->args,sizeof r.args);
                s_ac_calls.records[s_ac_calls.count++]=r;s_ac_calls.pending++;
                if(s_ac_notes.enabled) {
                    memcpy(t->previous_note_scope,ac_note_scope,sizeof ac_note_scope);
                    ac_note_scope[0]=t->run;ac_note_scope[1]=t->serial;ac_note_scope[2]=t->ctx;ac_note_scope[3]=t->thread;
                }
            }
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_call_end(audio_chronology_call_token* t,unsigned abnormal) {
    AC_SAVE;uint64_t token=t->serial;t->serial=0;
    if(token && s_ac_notes.enabled)memcpy(ac_note_scope,t->previous_note_scope,sizeof ac_note_scope);
    if(token && ac_active()) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now) && t->run==s_ac.start_us) {
            if(s_ac_calls.count>=s_ac.limit)ac_call_drop();
            else {
                ac_call_record r={0};r.us=now;r.run=t->run;r.token=token;r.ctx=t->ctx;r.thread=t->thread;r.target=t->target;r.depth=t->depth;r.phase=1;r.flags=abnormal?1u:0u;memcpy(r.args,t->args,sizeof r.args);
                s_ac_calls.records[s_ac_calls.count++]=r;if(s_ac_calls.pending)s_ac_calls.pending--;
            }
        }
        ac_unlock();
    }
    AC_RESTORE;
}
/* Admission touches no context or guest memory. The first expired hook can
 * close admission, but cannot flush; subsequent hooks do no clocks or locks. */
static int ac_note_admitted(void) {
    return ac_active() && s_ac_notes.enabled && ac_note_scope[1] && ac_note_scope[0]==s_ac.start_us;
}
static int ac_note_room(void) {
    if(s_ac_notes.count<s_ac.limit)return 1;
    if(s_ac_notes.dropped!=~0u)s_ac_notes.dropped++;return 0;
}
static void ac_note_identity(ac_note_record* r,uint64_t now) {
    r->us=now;r->run=ac_note_scope[0];r->span=ac_note_scope[1];r->ctx=ac_note_scope[2];r->thread=ac_note_scope[3];
}
void audio_chronology_wait_begin(audio_chronology_wait_token* t,uint64_t requested) {
    AC_SAVE;t->serial=0;
    if(ac_note_admitted()) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now) && ac_note_room()) {
            ac_note_record r={0};ac_note_identity(&r,now);r.kind=1;r.arg0=requested;r.token=++s_ac_notes.serial;
            t->serial=r.token;t->run=r.run;t->span=r.span;t->ctx=r.ctx;t->thread=r.thread;t->requested=requested;
            s_ac_notes.records[s_ac_notes.count++]=r;s_ac_notes.pending++;
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_wait_end(const audio_chronology_wait_token* t,int host_rc,int host_errno) {
    AC_SAVE;
    if(t->serial && ac_active() && s_ac_notes.enabled) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now) && t->run==s_ac.start_us && ac_note_room()) {
            ac_note_record r={0};r.us=now;r.run=t->run;r.span=t->span;r.token=t->serial;r.ctx=t->ctx;r.thread=t->thread;
            r.kind=1;r.phase=1;r.arg0=t->requested;r.rc=host_rc;r.arg1=(uint32_t)host_errno;
            s_ac_notes.records[s_ac_notes.count++]=r;if(s_ac_notes.pending)s_ac_notes.pending--;
        }
        ac_unlock();
    }
    AC_RESTORE;
}
void audio_chronology_workload(uint64_t spurs,uint32_t wid,uint64_t info,int32_t rc,uint32_t contention,uint32_t ready,uint32_t idle) {
    AC_SAVE;
    if(ac_note_admitted()) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now) && ac_note_room()) {
            ac_note_record r={0};ac_note_identity(&r,now);r.kind=2;r.phase=2;r.rc=rc;r.arg0=spurs;r.arg1=wid;r.arg2=info;
            r.arg3=rc?AC_UNKNOWN:(uint64_t)contention|((uint64_t)ready<<8)|((uint64_t)idle<<16);
            s_ac_notes.records[s_ac_notes.count++]=r;
        }
        ac_unlock();
    }
    AC_RESTORE;
}
/* No guest/context access: callers pass already-observed scalar values. */
uint64_t audio_chronology_scalar_scope(void) {
    AC_SAVE;uint64_t span=0;
    if(ac_note_admitted() && s_ac_notes.scalar_enabled)span=ac_note_scope[1];
    AC_RESTORE;return span;
}
uint64_t audio_chronology_scalar(uint64_t token,uint32_t tag,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3) {
    AC_SAVE;uint64_t recorded=0;
    if(ac_note_admitted() && s_ac_notes.scalar_enabled) {
        ac_lock();uint64_t now=ac_now();
        if(ac_window_locked(now) && ac_note_room()) {
            ac_note_record r={0};ac_note_identity(&r,now);
            r.kind=3;r.phase=tag;r.token=token?token:++s_ac_notes.serial;
            r.arg0=a0;r.arg1=a1;r.arg2=a2;r.arg3=a3;
            s_ac_notes.records[s_ac_notes.count++]=r;recorded=r.token;
        }
        ac_unlock();
    }
    AC_RESTORE;return recorded;
}
static void ac_notes_dump_locked(void) {
    if(!s_ac_notes.enabled)return;
    fprintf(stderr,"[ppu-call-note-summary] run_id=%llu start_us=%llu duration_us=%llu count=%u dropped=%u limit=%u pending_recorded_sleeps=%u coverage=%s\n",
        (unsigned long long)s_ac.start_us,(unsigned long long)s_ac.start_us,(unsigned long long)s_ac.duration_us,s_ac_notes.count,s_ac_notes.dropped,s_ac.limit,s_ac_notes.pending,
        s_ac_notes.scalar_enabled?"existing-sleep-workload-and-scalar-only":"existing-sleep-and-workload-info-only");
    for(uint32_t i=0;i<s_ac_notes.count;i++) {
        const ac_note_record* r=&s_ac_notes.records[i];
        fprintf(stderr,"[ppu-call-note] seq=%u run_id=%llu span=%llu token=%llu us=%llu ctx=%llX thread=%llu kind=%u phase=%u rc=%d arg0=%llu arg1=%llu arg2=%llu arg3=%llu\n",
            i+1,(unsigned long long)r->run,(unsigned long long)r->span,(unsigned long long)r->token,(unsigned long long)r->us,(unsigned long long)r->ctx,(unsigned long long)r->thread,r->kind,r->phase,r->rc,
            (unsigned long long)r->arg0,(unsigned long long)r->arg1,(unsigned long long)r->arg2,(unsigned long long)r->arg3);
    }
}
void audio_chronology_poll(void) {
    AC_SAVE;
    if(ac_enabled() && !InterlockedCompareExchange(&s_ac.finished,0,0)) {
        uint64_t now=ac_now();
        LONG64 next=InterlockedCompareExchange64(&s_ac.next_poll,0,0);
        if(InterlockedCompareExchange(&s_ac.window_active,0,0) || now>=(uint64_t)next) {
            ac_lock();ac_poll_locked(now);ac_unlock();
        }
    }
    AC_RESTORE;
}
void audio_chronology_finish(void) { AC_SAVE;if(ac_enabled()) { ac_lock();ac_dump_locked("shutdown");ac_unlock(); }AC_RESTORE; }
#endif
