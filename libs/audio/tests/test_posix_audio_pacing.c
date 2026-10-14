/* Real POSIX mixer, inert SDL/VM, deterministic clock. No device opens. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
static int test_clock_gettime(clockid_t, struct timespec*);
static int test_usleep(useconds_t);
#define clock_gettime test_clock_gettime
#define usleep test_usleep
#include "../../../runtime/diagnostics/audio_chronology_impl.h"
#include "../cellAudio.c"
#undef clock_gettime
#undef usleep
#include <assert.h>

static uint8_t memory[0x20000];
uint8_t* vm_base=memory;
uint8_t g_vm_page_bitmap[65536/8];
uint32_t g_ww_lo,g_ww_hi;
int g_resv_store_active;
void SetLastError(DWORD e){(void)e;}
void ppu_resv_break_store(uint64_t a){(void)a;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;}
void spu_lockline_lock(void){}
void spu_lockline_unlock(void){}

static uint64_t now_ns=1999000000ull, start_ns, end_ns, last_ns, drain_fraction;
static uint64_t last_notify, resume_ns, resumed_first, stopped_cursor, submit_count;
static unsigned frames, peak_queue, notifications, max_batch, batch, sleeps, queue_queries;
static int mode, jumped, stopped, restarted, resumed;
static void fill_slot(unsigned slot) {
    for(unsigned i=0;i<512;i++) {
        unsigned char* p=(unsigned char*)s_ports[0].buffer+(slot*512+i)*4;
        p[0]=0x3e;p[1]=0x80;p[2]=p[3]=0; /* .25f, big endian */
    }
}
static int test_clock_gettime(clockid_t id,struct timespec* ts) {
    assert(id==CLOCK_MONOTONIC);
    /* Give each iteration a real cost, while sleeps determine the cadence. */
    now_ns+=10000;
    uint64_t elapsed=now_ns-start_ns;
    int freeze=mode==2 || (mode==3 && elapsed<200000000ull);
    if(mode==5 && elapsed>=100000000ull && !stopped) {
        stopped=1;assert(cellAudioPortStop(0)==0);stopped_cursor=s_ports[0].read_index;
        assert(cellAudioGetPortConfig(0,(CellAudioPortConfig*)(uintptr_t)0x1800)==0);
        assert(vm_read32(0x1804)==CELL_AUDIO_STATUS_READY);
    }
    if(mode==5 && elapsed>=200000000ull && !restarted) {
        assert(s_ports[0].read_index==stopped_cursor);
        restarted=1;assert(cellAudioPortStart(0)==0);
        assert(cellAudioGetPortConfig(0,(CellAudioPortConfig*)(uintptr_t)0x1800)==0);
        assert(vm_read32(0x1804)==CELL_AUDIO_STATUS_RUN);
    }
    if(mode==8) s_sdl_audio_dev=(elapsed>=200000000ull && elapsed<400000000ull)?0:1;
    if(!freeze) {
        uint64_t amount=(now_ns-last_ns)*CELL_AUDIO_SAMPLE_RATE+drain_fraction;
        unsigned consumed=(unsigned)(amount/1000000000ull);
        drain_fraction=amount%1000000000ull;
        frames=consumed>frames?0:frames-consumed;
    }
    last_ns=now_ns;
    if(now_ns>=end_ns)s_mix_thread_running=0;
    ts->tv_sec=(time_t)(now_ns/1000000000ull);ts->tv_nsec=(long)(now_ns%1000000000ull);
    return 0;
}
static int test_usleep(useconds_t us) {
    sleeps++;now_ns+=(uint64_t)us*1000;
    if(mode==4 && !jumped && now_ns-start_ns>=200000000ull) {
        now_ns+=2000000000ull;jumped=1;resume_ns=now_ns;
    }
    struct timespec discarded;test_clock_gettime(CLOCK_MONOTONIC,&discarded);
    return 0;
}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* a,int b,const SDL_AudioSpec* c,SDL_AudioSpec* d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;assert(!"no device opens");return 0;}
void SDL_PauseAudioDevice(SDL_AudioDeviceID d,int p){(void)d;(void)p;}
void SDL_CloseAudioDevice(SDL_AudioDeviceID d){(void)d;}
int SDL_QueueAudio(SDL_AudioDeviceID d,const void* pcm,Uint32 bytes) {
    assert(d==1 && pcm==s_mix_buffer && bytes==256*2*sizeof(float));
    assert(s_mix_buffer[0]==(s_ports[0].running?.25f:0.0f));
    submit_count++;
    if(mode!=6)frames+=256;
    if(frames>peak_queue)peak_queue=frames;
    return mode==6?-1:0;
}
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID d){assert(d==1);queue_queries++;return frames*2*sizeof(float);}
const char* SDL_GetError(void){return "synthetic rejection";}
void SDL_Delay(Uint32 ms){(void)ms;}
uint32_t sys_event_find_queue_by_key(uint64_t key){assert(key==42);return 7;}
uint32_t sys_event_queue_create_direct(uint64_t k,int32_t n){(void)k;(void)n;return 7;}
int sys_event_queue_push_by_id(uint32_t q,uint64_t k,uint64_t a,uint64_t b,uint64_t c) {
    assert(q==7 && k==42 && !a && !b && !c);
    if(s_ports[0].running) {
        unsigned consumed=(unsigned)((s_ports[0].read_index-1)%8);
        for(unsigned i=0;i<512;i++)assert(s_ports[0].buffer[consumed*512+i]==0.0f);
        assert(s_mix_buffer[0]==.25f);fill_slot(consumed);
    } else assert(s_mix_buffer[0]==0.0f);
    notifications++;
    if(now_ns-last_notify<1000000ull)batch++;else batch=1;
    if(batch>max_batch)max_batch=batch;
    if(jumped) {
        if(!resumed){resumed=1;resumed_first=now_ns;}
        /* A suspension must not turn elapsed wall time into burst consumption. */
        if(now_ns-resumed_first<5000000ull)assert(batch==1);
    }
    last_notify=now_ns;
    assert(vm_read64(0x1000)==s_ports[0].read_index%8);
    return 0;
}
int main(int argc,char** argv) {
    assert(argc==2);setbuf(stdout,NULL);mode=atoi(argv[1]);start_ns=last_ns=now_ns;
    end_ns=start_ns+(mode==4?3000000000ull:(mode==7?10000000000ull:1000000000ull));
    s_audio_initialized=1;mutex_init(&s_audio_mutex);audio_diagnostics_init();
    AudioPortSlot* p=&s_ports[0];p->in_use=p->running=1;p->param.nChannel=2;p->param.nBlock=8;p->param.level=1;
    p->buffer=(float*)(memory+0x2000);p->read_idx_addr=0x1000;p->port_addr=0x2000;p->buf_size=16384;
    assert(cellAudioGetPortConfig(0,(CellAudioPortConfig*)(uintptr_t)0x1800)==0);
    assert(vm_read32(0x1800)==0x1000 && vm_read64(0x1808)==2 && vm_read64(0x1810)==8);
    assert(vm_read32(0x1818)==16384 && vm_read32(0x181c)==0x2000);
    for(unsigned i=0;i<8;i++)fill_slot(i);
    s_notify_queues[0].in_use=1;s_notify_queues[0].key=42;
    s_sdl_audio_dev=mode==1?0:1;
    if(mode==2 || mode==3)frames=1024;
    s_mix_thread_running=1;audio_mix_thread_func(NULL);
    printf("mode=%d elapsed_ns=%llu blocks=%u cursor=%llu submit=%llu peak_queue=%u max_batch=%u sleeps=%u queries=%u\n",mode,(unsigned long long)(now_ns-start_ns),notifications,(unsigned long long)p->read_index,(unsigned long long)submit_count,peak_queue,max_batch,sleeps,queue_queries);
    assert(sleeps>0 && sleeps<(mode==7?11000:1100));
    if(mode==2){assert(notifications==0 && p->read_index==0 && ld_be_f32(p->buffer)==.25f);}
    else if(mode==3){assert(notifications>=148 && notifications<=154);}
    else if(mode==4){assert(jumped && resumed && notifications>=180 && notifications<=195);}
    else if(mode==7){assert(notifications>=1874 && notifications<=1880);}
    else {assert(notifications>=186 && notifications<=193);}
    if(mode==1){assert(!submit_count && max_batch==1);}
    if(mode==8)assert(submit_count+30<notifications);
    if(mode==5){assert(stopped && restarted && p->read_index+17<notifications && p->read_index+21>notifications);}
    else assert(p->read_index==notifications);
    if(mode!=6)assert(peak_queue<=1280);
    assert(max_batch<=4);
    mutex_destroy(&s_audio_mutex);puts("PASS actual POSIX mixer timing/queue/control fixture");return 0;
}
