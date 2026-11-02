/* A missing pre-header barrier would execute UI state before publication;
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
    puts("PASS: exact FIFO source publication, pre-header barrier, replay/mutation/VM rejection, bounded capacity and cancellation");return 0;
}
