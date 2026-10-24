/* Included once by spu_coherency.c; no extra build-system source entry.
 * ARM/STOP/DUMP are serialized on the existing controller only. Workers never
 * expose TLS pointers. Each owns one process-lived report slot and seals it at
 * most once per generation. Release sealed -> acquire controller establishes
 * payload visibility; no writer changes a sealed payload until a new ARM,
 * which cannot occur until the previous dump has finished. A command retains
 * its TLS generation across STOP/new ARM. Missing/unacknowledged/idle workers
 * remain unknown; no cleanup range is assumed to mean host-thread termination.
 */
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define OBS_THREADS 32u
#define OBS_ROWS 128u
#define OBS_PROBES 4u
#define OBS_ACTIVE 1u
#define OBS_STOP 2u
#define OBS_BUDGET_MAX 100000000ull
/* All observation timestamps use the SAME clock as default hybrid deadlines. */
static uint64_t obs_now(void) { return spu_lockline_now_ns(); }
static uint64_t obs_clock_start,obs_clock_stop,obs_id,obs_budget,obs_serial,obs_last_id;
static unsigned obs_busy,obs_registered;
static int obs_fd=-1;
typedef struct {
    uint32_t image,pc,spu,cmd,ea,outcome;
    uint64_t n,samples,transaction_ns;
} obs_row;
typedef struct {
    uint64_t tid,generation,commands,started,late,budget_exhausted,overflow;
    uint64_t first_ns,last_ns;
    obs_row rows[OBS_ROWS];
} obs_payload;
typedef struct { uint64_t sealed; obs_payload data; } obs_report;
static obs_report obs_reports[OBS_THREADS];
static __thread struct {
    unsigned registered,slot,recording,sealed;
    uint32_t random,countdown;
    uint64_t generation,budget;
    obs_payload data;
    obs_row current;
    uint64_t transaction_start;
} obs_tls;
static void obs_seal(void)
{
    if(!obs_tls.generation||obs_tls.sealed||obs_tls.recording)return;
    if(!obs_tls.data.last_ns)obs_tls.data.last_ns=obs_now();
    obs_tls.sealed=1;
    if(obs_tls.slot>=OBS_THREADS)return;
    obs_reports[obs_tls.slot].data=obs_tls.data;
    __atomic_store_n(&obs_reports[obs_tls.slot].sealed,obs_tls.generation,__ATOMIC_RELEASE);
}
int spu_atomic_observe_begin(spu_context* c,uint32_t cmd,uint32_t ea,uint64_t state)
{
    uint64_t gen=state>>2;
    if(obs_tls.recording)return 0; /* No nested observer state or guest changes. */
    if(obs_tls.generation && (obs_tls.generation!=gen||(state&3u)!=OBS_ACTIVE))obs_seal();
    if((state&3u)!=OBS_ACTIVE)return 0;
    if(obs_tls.generation!=gen){
        uint64_t budget=__atomic_load_n(&obs_budget,__ATOMIC_RELAXED);
        if(__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE)!=state)return 0;
        if(!obs_tls.registered){
            obs_tls.slot=__atomic_fetch_add(&obs_registered,1u,__ATOMIC_RELAXED);
            obs_tls.registered=1;
        }
        memset(&obs_tls.data,0,sizeof(obs_tls.data));
        obs_tls.generation=gen;obs_tls.budget=budget;obs_tls.sealed=0;
        obs_tls.data.generation=gen;
#ifdef __APPLE__
        pthread_threadid_np(NULL,&obs_tls.data.tid);
#else
        obs_tls.data.tid=(uint64_t)(uintptr_t)pthread_self();
#endif
        obs_tls.random=(uint32_t)(gen+obs_tls.slot+1u);obs_tls.countdown=256;
        obs_tls.data.first_ns=obs_now();
    }
    if(obs_tls.sealed||obs_tls.data.started>=obs_tls.budget)return 0;
    if(__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE)!=state){obs_seal();return 0;}
    memset(&obs_tls.current,0,sizeof(obs_tls.current));
    obs_tls.current.image=c->image_id;obs_tls.current.pc=c->pc;
    obs_tls.current.spu=c->spu_id;obs_tls.current.cmd=cmd;obs_tls.current.ea=ea;
    obs_tls.data.started++;obs_tls.recording=1;
    /* Observable TLS state is established before entering any legacy lock.
     * Only the whole handler interval is sampled; no observer hook runs inside
     * a lock/notifier, including cold PPU threads when ARM changes state. */
    if(!--obs_tls.countdown){
        obs_tls.random=obs_tls.random*1664525u+1013904223u;
        obs_tls.countdown=128u+(obs_tls.random>>24);
        obs_tls.current.samples=1;obs_tls.transaction_start=obs_now();
    }
    return 1;
}
void spu_atomic_observe_end(uint32_t outcome)
{
    if(!obs_tls.recording)return;
    obs_row* r=&obs_tls.current;
    if(r->samples)r->transaction_ns=obs_now()-obs_tls.transaction_start;
    r->outcome=outcome;
    unsigned h=(r->ea>>7)^r->pc^(r->image*0x9e3779b9u)^r->cmd^(r->outcome*17u)^r->spu;
    unsigned k;
    for(k=0;k<OBS_PROBES;k++){
        obs_row* p=&obs_tls.data.rows[(h+k)&(OBS_ROWS-1u)];
        if(!p->n){*p=*r;p->n=1;break;}
        if(p->image==r->image&&p->pc==r->pc&&p->spu==r->spu&&p->cmd==r->cmd&&p->ea==r->ea&&p->outcome==r->outcome){
            p->n++;p->samples+=r->samples;p->transaction_ns+=r->transaction_ns;break;
        }
    }
    if(k==OBS_PROBES)obs_tls.data.overflow++;
    obs_tls.data.commands++;
    uint64_t state=__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE);
    if(state!=((obs_tls.generation<<2)|OBS_ACTIVE))obs_tls.data.late++;
    obs_tls.recording=0;
    if(obs_tls.data.started>=obs_tls.budget)obs_tls.data.budget_exhausted=1;
    if(state!=((obs_tls.generation<<2)|OBS_ACTIVE)||obs_tls.data.budget_exhausted){
        obs_tls.data.last_ns=obs_now();obs_seal();
    }
}
int ps3_spu_atomic_observe_arm(uint64_t id,uint64_t budget,const char* path)
{
    const char* on=getenv("SPU_ATOMIC_OBSERVE");const char* prof=getenv("PS3_TPROF");
    if(!on||strcmp(on,"1")||(prof&&*prof)||getenv("SPU_LOCKLINE_PROFILE")||getenv("SPU_LOCKLINE_MODE")||getenv("SPU_LOCKLINE_HIST")||!id||!budget||budget>OBS_BUDGET_MAX||!path||path[0]!='/')return -1;
    size_t n=strlen(path);if(!n||n>1023)return -1;
    for(size_t i=0;i<n;i++)if((unsigned char)path[i]<=32u)return -1;
    unsigned z=0;if(!__atomic_compare_exchange_n(&obs_busy,&z,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return -1;
    if(id<=obs_last_id){__atomic_store_n(&obs_busy,0u,__ATOMIC_RELEASE);return -1;}
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if(fd<0){__atomic_store_n(&obs_busy,0u,__ATOMIC_RELEASE);return -1;}
    obs_fd=fd;obs_id=id;obs_last_id=id;obs_clock_start=obs_now();obs_clock_stop=0;
    __atomic_store_n(&obs_budget,budget,__ATOMIC_RELAXED);obs_serial++;
    __atomic_store_n(&g_spu_atomic_observe_state,(obs_serial<<2)|OBS_ACTIVE,__ATOMIC_RELEASE);
    return 0;
}
int ps3_spu_atomic_observe_stop(uint64_t id)
{
    if(!__atomic_load_n(&obs_busy,__ATOMIC_ACQUIRE)||id!=obs_id)return -1;
    uint64_t want=(obs_serial<<2)|OBS_ACTIVE;
    uint64_t when=obs_now();
    if(!__atomic_compare_exchange_n(&g_spu_atomic_observe_state,&want,(obs_serial<<2)|OBS_STOP,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return -1;
    obs_clock_stop=when;return 0;
}
int ps3_spu_atomic_observe_dump(uint64_t id)
{
    if(!__atomic_load_n(&obs_busy,__ATOMIC_ACQUIRE)||id!=obs_id||__atomic_load_n(&g_spu_atomic_observe_state,__ATOMIC_ACQUIRE)!=((obs_serial<<2)|OBS_STOP))return -1;
    /* Grace gives active workers a boundary; NEVER reads mutable TLS or forces ACK. */
    uint64_t deadline=obs_now()+100000000ull;
    unsigned n=__atomic_load_n(&obs_registered,__ATOMIC_ACQUIRE),limit=n<OBS_THREADS?n:OBS_THREADS;
    for(;;){unsigned ready=0;for(unsigned i=0;i<limit;i++)ready+=__atomic_load_n(&obs_reports[i].sealed,__ATOMIC_ACQUIRE)==obs_serial;
        if(ready==limit||obs_now()>=deadline)break;
        struct timespec ts={0,1000000};nanosleep(&ts,NULL);
    }
    FILE* f=fdopen(obs_fd,"w");int rc=-1;
    if(f){
        uint64_t total=0,late=0;unsigned ready=0;uint32_t published=0;
        /* Freeze one acquire-validated report set: late ACK cannot change totals
         * between header and rows. Payloads in this set are immutable. */
        for(unsigned i=0;i<limit;i++)if(__atomic_load_n(&obs_reports[i].sealed,__ATOMIC_ACQUIRE)==obs_serial){published|=1u<<i;total+=obs_reports[i].data.commands;late+=obs_reports[i].data.late;ready++;}
        fprintf(f,"{\"schema\":1,\"clock\":\"hybrid_uptime_raw_ns\",\"id\":%llu,\"generation\":%llu,\"start_ns\":%llu,\"stop_ns\":%llu,\"commands\":%llu,\"completed_after_stop\":%llu,\"registered_lifetime_threads\":%u,\"reported_threads\":%u,\"missing_unknown\":%u,\"thread_capacity_overflow\":%u,\"unseen_idle_threads\":\"unknown\",\"scope\":\"published threads only; external all-worker qualification required\",\"key_capacity\":128,\"max_key_probes\":4,\"reports\":[",(unsigned long long)id,(unsigned long long)obs_serial,(unsigned long long)obs_clock_start,(unsigned long long)obs_clock_stop,(unsigned long long)total,(unsigned long long)late,n,ready,limit-ready,n>OBS_THREADS?n-OBS_THREADS:0);
        unsigned out=0;
        for(unsigned i=0;i<limit;i++){
            if(!(published&(1u<<i)))continue;
            const obs_payload* p=&obs_reports[i].data;
            fprintf(f,"%s{\"slot\":%u,\"thread\":%llu,\"started\":%llu,\"commands\":%llu,\"completed_after_stop\":%llu,\"budget_exhausted\":%s,\"key_overflow\":%llu,\"first_ns\":%llu,\"last_ns\":%llu,\"rows\":[",out++?",":"",i,(unsigned long long)p->tid,(unsigned long long)p->started,(unsigned long long)p->commands,(unsigned long long)p->late,p->budget_exhausted?"true":"false",(unsigned long long)p->overflow,(unsigned long long)p->first_ns,(unsigned long long)p->last_ns);
            unsigned row=0;
            for(unsigned j=0;j<OBS_ROWS;j++){const obs_row* r=p->rows+j;if(!r->n)continue;
                fprintf(f,"%s{\"ctx_image_id_snapshot\":%u,\"ctx_pc_snapshot\":%u,\"ctx_spu_id_snapshot\":%u,\"current_executing_image\":\"unknown\",\"cmd\":%u,\"ea\":%u,\"outcome\":%u,\"n\":%llu,\"timed_transactions\":%llu,\"sample_transaction_ns\":%llu}",row++?",":"",r->image,r->pc,r->spu,r->cmd,r->ea,r->outcome,(unsigned long long)r->n,(unsigned long long)r->samples,(unsigned long long)r->transaction_ns);
            }fputs("]}",f);
        }fputs("]}\n",f);int bad=ferror(f);rc=fclose(f);if(bad)rc=-1;
    }else close(obs_fd);
    obs_fd=-1;__atomic_store_n(&g_spu_atomic_observe_state,0ull,__ATOMIC_RELEASE);
    __atomic_store_n(&obs_busy,0u,__ATOMIC_RELEASE);return rc;
}
int ps3_spu_atomic_observe_control_request(const char* line)
{
    if(strncmp(line,"spu-atomic",10)|| (line[10]&&line[10]!=' '&&line[10]!='\t'))return 0;
    unsigned long long id=0,ms=0,budget=0;char path[1024],extra,number[3][32];
    if(sscanf(line,"spu-atomic %31s %31s %31s %1023s %c",number[0],number[1],number[2],path,&extra)!=4)return 1;
    unsigned long long* values[3]={&id,&ms,&budget};
    for(unsigned i=0;i<3;i++){
        for(const char* p=number[i];*p;p++)if(*p<'0'||*p>'9')return 1;
        errno=0;*values[i]=strtoull(number[i],NULL,10);if(errno==ERANGE)return 1;
    }
    if(!ms||ms>20000||!id||!budget||budget>OBS_BUDGET_MAX)return 1;
    if(ps3_spu_atomic_observe_arm(id,budget,path))return 1;
    fprintf(stderr,"[spu-atomic-observe] armed=%llu generation=%llu start_ns=%llu duration_ms=%llu budget=%llu\n",id,(unsigned long long)obs_serial,(unsigned long long)obs_clock_start,ms,budget);
    const uint64_t deadline=obs_clock_start+ms*1000000ull;
    while(obs_now()<deadline){struct timespec ts={0,1000000};nanosleep(&ts,NULL);}
    int rc=ps3_spu_atomic_observe_stop(id);if(!rc)rc=ps3_spu_atomic_observe_dump(id);
    fprintf(stderr,"[spu-atomic-observe] capture=%llu result=%d output=%s\n",id,rc,path);
    return 1;
}
#else
int spu_atomic_observe_begin(spu_context* c,uint32_t cmd,uint32_t ea,uint64_t s){(void)c;(void)cmd;(void)ea;(void)s;return 0;}
void spu_atomic_observe_end(uint32_t x){(void)x;}
int ps3_spu_atomic_observe_arm(uint64_t i,uint64_t n,const char* p){(void)i;(void)n;(void)p;return -1;}
int ps3_spu_atomic_observe_stop(uint64_t i){(void)i;return -1;}
int ps3_spu_atomic_observe_dump(uint64_t i){(void)i;return -1;}
int ps3_spu_atomic_observe_control_request(const char* p){(void)p;return 0;}
#endif
