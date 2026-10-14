/* Actual cellAudio implementation against inert synthetic SDL/VM services. */
#include <stdint.h>
static uint64_t test_clock=1000000;
static uint64_t test_now(void) { return test_clock; }
#define AUDIO_CHRONOLOGY_TEST_CLOCK test_now
#include "../../diagnostics/audio_chronology_impl.h"
#ifndef AUDIO_CHRONOLOGY_AUDIO_SOURCE
#define AUDIO_CHRONOLOGY_AUDIO_SOURCE "../../../libs/audio/cellAudio.c"
#endif
#include AUDIO_CHRONOLOGY_AUDIO_SOURCE
#include <assert.h>
static uint8_t test_memory[0x40000];
uint8_t* vm_base=test_memory;
uint8_t g_vm_page_bitmap[65536/8];
static uint32_t fake_queue, notify_calls;
uint32_t g_ww_lo, g_ww_hi;
int g_resv_store_active;
void SetLastError(DWORD e) { (void)e; }
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t ea,uint64_t value,int width) { (void)ea;(void)value;(void)width; }
int spu_coh_is_reserved(uint32_t ea) { (void)ea;return 0; }
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* name,int capture,const SDL_AudioSpec* desired,SDL_AudioSpec* obtained,int flags) { (void)name;(void)capture;(void)desired;(void)obtained;(void)flags;return 1; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID id,int pause) { (void)id;(void)pause; }
void SDL_CloseAudioDevice(SDL_AudioDeviceID id) { (void)id; }
int SDL_QueueAudio(SDL_AudioDeviceID id,const void* data,Uint32 length) { (void)id;(void)data;fake_queue+=length;return 0; }
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID id) { (void)id;errno=ERANGE;return fake_queue; }
const char* SDL_GetError(void) { return "synthetic-no-error"; }
void SDL_Delay(Uint32 ms) { (void)ms; }
int sys_event_queue_push_by_id(uint32_t q,uint64_t source,uint64_t a,uint64_t b,uint64_t c) { (void)q;(void)source;(void)a;(void)b;(void)c;notify_calls++;return -1; }
uint32_t sys_event_find_queue_by_key(uint64_t key) { return key==42?3:0; }
uint32_t sys_event_queue_create_direct(uint64_t key,int32_t n) { (void)key;(void)n;return 3; }
static void put_sample(unsigned n,uint32_t bits) { test_memory[0x12000+n*4]=(uint8_t)(bits>>24);test_memory[0x12001+n*4]=(uint8_t)(bits>>16);test_memory[0x12002+n*4]=(uint8_t)(bits>>8);test_memory[0x12003+n*4]=(uint8_t)bits; }
int main(int argc,char** argv) {
    (void)argv;assert(argc==1);s_audio_initialized=1;mutex_init(&s_audio_mutex);s_audio_start_us=1234567;
    AudioPortSlot* p=&s_ports[0];p->in_use=p->running=1;p->param.nChannel=2;p->param.nBlock=8;p->param.level=1;
    p->buffer=(float*)(test_memory+0x12000);p->port_addr=0x12000;p->buf_size=16384;p->read_idx_addr=0x3000;p->read_index=35;
    audio_chronology_port(0,0x12000,2,8,1);
    const char* trigger=getenv("AUDIO_CHRONOLOGY_TRIGGER");if(trigger) { FILE* f=fopen(trigger,"w");assert(f);fclose(f);test_clock+=250001; }
    // Observe the unchanged current API semantics, not a reference fix.
    assert(cellAudioGetPortBlockTag(0,1,(u64*)(uintptr_t)0x2000)==0);assert(vm_read64(0x2000)==41);
    assert(cellAudioGetPortBlockTag(0,4,(u64*)(uintptr_t)0x2000)==0);assert(vm_read64(0x2000)==36);
    assert(cellAudioGetPortTimestamp(0,35,(u64*)(uintptr_t)0x2008)==0);assert(vm_read64(0x2008)==1234567+35*256000000ull/48000ull);
    assert(cellAudioGetPortTimestamp(0,36,(u64*)(uintptr_t)0x2008)==CELL_AUDIO_ERROR_TAG_NOT_FOUND);
    assert(cellAudioGetPortBlockTag(0,8,(u64*)(uintptr_t)0x2000)==CELL_AUDIO_ERROR_PARAM);
    assert(cellAudioGetPortTimestamp(8,0,(u64*)(uintptr_t)0x2008)==CELL_AUDIO_ERROR_PARAM);
    p->read_index=0;for(unsigned n=0;n<512;n++)put_sample(n,0x3E800000); // .25 stereo
    audio_mix_one_block();assert(p->read_index==1 && vm_read64(0x3000)==1);
    for(unsigned i=0;i<512;i++) { assert(s_mix_buffer[i]==.25f);assert(!vm_read32(0x12000+i*4)); }
    s_sdl_audio_dev=1;errno=EDOM;audio_backend_submit(s_mix_buffer,256);assert(fake_queue==2048 && errno==EDOM);
    s_notify_queues[0].in_use=1;s_notify_queues[0].key=42;audio_notify_event_queues();assert(notify_calls==1);
    s_notify_queues[0].key=7;audio_notify_event_queues();assert(notify_calls==1);
    // Observable functional digest identical for parent/current and disabled/enabled.
    printf("functional read=%llu guest=%llu tag=%llu timestamp=%llu mix=%g queued=%u notify=%u\n",(unsigned long long)p->read_index,(unsigned long long)vm_read64(0x3000),(unsigned long long)vm_read64(0x2000),(unsigned long long)vm_read64(0x2008),s_mix_buffer[0],fake_queue,notify_calls);
#ifndef AUDIO_CHRONOLOGY_PARENT
    if(audio_chronology_enabled()) {
        assert(s_ac.api_count[0]==3 && s_ac.api_count[1]==3);
        unsigned stages=0;for(unsigned i=0;i<s_ac.count;i++)if(s_ac.records[i].kind>=AC_CONSUME && s_ac.records[i].kind<=AC_CURSOR)stages++;
        assert(stages==4);assert(s_ac.zero_queue==1);
        audio_chronology_finish();
    } else assert(!s_ac.count);
#endif
    mutex_destroy(&s_audio_mutex);return 0;
}
