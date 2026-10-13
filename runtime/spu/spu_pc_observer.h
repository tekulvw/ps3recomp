/* Generic, opt-in pre-instruction buffer observer. Arbitrary LS data is not
 * necessarily audio. Configuration and counters are process-wide, shared by
 * the microstep and full interpreter; no guest state or context layout changes.
 * SPU_PC_OBSERVE names a bounded text file; see docs/spu_pc_observer.md. */
#ifndef SPU_PC_OBSERVER_H
#define SPU_PC_OBSERVER_H
#include "spu_context.h"
#include "../platform/win32_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SPU_PC_OBSERVER_FILE_MAX 4096
#define SPU_PC_OBSERVER_SITES_MAX 8
#define SPU_PC_OBSERVER_GUARDS_MAX 32
#define SPU_PC_OBSERVER_GROUPS_MAX 8
#define SPU_PC_OBSERVER_EVENTS_MAX 256
#define SPU_PC_OBSERVER_BUFFER_MAX 16384
#define SPU_PC_OBSERVER_TRIGGER_PATH_MAX 128

typedef struct spu_pc_observer_site {
    uint32_t pc, opcode, sp, sn, sd, dp, dn, dd, group;
} spu_pc_observer_site;
typedef struct spu_pc_observer_group {
    uint32_t quota, guards;
    uint32_t guard[SPU_PC_OBSERVER_GUARDS_MAX][2];
    volatile LONG events;
} spu_pc_observer_group;
typedef struct spu_pc_observer_state {
    /* -1 uninitialized, -2 initializing, 0 disabled, 1 enabled. */
    volatile LONG gate, events;
    uint32_t limit, sites, guards, identities;
    spu_pc_observer_site site[SPU_PC_OBSERVER_SITES_MAX];
    uint32_t guard[SPU_PC_OBSERVER_GUARDS_MAX][2], identity[8];
    volatile LONG trigger_armed;
    volatile LONG64 trigger_next_poll_ms;
    char trigger_path[SPU_PC_OBSERVER_TRIGGER_PATH_MAX+1];
    uint32_t groups;
    spu_pc_observer_group group[SPU_PC_OBSERVER_GROUPS_MAX];
} spu_pc_observer_state;
#ifdef __cplusplus
extern "C" {
#endif
/* One merged object across translation units, including C++ callers. */
SPU_GATE spu_pc_observer_state g_spu_pc_observer = {
    -1,0,0,0,0,0,{{0,0,0,0,0,0,0,0,0}},{{0,0}},{0},0,0,{0},0,{{0,0,{{0,0}},0}}
};
#ifdef __cplusplus
}
#endif

static inline int spu_pc_observer_number(const char* s, int base, uint32_t* out)
{
    char* end;
    const char* p=s;
    if(base==16 && p[0]=='0' && (p[1]=='x' || p[1]=='X')) p+=2;
    if(!*p) return 0;
    for(;*p;p++)
        if(!(*p>='0' && *p<='9') &&
           !(base==16 && ((*p>='a' && *p<='f') || (*p>='A' && *p<='F')))) return 0;
    errno=0;
    unsigned long long n=strtoull(s,&end,base);
    if (errno || *end || n>0xFFFFFFFFull) return 0;
    *out=(uint32_t)n; return 1;
}

/* Parse into a private fixed-size object. Nothing is published on failure.
 * A final end marker detects truncated files, including complete-line prefixes. */
static inline int spu_pc_observer_load(spu_pc_observer_state* c)
{
    const char* path=getenv("SPU_PC_OBSERVE");
    if (!path || !*path) return 0;
    FILE* f=fopen(path,"rb");
    if (!f) return 0;
    char buf[SPU_PC_OBSERVER_FILE_MAX+2];
    size_t count=fread(buf,1,SPU_PC_OBSERVER_FILE_MAX+1,f);
    int bad=ferror(f) || count>SPU_PC_OBSERVER_FILE_MAX || !feof(f);
    fclose(f);
    if (bad || !count || buf[count-1]!='\n' || memchr(buf,0,count)) return 0;
    buf[count]=0;
    int ended=0, have_limit=0, have_identity=0, have_trigger=0;
    int legacy_sites=0, grouped_sites=0;
    char* line=buf;
    while (*line) {
        char* next=strchr(line,'\n');
        *next++=0;
        if (strlen(line)>160) return 0;
        char* token[10]; unsigned nt=0;
        char* p=line;
        while (*p==' ' || *p=='\t' || *p=='\r') ++p;
        if (*p && *p!='#') {
            if (ended) return 0;
            while (*p) {
                if (nt==10) return 0;
                token[nt++]=p;
                while (*p && *p!=' ' && *p!='\t' && *p!='\r') ++p;
                if (*p) *p++=0;
                while (*p==' ' || *p=='\t' || *p=='\r') ++p;
            }
            if (!strcmp(token[0],"end")) {
                if (nt!=1) return 0;
                ended=1;
            } else if (!strcmp(token[0],"limit")) {
                if (nt!=2 || have_limit++ || !spu_pc_observer_number(token[1],10,&c->limit) ||
                    !c->limit || c->limit>SPU_PC_OBSERVER_EVENTS_MAX) return 0;
            } else if (!strcmp(token[0],"trigger")) {
                if(nt!=2 || have_trigger++ || strlen(token[1])>SPU_PC_OBSERVER_TRIGGER_PATH_MAX) return 0;
                const char* path=token[1];
                /* Absolute POSIX, drive-rooted Windows, or UNC path. No spaces
                 * or escaping in this bounded whole-token configuration. */
                int drive=((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z')) &&
                    path[1]==':' && (path[2]=='/' || path[2]=='\\');
                if(path[0]!='/' && !drive && !(path[0]=='\\' && path[1]=='\\')) return 0;
                memcpy(c->trigger_path,path,strlen(path)+1);
            } else if (!strcmp(token[0],"group")) {
                uint32_t id,quota;
                if(nt!=3 || !spu_pc_observer_number(token[1],10,&id) ||
                   !id || id>SPU_PC_OBSERVER_GROUPS_MAX ||
                   !spu_pc_observer_number(token[2],10,&quota) ||
                   !quota || quota>SPU_PC_OBSERVER_EVENTS_MAX || c->group[id-1].quota) return 0;
                c->group[id-1].quota=quota; ++c->groups;
            } else if (!strcmp(token[0],"groupguard")) {
                uint32_t id,a,w;
                if(nt!=4 || !spu_pc_observer_number(token[1],10,&id) ||
                   !id || id>SPU_PC_OBSERVER_GROUPS_MAX || !c->group[id-1].quota ||
                   !spu_pc_observer_number(token[2],16,&a) ||
                   !spu_pc_observer_number(token[3],16,&w) || (a&3) || a>SPU_LS_SIZE-4) return 0;
                spu_pc_observer_group* g=&c->group[id-1];
                if(g->guards==SPU_PC_OBSERVER_GUARDS_MAX) return 0;
                for(unsigned i=0;i<g->guards;i++) if(g->guard[i][0]==a) return 0;
                g->guard[g->guards][0]=a; g->guard[g->guards++][1]=w;
            } else if (!strcmp(token[0],"guard")) {
                uint32_t a,w;
                if (nt!=3 || c->guards==SPU_PC_OBSERVER_GUARDS_MAX ||
                    !spu_pc_observer_number(token[1],16,&a) ||
                    !spu_pc_observer_number(token[2],16,&w) ||
                    (a&3) || a>SPU_LS_SIZE-4) return 0;
                for (unsigned i=0;i<c->guards;i++) if (c->guard[i][0]==a) return 0;
                c->guard[c->guards][0]=a; c->guard[c->guards++][1]=w;
            } else if (!strcmp(token[0],"identity")) {
                if (nt<2 || nt>9 || have_identity++) return 0;
                for (unsigned i=1;i<nt;i++) {
                    uint32_t r;
                    if (!spu_pc_observer_number(token[i],10,&r) || r>127) return 0;
                    for (unsigned j=0;j<c->identities;j++) if(c->identity[j]==r) return 0;
                    c->identity[c->identities++]=r;
                }
            } else if (!strcmp(token[0],"site") || !strcmp(token[0],"groupsite")) {
                unsigned first=1; uint32_t group=0;
                if(!strcmp(token[0],"groupsite")) {
                    if(nt!=10 || !spu_pc_observer_number(token[1],10,&group) ||
                       !group || group>SPU_PC_OBSERVER_GROUPS_MAX || !c->group[group-1].quota) return 0;
                    first=2; grouped_sites=1;
                } else { if(nt!=9) return 0; legacy_sites=1; }
                if(c->sites==SPU_PC_OBSERVER_SITES_MAX || (legacy_sites && grouped_sites)) return 0;
                spu_pc_observer_site s; s.group=group;
                uint32_t v[8];
                for (unsigned i=0;i<8;i++)
                    if (!spu_pc_observer_number(token[i+first],i<2?16:10,&v[i])) return 0;
                s.pc=v[0]; s.opcode=v[1]; s.sp=v[2]; s.sn=v[3]; s.sd=v[4];
                s.dp=v[5]; s.dn=v[6]; s.dd=v[7];
                if ((s.pc&3) || s.pc>SPU_LS_SIZE-4 || s.sp>127 || s.sn>127 ||
                    s.dp>127 || s.dn>127 || s.sd<1 || s.sd>16 || s.dd<1 || s.dd>16) return 0;
                for (unsigned i=0;i<c->sites;i++) if(c->site[i].pc==s.pc && c->site[i].group==s.group) return 0;
                c->site[c->sites++]=s;
            } else return 0;
        }
        line=next;
    }
    if(!ended || !have_limit || !c->sites) return 0;
    if(!c->groups) return c->guards && legacy_sites;
    if(legacy_sites || !grouped_sites) return 0;
    uint32_t total=0;
    for(unsigned i=0;i<SPU_PC_OBSERVER_GROUPS_MAX;i++) if(c->group[i].quota) {
        int used=0;
        for(unsigned j=0;j<c->sites;j++) if(c->site[j].group==i+1) used=1;
        if(!used || !c->group[i].guards) return 0;
        total+=c->group[i].quota;
    }
    return total==c->limit;
}

static int spu_pc_observer_initialize(LONG gate)
{
    if (gate==-1 && InterlockedCompareExchange(&g_spu_pc_observer.gate,-2,-1)==-1) {
        spu_pc_observer_state config={0};
        int enabled=spu_pc_observer_load(&config);
        if (enabled) {
            /* Never assign the atomic fields while another thread reads them. */
            g_spu_pc_observer.limit=config.limit;
            g_spu_pc_observer.sites=config.sites;
            g_spu_pc_observer.guards=config.guards;
            g_spu_pc_observer.identities=config.identities;
            memcpy(g_spu_pc_observer.site,config.site,sizeof config.site);
            memcpy(g_spu_pc_observer.guard,config.guard,sizeof config.guard);
            memcpy(g_spu_pc_observer.identity,config.identity,sizeof config.identity);
            memcpy(g_spu_pc_observer.trigger_path,config.trigger_path,sizeof config.trigger_path);
            g_spu_pc_observer.groups=config.groups;
            for(unsigned i=0;i<SPU_PC_OBSERVER_GROUPS_MAX;i++) {
                g_spu_pc_observer.group[i].quota=config.group[i].quota;
                g_spu_pc_observer.group[i].guards=config.group[i].guards;
                memcpy(g_spu_pc_observer.group[i].guard,config.group[i].guard,sizeof config.group[i].guard);
            }
        }
        WriteRelease(&g_spu_pc_observer.gate,enabled);
        return enabled;
    }
    /* Contention is confined to initialization; no locks or RMW afterwards. */
    do { gate=ReadAcquire(&g_spu_pc_observer.gate); } while (gate==-2);
    return gate;
}

#if defined(_MSC_VER)
#define SPU_PC_OBSERVER_INLINE static __forceinline
#else
#define SPU_PC_OBSERVER_INLINE static inline __attribute__((always_inline))
#endif
SPU_PC_OBSERVER_INLINE int spu_pc_observer_enabled(void)
{
    LONG gate=ReadAcquire(&g_spu_pc_observer.gate);
    return gate>=0 ? gate : spu_pc_observer_initialize(gate);
}

typedef struct spu_pc_observer_stats {
    uint32_t words, finite, nonzero, nonfinite;
    float peak;
} spu_pc_observer_stats;
static inline uint32_t spu_pc_observer_word(const uint8_t* p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline spu_pc_observer_stats spu_pc_observer_scan(const uint8_t* p, uint32_t bytes)
{
    spu_pc_observer_stats s={0}; s.words=bytes/4;
    for (unsigned i=0;i<s.words;i++) {
        uint32_t magnitude=spu_pc_observer_word(p+i*4)&0x7FFFFFFFu;
        if ((magnitude&0x7F800000u)==0x7F800000u) { ++s.nonfinite; continue; }
        ++s.finite; if (magnitude) ++s.nonzero;
        float value; memcpy(&value,&magnitude,4);
        if(value>s.peak) s.peak=value;
    }
    return s;
}

/* Opt-in arming is latched for this linked runtime. File metadata lookup does
 * not allocate or read contents. The shared atomic deadline permits at most
 * one lookup per 250ms, and only after a matched guarded, in-bounds site. */
#if defined(_MSC_VER)
static __declspec(noinline) int spu_pc_observer_trigger_ready(void)
#else
static __attribute__((noinline,cold)) int spu_pc_observer_trigger_ready(void)
#endif
{
    if(ReadAcquire(&g_spu_pc_observer.trigger_armed)) return 1;
    ULONGLONG now=GetTickCount64();
    LONG64 prior=InterlockedCompareExchange64(&g_spu_pc_observer.trigger_next_poll_ms,0,0);
    if(now<(ULONGLONG)prior || now>0x7FFFFFFFFFFFFFFFull-250) return 0;
    if(InterlockedCompareExchange64(&g_spu_pc_observer.trigger_next_poll_ms,(LONG64)(now+250),prior)!=prior)
        return ReadAcquire(&g_spu_pc_observer.trigger_armed)!=0;
    DWORD saved_error=GetLastError(); int saved_errno=errno;
    DWORD attributes=GetFileAttributesA(g_spu_pc_observer.trigger_path);
    SetLastError(saved_error); errno=saved_errno;
    if(attributes==INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    InterlockedExchange(&g_spu_pc_observer.trigger_armed,1);
    return 1;
}

/* Call only immediately before executing this actual live instruction. Raw
 * register scalars use native lane 0, matching PREF and microstep addresses. */
static void spu_pc_observe_enabled(spu_context* ctx, uint32_t pc, uint32_t opcode)
{
    const spu_pc_observer_state* c=&g_spu_pc_observer;
    const spu_pc_observer_site* s=NULL;
    if((uint32_t)ReadAcquire(&g_spu_pc_observer.events)>=c->limit) return;
    for(unsigned i=0;i<c->sites;i++) {
        const spu_pc_observer_site* candidate=&c->site[i];
        if(candidate->pc!=pc || candidate->opcode!=opcode) continue;
        if(candidate->group && (uint32_t)ReadAcquire(&g_spu_pc_observer.group[candidate->group-1].events)>=
                               c->group[candidate->group-1].quota) continue;
        /* Exhausted groups return before any context/LS read. Aliased PCs in
         * other groups may still be live: qualify each candidate separately. */
        if(!ctx || !ctx->ls) return;
        if(spu_pc_observer_word(ctx->ls+pc)!=candidate->opcode) return;
        unsigned j;
        for(j=0;j<c->guards;j++)
            if(spu_pc_observer_word(ctx->ls+c->guard[j][0])!=c->guard[j][1]) break;
        if(j!=c->guards) return;
        if(candidate->group) {
            const spu_pc_observer_group* g=&c->group[candidate->group-1];
            for(j=0;j<g->guards;j++)
                if(spu_pc_observer_word(ctx->ls+g->guard[j][0])!=g->guard[j][1]) break;
            if(j!=g->guards) continue;
        }
        s=candidate; break;
    }
    if(!s) return;
    uint32_t src=spu_preferred_u32(&ctx->gpr[s->sp]), sr=spu_preferred_u32(&ctx->gpr[s->sn]);
    uint32_t dst=spu_preferred_u32(&ctx->gpr[s->dp]), dr=spu_preferred_u32(&ctx->gpr[s->dn]);
    if(sr%s->sd || dr%s->dd) return;
    uint32_t sn=sr/s->sd, dn=dr/s->dd;
    if(sn>SPU_PC_OBSERVER_BUFFER_MAX || dn>SPU_PC_OBSERVER_BUFFER_MAX ||
        src>SPU_LS_SIZE || dst>SPU_LS_SIZE || sn>SPU_LS_SIZE-src || dn>SPU_LS_SIZE-dst) return;
    if(c->trigger_path[0] && !spu_pc_observer_trigger_ready()) return;
    LONG group_prior=0;
    if(s->group) {
        spu_pc_observer_group* g=&g_spu_pc_observer.group[s->group-1];
        group_prior=ReadAcquire(&g->events);
        do {
            if((uint32_t)group_prior>=g->quota) return;
            LONG seen=InterlockedCompareExchange(&g->events,group_prior+1,group_prior);
            if(seen==group_prior) break;
            group_prior=seen;
        } while(1);
    }
    /* Group quota sum equals the global limit, so a group reservation always
     * has global capacity. Both counters are shared across contexts/threads. */
    LONG prior=ReadAcquire(&g_spu_pc_observer.events);
    do {
        if((uint32_t)prior>=c->limit) return;
        LONG seen=InterlockedCompareExchange(&g_spu_pc_observer.events,prior+1,prior);
        if(seen==prior) break;
        prior=seen;
    } while(1);
    spu_pc_observer_stats a=spu_pc_observer_scan(ctx->ls+src,sn), b=spu_pc_observer_scan(ctx->ls+dst,dn);
    /* Assemble one line before fprintf, preventing interleaved identity/stats.
     * Maximum 8 identity registers keeps the fixed output buffer bounded. */
    char line[1536];
    int n=snprintf(line,sizeof line,
        "[spu-pc-buffer] seq=%u ctx=%p img=%d pc=0x%05X opcode=0x%08X r0=0x%08X r1=0x%08X src_pr=%u src_nr=%u src_raw=%u src_div=%u src=0x%05X src_bytes=%u src_words=%u src_finite=%u src_nonzero=%u src_nonfinite=%u src_peak=%.9g dst_pr=%u dst_nr=%u dst_raw=%u dst_div=%u dst=0x%05X dst_bytes=%u dst_words=%u dst_finite=%u dst_nonzero=%u dst_nonfinite=%u dst_peak=%.9g",
        (unsigned)prior+1,(void*)ctx,ctx->image_id,pc,opcode,
        spu_preferred_u32(&ctx->gpr[0]),spu_preferred_u32(&ctx->gpr[1]),
        s->sp,s->sn,sr,s->sd,src,sn,a.words,a.finite,a.nonzero,a.nonfinite,(double)a.peak,
        s->dp,s->dn,dr,s->dd,dst,dn,b.words,b.finite,b.nonzero,b.nonfinite,(double)b.peak);
    if(n<0 || (size_t)n>=sizeof line) return;
    for(unsigned i=0;i<c->identities;i++) {
        int more=snprintf(line+n,sizeof line-(size_t)n," id_r%u=0x%08X",c->identity[i],
                          spu_preferred_u32(&ctx->gpr[c->identity[i]]));
        if(more<0 || (size_t)more>=sizeof line-(size_t)n) return;
        n+=more;
    }
    if(s->group) {
        int more=snprintf(line+n,sizeof line-(size_t)n," group=%u group_seq=%u",s->group,(unsigned)group_prior+1);
        if(more<0 || (size_t)more>=sizeof line-(size_t)n) return;
        n+=more;
    }
    fprintf(stderr,"%s\n",line);
}

SPU_PC_OBSERVER_INLINE void spu_pc_observe(spu_context* ctx, uint32_t pc, uint32_t opcode)
{
    /* Keep the disabled branch in the caller, outside the scan's stack frame. */
    if (spu_pc_observer_enabled()) spu_pc_observe_enabled(ctx,pc,opcode);
}
/* Runtime loops cache the immutable gate. Drop the local flag at exhaustion,
 * so a long-running interpreter invocation also stops observer work. */
SPU_PC_OBSERVER_INLINE void spu_pc_observe_cached(spu_context* ctx, uint32_t pc,
                                                 uint32_t opcode, int* observe)
{
    if (!*observe) return;
    if ((uint32_t)ReadAcquire(&g_spu_pc_observer.events)>=g_spu_pc_observer.limit) {
        *observe=0; return;
    }
    spu_pc_observe_enabled(ctx,pc,opcode);
}
#undef SPU_PC_OBSERVER_INLINE
#endif
