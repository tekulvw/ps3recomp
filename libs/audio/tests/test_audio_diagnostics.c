/* Real mixer and SDL submission boundary; no audio device or thread starts. */
#include <SDL2/SDL.h>
static int test_queue(SDL_AudioDeviceID, const void*, Uint32);
static Uint32 test_queued(SDL_AudioDeviceID);
#define SDL_QueueAudio test_queue
#define SDL_GetQueuedAudioSize test_queued
#include "../cellAudio.c"
#include <assert.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) {(void)a;(void)v;(void)w;}
int spu_coh_is_reserved(uint32_t a) {(void)a;return 0;}
void spu_coh_notify_write(uint32_t a) {(void)a;}
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
static int calls, fail;
static Uint32 queued;
static int test_queue(SDL_AudioDeviceID id,const void* pcm,Uint32 bytes) {
    assert(id==7 && pcm==s_mix_buffer && bytes==256*2*4);calls++;
    if(fail){SDL_SetError("controlled queue failure");return -1;}
    queued+=bytes;return 0;
}
static Uint32 test_queued(SDL_AudioDeviceID id) {assert(id==7);return queued;}
static void put_be_float(float* p,float value) {
    uint32_t bits;memcpy(&bits,&value,4);unsigned char* b=(void*)p;
    b[0]=bits>>24;b[1]=bits>>16;b[2]=bits>>8;b[3]=bits;
}
int main(void) {
    mutex_init(&s_audio_mutex);audio_diagnostics_init();s_sdl_audio_dev=7;
    AudioPortSlot* p=&s_ports[0];p->in_use=p->running=1;
    p->param.nChannel=8;p->param.nBlock=2;p->param.level=1;
    p->port_addr=0x58000000;p->buffer=calloc(2*256*8,sizeof(float));assert(p->buffer);
    const float samples[8]={0.1f,-0.2f,0.3f,-0.4f,0.5f,-0.6f,0.7f,-0.8f};
    for(unsigned c=0;c<8;c++)put_be_float(p->buffer+c,samples[c]);
    audio_diagnostics_report(1000000);
    audio_mix_one_block();
    assert(fabsf(s_mix_buffer[0]-(0.1f+0.3f*.707f-0.4f*.5f+0.5f*.5f+0.7f*.3f))<1e-6f);
    assert(fabsf(s_mix_buffer[1]-(-0.2f+0.3f*.707f-0.4f*.5f-0.6f*.5f-0.8f*.3f))<1e-6f);
    for(unsigned i=0;i<256*8;i++)assert(p->buffer[i]==0);
    audio_backend_submit(s_mix_buffer,256);assert(calls==1 && queued==2048);
    fail=1;audio_backend_submit(s_mix_buffer,256);assert(calls==2 && queued==2048);
    audio_diagnostics_report(1999999); /* Must not emit early. */
    audio_diagnostics_report(2000000);
    audio_mix_one_block(); /* An empty block resets the next window to silence. */
    audio_diagnostics_report(3000000);
    free(p->buffer);mutex_destroy(&s_audio_mutex);puts("PASS real mixer and submission unchanged");return 0;
}
