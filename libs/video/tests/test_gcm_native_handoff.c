/* A missing pre-header barrier would execute retained state before publication;
 * replay, stale cookies or cross-VM delivery would submit the wrong source. */
#define main checked_fixture_main
#include "test_gcm_checked_reference.c"
#undef main
#include "../rsx_native_handoff.h"
static unsigned callbacks;
static uint64_t last_cookie;
int rsx_draw_engine_native_source_target(rsx_native_source_target* out) {
    memset(out,0,sizeof(*out));out->ready=1;out->color_a_offset=0x4000;
    out->color_a_dma=0xfeed0000;out->clip_width=1280;out->clip_height=720;
    out->native_surface=23;return 1;
}
static void consume(void* user,const rsx_native_handoff_event* e) {
    assert(user==&callbacks);assert(e->abi_version==1&&e->struct_size==sizeof(*e));
    assert(e->source_observation==91&&e->header_ea==0x100000&&e->context_ea==0x200000);
    assert(e->vm_identity==17&&e->vm_base_identity==(uint64_t)(uintptr_t)vm_base&&e->header==0x0004183c&&e->argument==0);
    assert(e->fifo_get==0&&e->fifo_put==8&&s_fifo_getoff==0);
    assert(e->target.color_a_offset==0x4000&&e->target.native_surface==23);
    assert(cellGcm_native_handoff_cancel(e->cookie)==-4); /* callback runs outside registry lock */
    last_cookie=e->cookie;callbacks++;
}
static unsigned ring_callbacks;
static void consume_ring(void* user,const rsx_native_handoff_ring_event* e) {
    assert(user==&ring_callbacks&&e->abi_version==1&&e->struct_size==sizeof(*e));
    assert(e->source.context_ea==0); /* no synthetic PPE GCM context */
    assert(e->source.vm_identity==17&&e->source.source_observation==92);
    assert(e->source.header_ea==0x110000&&e->source.fifo_get==0x10000&&s_fifo_getoff==0x10000);
    assert(e->writer_state_ea==0x210000&&e->ring_begin_ea==0x110000&&e->ring_bytes==0x4000);
    assert(e->source.header==0x00041d6c&&e->source.argument==0x480);
    assert(cellGcm_native_handoff_cancel(e->source.cookie)==-4);
    ring_callbacks++;
}
static void wrapped_ring_writer_checks(void) {
    // Same source shape as Run32: a wrapped current at the ring head, while
    // begin/end still describe the last 8192-byte block (8172-byte payload).
    const u32 begin=0x110000,bytes=0xA3000,tail=begin+bytes-0x2000;
    rsx_native_handoff_ring_arm a={1,sizeof(a),17,93,0x210000,begin,bytes,begin,
        0x00041d6c,0x480,&ring_callbacks,consume_ring};
    raw32(a.writer_state_ea,tail);raw32(a.writer_state_ea+4,tail+0x1FEC);
    raw32(a.writer_state_ea+8,begin);raw32(a.writer_state_ea+12,0x01B17A70);
    raw32(begin,a.expected_header);raw32(begin+4,a.expected_argument);
    uint64_t cookie=0;
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==0&&cookie);
    raw32(a.writer_state_ea+12,0); // Wrap never waives publication identity.
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3);
    raw32(a.writer_state_ea+12,0x01B17A70);
    assert(cellGcm_native_handoff_ring_publish(cookie)==0);
    assert(cellGcm_native_handoff_cancel(cookie)==0);
    raw32(a.writer_state_ea+4,tail+0x1FF0);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-3&&!cookie);
    raw32(a.writer_state_ea+4,tail+0x1FEC);
    raw32(a.writer_state_ea,tail-128);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-3&&!cookie);
    raw32(a.writer_state_ea,tail);
    a.header_ea=begin+128;raw32(a.writer_state_ea+8,a.header_ea);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-3&&!cookie);
}
static void ring_writer_checks(void) {
    raw32(0x210000,0x110000);raw32(0x210004,0x112000);
    raw32(0x210008,0x110000);raw32(0x21000c,0xffffffff);
    raw32(0x110000,0);raw32(0x110004,0);
    _Static_assert(sizeof(rsx_native_handoff_ring_arm)==64,"ring arm ABI");
    rsx_native_handoff_ring_arm a={1,sizeof(a),17,92,0x210000,0x110000,0x4000,0x110000,0x00041d6c,0x480,&ring_callbacks,consume_ring};
    uint64_t cookie=0;
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==0&&cookie);
    uint64_t duplicate=1;assert(cellGcm_native_handoff_ring_arm(&a,&duplicate)==-4&&duplicate==0);
    cellGcm_syscall_set_fifo(0x10008,0x10000);cellGcm_rsx_process_fifo();
    assert(s_fifo_getoff==0x10000&&ring_callbacks==0);
    assert(cellGcm_native_handoff_publish(cookie)==-3); /* wrong writer mode */
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3); /* words unwritten */
    raw32(0x110004,0x480);raw32(0x110000,0x00041d6c);
    raw32(0x210008,0x110008); /* old UI rule must not qualify a ring writer */
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3);
    raw32(0x210008,0x110000);raw32(0x21000c,0);
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3); /* actual writer state changed; no allocation-generation inference */
    raw32(0x21000c,0xffffffff);
    assert(cellGcm_native_handoff_ring_publish(cookie)==0);
    /* Real final writer state can advance after publication, before GET. */
    raw32(0x210008,0x110080);
    cellGcm_rsx_process_fifo();assert(ring_callbacks==1&&s_fifo_getoff==0x10008);
    assert(cellGcm_native_handoff_cancel(cookie)==-3);
    cellGcm_syscall_set_fifo(0x10008,0x10000);cellGcm_rsx_process_fifo();assert(ring_callbacks==1);
    raw32(0x210008,0x110000);a.header_ea=0x114000;
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-1&&cookie==0); /* outside source ring */
    a.header_ea=0x110000;a.ring_bytes=7;
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-1&&cookie==0);
    a.ring_bytes=0x4000;raw32(0x210004,0x110004);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==-3&&cookie==0);
    raw32(0x210004,0x112000);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==0);
    uint8_t* real_vm=vm_base;vm_base+=4;
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3);vm_base=real_vm;
    assert(cellGcm_native_handoff_cancel(cookie)==0);
    assert(cellGcm_native_handoff_ring_arm(&a,&cookie)==0);
    assert(cellGcm_native_handoff_ring_publish(cookie)==0);
    raw32(0x110004,0x490);cellGcm_syscall_set_fifo(0x10008,0x10000);cellGcm_rsx_process_fifo();
    assert(ring_callbacks==1&&cellGcm_native_handoff_cancel(cookie)==-3); /* changed before GET */
}

/* Ring watch: a writer we do not control writes the header; it is judged when
 * GET arrives, every outcome is reported exactly once, and GET never waits. */
static unsigned watch_calls;
static int32_t watch_status;
static u32 watch_get_at_callback;
static rsx_native_handoff_ring_event watch_event;
static void consume_watch(void* user,const rsx_native_handoff_ring_event* e,int32_t status) {
    assert(user==&watch_calls&&e->abi_version==1&&e->struct_size==sizeof(*e));
    assert(e->source.vm_identity==17&&e->source.source_observation==94);
    assert(e->writer_state_ea==0x220000&&e->ring_begin_ea==0x120000&&e->ring_bytes==0x4000);
    assert(cellGcm_native_handoff_cancel(e->source.cookie)==-4); /* outside the lock, held */
    watch_event=*e;watch_status=status;watch_get_at_callback=s_fifo_getoff;watch_calls++;
}
static rsx_native_handoff_ring_watch watch_at(u32 header_ea) {
    rsx_native_handoff_ring_watch w={1,sizeof(w),17,94,0x220000,0x120000,0x4000,header_ea,
        0x00041d6c,0x480,{0,0},{0,0},0,0,&watch_calls,consume_watch};
    return w;
}
static void clear_ring(void) { for(u32 a=0x11f000;a<0x124000;a+=4)raw32(a,0); }
static void ring_watch_checks(void) {
    _Static_assert(sizeof(rsx_native_handoff_ring_watch)==88,"ring watch ABI");
    uint64_t cookie=7;
    rsx_native_handoff_ring_watch w=watch_at(0x120000);
    /* Invalid input. */
    w.abi_version=2;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1&&cookie==0);w.abi_version=1;
    w.header_ea=0x124000;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1);
    w.header_ea=0x123ff8;w.tail_mask[0]=1;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1);
    w.tail_mask[0]=0;w.reserved=1;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1);
    w=watch_at(0x120000);w.consume=NULL;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1);
    w=watch_at(0x120000);w.expected_header=0x20000000u;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-1);

    /* ARRIVED: nothing read at arm; the words appear later; callback runs
     * before the header is decoded and GET then continues. */
    clear_ring();w=watch_at(0x120000);
    assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0&&cookie);
    uint64_t dup=1;assert(cellGcm_native_handoff_ring_watch(&w,&dup)==-4&&dup==0);
    assert(cellGcm_native_handoff_ring_publish(cookie)==-3); /* not a publishing writer */
    raw32(0x120000,0x00041d6c);raw32(0x120004,0x480);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20008,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_ARRIVED);
    assert(watch_get_at_callback==0x20000&&s_fifo_getoff==0x20008);
    assert(watch_event.source.header==0x00041d6c&&watch_event.source.argument==0x480);
    assert(watch_event.source.header_ea==0x120000&&watch_event.source.target.native_surface==23);
    assert(watch_event.source.cookie==cookie&&cellGcm_native_handoff_cancel(cookie)==-3);
    cellGcm_syscall_set_fifo(0x20008,0x20000);cellGcm_rsx_process_fifo();assert(watch_calls==1); /* one-shot */

    /* MISMATCH: GET arrives before the writer; reported, and GET never waits. */
    clear_ring();assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20008,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_MISMATCH&&s_fifo_getoff==0x20008);
    assert(watch_event.source.header==0&&watch_event.source.target.native_surface==0);

    /* Masked tail words: the writer's epoch bits must match too. */
    clear_ring();raw32(0x120000,0x00041d6c);raw32(0x120004,0x480);
    raw32(0x120008,0x00041d74);raw32(0x12000c,0x80000ec0u);
    w.expected_tail[0]=0x00041d74;w.tail_mask[0]=0xffffffffu;
    w.expected_tail[1]=0x40000000u;w.tail_mask[1]=0xc0000000u;
    assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20008,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_MISMATCH);
    raw32(0x12000c,0x40000ec0u);assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20008,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_ARRIVED);

    /* SKIPPED: a forward JUMP over the header. */
    clear_ring();w=watch_at(0x120010);
    assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    raw32(0x120000,0x20000000u|0x20020u);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20024,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_SKIPPED&&s_fifo_getoff==0x20024);
    /* SKIPPED: a JUMP back out of the span before reaching the header. */
    clear_ring();assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    raw32(0x120004,0x20000000u|0x1f000u);
    watch_calls=0;cellGcm_syscall_set_fifo(0x1f004,0x20004);cellGcm_rsx_process_fifo();
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_SKIPPED);
    /* A JUMP that stays short of the header is not a skip. */
    clear_ring();w=watch_at(0x120020);
    assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    raw32(0x120000,0x20000000u|0x20010u);
    watch_calls=0;cellGcm_syscall_set_fifo(0x20014,0x20000);cellGcm_rsx_process_fifo();
    assert(watch_calls==0&&s_fifo_getoff==0x20014);
    /* Cancel before arrival: no callback can begin. */
    assert(cellGcm_native_handoff_cancel(cookie)==0);
    raw32(0x120020,0x00041d6c);raw32(0x120024,0x480);
    cellGcm_syscall_set_fifo(0x20028,0x20020);cellGcm_rsx_process_fifo();assert(watch_calls==0);

    /* SKIPPED by a consumer resynchronisation. */
    clear_ring();assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    {u32 get=0x20000;watch_calls=0;gcm_fifo_resync_why("watch-test",&get,0x20030);}
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_SKIPPED);

    /* EXPIRED after max_flips FIFO flips. */
    clear_ring();w.max_flips=2;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    watch_calls=0;gcm_native_watch_sweep(2,0,0,0,0);assert(watch_calls==0);
    gcm_native_watch_sweep(2,0,0,0,0);
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_EXPIRED);
    w.max_flips=0;assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    for(int i=0;i<8;i++)gcm_native_watch_sweep(2,0,0,0,0);
    assert(watch_calls==1); /* no expiry requested */
    assert(cellGcm_native_handoff_cancel(cookie)==0);

    /* VM_RETIRED: the base changed before arrival. */
    assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==0);
    uint8_t* real_vm=vm_base;vm_base+=4;watch_calls=0;
    gcm_native_watch_sweep(2,0,0,0,0);vm_base=real_vm;
    assert(watch_calls==1&&watch_status==RSX_NATIVE_WATCH_VM_RETIRED);

    /* Capacity is shared with the other handoff kinds. */
    uint64_t cookies[64];
    for(unsigned i=0;i<64;i++){w=watch_at(0x120000+8*i);assert(cellGcm_native_handoff_ring_watch(&w,&cookies[i])==0);}
    w=watch_at(0x120200);assert(cellGcm_native_handoff_ring_watch(&w,&cookie)==-4&&cookie==0);
    for(unsigned i=0;i<64;i++)assert(cellGcm_native_handoff_cancel(cookies[i])==0);
}
int main(void) {
    vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(vm_base!=MAP_FAILED);ppu_vm_size=0x4000000;ppu_hle_inject_base=0x3000000;
    memset(s_ea_address_table,0xff,sizeof(s_ea_address_table));s_ea_address_table[0]=1;
    s_gcm_syscall_mode=1;
    raw32(0x200000,0x100000);raw32(0x200004,0x101000);raw32(0x200008,0x100000);
    rsx_native_handoff_arm a={1,sizeof(a),17,91,0x200000,0x100000,0x0004183c,0,&callbacks,consume};
    uint64_t cookie=0;
    assert(cellGcm_native_handoff_arm(&a,&cookie)==0&&cookie);
    uint64_t duplicate=3;assert(cellGcm_native_handoff_arm(&a,&duplicate)==-4&&duplicate==0);
    cellGcm_syscall_set_fifo(8,0);
    cellGcm_rsx_process_fifo();assert(s_fifo_getoff==0&&callbacks==0);
    assert(cellGcm_native_handoff_publish(cookie)==-3); /* no real write yet */
    raw32(0x100004,0);raw32(0x200008,0x100008);raw32(0x100000,0x0004183c);
    assert(cellGcm_native_handoff_publish(cookie)==0);
    assert(cellGcm_native_handoff_publish(cookie)==-3);
    cellGcm_rsx_process_fifo();assert(s_fifo_getoff==8&&callbacks==1&&last_cookie==cookie);
    assert(cellGcm_native_handoff_cancel(cookie)==-3);
    cellGcm_syscall_set_fifo(8,0);cellGcm_rsx_process_fifo();assert(callbacks==1); /* replay */
    raw32(0x200008,0x100000);
    assert(cellGcm_native_handoff_arm(&a,&cookie)==0&&cookie!=last_cookie);
    assert(cellGcm_native_handoff_cancel(cookie)==0);
    assert(cellGcm_native_handoff_publish(cookie)==-3);
    cellGcm_syscall_set_fifo(8,0);cellGcm_rsx_process_fifo();assert(callbacks==1);
    /* Published bytes changed before GET: no native callback is permissible. */
    raw32(0x200008,0x100000);
    assert(cellGcm_native_handoff_arm(&a,&cookie)==0);
    raw32(0x200008,0x100008);assert(cellGcm_native_handoff_publish(cookie)==0);
    raw32(0x100004,1);cellGcm_syscall_set_fifo(8,0);cellGcm_rsx_process_fifo();
    assert(callbacks==1&&cellGcm_native_handoff_cancel(cookie)==-3);
    /* Captured VM base is checked independently from opaque VM lifecycle ID. */
    raw32(0x200008,0x100000);assert(cellGcm_native_handoff_arm(&a,&cookie)==0);
    uint8_t* actual_vm=vm_base;vm_base+=4;assert(cellGcm_native_handoff_publish(cookie)==-3);
    vm_base=actual_vm;assert(cellGcm_native_handoff_cancel(cookie)==0);
    uint64_t cookies[64];
    for(unsigned i=0;i<64;i++) {
        a.context_ea=0x201000+i*16;a.header_ea=0x102000+i*8;
        raw32(a.context_ea,0x102000);raw32(a.context_ea+4,0x103000);raw32(a.context_ea+8,a.header_ea);
        assert(cellGcm_native_handoff_arm(&a,&cookies[i])==0);
    }
    a.context_ea+=16;a.header_ea+=8;
    raw32(a.context_ea,0x102000);raw32(a.context_ea+4,0x103000);raw32(a.context_ea+8,a.header_ea);
    assert(cellGcm_native_handoff_arm(&a,&cookie)==-4&&cookie==0);
    for(unsigned i=0;i<64;i++)assert(cellGcm_native_handoff_cancel(cookies[i])==0);
    assert(cellGcm_native_handoff_arm(&a,&cookie)==0&&cookie>cookies[63]);
    assert(cellGcm_native_handoff_cancel(cookie)==0);
    ring_writer_checks();
    wrapped_ring_writer_checks();
    ring_watch_checks();
    puts("PASS: exact FIFO source publication, pre-header barrier, replay/mutation/VM rejection, bounded capacity and cancellation; ring watch reports arrival, mismatch, skip, expiry and VM retirement once");return 0;
}
