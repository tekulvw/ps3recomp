/* CPU-only wiring regression: real common engines, injected non-GPU backend.
 * No submission/completion or performance evidence is produced by this test. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../rsx_draw_engine.h"
#include "../rsx_commands.h"
#include "../rsx_native_owner_gate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static rsx_native_owner_gate owner=RSX_NATIVE_OWNER_GATE_INIT;
static rsx_native_owner_thread thread;
static unsigned partials,initializations,teardowns;
u8* vm_base;u32 ppu_vm_size;
u32 ppu_hle_inject_base;
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t address,uint32_t value){(void)address;(void)value;assert(!"fixture must not publish guest data");}
void ps3_ms(const char* key){(void)key;assert(!"fixture must not issue a draw/flip");}
unsigned long long ps3_ms_now(void){assert(!"fixture has no guest notification clock");return 0;}
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;return offset;}
u32 cellGcmResolveIO(u32 offset){return offset;}
void cellGcm_replay_display_buffers_to_engine(void){}
/* This ownership-only fixture has no diagnostics/ablation controller. Explicit
 * inert seams keep those unrelated pthread providers out of Windows linkage. */
uint64_t ps3_critical_now_ns(void){return 0;}
int rsx_render_ablation_init(void){return 0;}
int rsx_render_ablation_skip_prepare(void){return 0;}
static int enter(void* ignored,uint32_t flags){
    (void)ignored;if(rsx_owner_original_begin(&owner,&thread))return 0;
    if((flags&RSX_ORIGINAL_LIFECYCLE_BIT)&&owner.retained){rsx_owner_original_end(&owner,&thread);return 0;}
    if(flags&RSX_ORIGINAL_MUTATE)rsx_owner_dirty(&owner,&thread);
    return 1;
}
static void leave(void* ignored,uint32_t flags,int delta,int preserve){
    (void)ignored;
    if(flags&RSX_ORIGINAL_FAILURE_BIT)rsx_owner_submitted(&owner,&thread,0);
    if(delta>0)++partials;else if(delta<0){assert(partials);--partials;}
    if(!preserve||partials)rsx_owner_dirty(&owner,&thread);
    rsx_owner_original_end(&owner,&thread);
}
static int backend_init(void* user,u32 w,u32 h){(void)user;(void)w;(void)h;++initializations;return 0;}
static void backend_shutdown(void* user){(void)user;++teardowns;}
static int checked(void* user,u32 reason){(void)user;(void)reason;return 1;}
int main(void){
    const rsx_original_owner_hooks hooks={NULL,enter,leave};
    rsx_draw_engine_set_owner_hooks(&hooks);rsx_commands_set_owner_hooks(&hooks);
    rsx_draw_backend backend={0};backend.init=backend_init;backend.shutdown=backend_shutdown;backend.submit_and_wait_checked=checked;
    rsx_draw_engine_set_backend(&backend);assert(rsx_draw_engine_init(8,8)==0);assert(initializations==1);
    rsx_state state;rsx_state_init(&state);
    assert(rsx_owner_original_begin(&owner,&thread)==0);
    assert(rsx_owner_generation(&owner,&thread,1)==0);
    /* Simulated clean starting state only; this is NOT a GPU completion. */
    rsx_owner_submitted(&owner,&thread,1);rsx_owner_original_end(&owner,&thread);
    assert(rsx_owner_retain(&owner,&thread,1)==0);
    assert(rsx_owner_native_begin(&owner,&thread,1)==0);rsx_owner_native_end(&owner,&thread);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END,5,0);assert(partials==1);
    assert(rsx_owner_native_begin(&owner,&thread,1)==-4);
    assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==0);assert(partials==2);
    assert(rsx_owner_native_begin(&owner,&thread,1)==-4);
    rsx_draw_engine_shutdown();assert(teardowns==0);assert(rsx_draw_engine_checked_ready());
    assert(rsx_draw_engine_init(8,8)==-1 && initializations==1);
    rsx_draw_engine_method(NV4097_SET_BEGIN_END,0);assert(partials==1);
    assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,0)==0);assert(!partials);
    assert(rsx_owner_native_begin(&owner,&thread,1)==-4); /* ending never fabricates a clean boundary */
    assert(rsx_owner_release(&owner,&thread,1)==0);
    assert(rsx_process_method(&state,NV4097_SET_BEGIN_END,5)==0 && partials==1);
    rsx_state_init(&state);assert(!partials && !state.in_begin_end);
    rsx_state* streams=calloc(300,sizeof(*streams));assert(streams);
    for(unsigned i=0;i<300;++i){
        rsx_state_init(&streams[i]);
        assert(rsx_process_method(&streams[i],NV4097_SET_BEGIN_END,5)==0 && partials==1);
        assert(rsx_process_method(&streams[i],NV4097_SET_BEGIN_END,0)==0 && !partials);
    }
    assert(rsx_owner_retain(&owner,&thread,1)==0);
    for(unsigned i=0;i<256;++i){
        assert(rsx_process_method(&streams[i],NV4097_SET_BEGIN_END,5)==0);
    }
    assert(partials==256);
    const u32 packet[2]={(1u<<18)|NV4097_SET_BEGIN_END,5};
    assert(rsx_process_command_buffer(&streams[256],packet,sizeof(packet))==RSX_ORIGINAL_OWNER_REFUSED);
    assert(!streams[256].in_begin_end && partials==256);
    for(unsigned i=0;i<256;++i)assert(rsx_process_method(&streams[i],NV4097_SET_BEGIN_END,0)==0);
    assert(!partials);
    assert(rsx_owner_original_begin(&owner,&thread)==0);
    rsx_owner_submitted(&owner,&thread,1);rsx_owner_original_end(&owner,&thread);
    assert(rsx_owner_native_begin(&owner,&thread,1)==-6); /* END/flush cannot requalify invalid lifetime */
    assert(rsx_process_method(&streams[0],NV4097_SET_BEGIN_END,5)==0 && partials==1);
    assert(rsx_owner_release(&owner,&thread,1)==0);
    assert(rsx_owner_original_begin(&owner,&thread)==0);
    assert(rsx_owner_generation(&owner,&thread,2)==0);rsx_owner_original_end(&owner,&thread);
    assert(partials==1); /* new owner generation does not erase actual open CPU work */
    assert(rsx_process_method(&streams[0],NV4097_SET_BEGIN_END,0)==0 && !partials);
    assert(rsx_process_method(&streams[256],NV4097_SET_BEGIN_END,5)==0 && partials==1);
    rsx_state_init(&streams[256]);assert(!partials);free(streams);
    rsx_draw_engine_shutdown();assert(teardowns==1);
    rsx_draw_engine_set_owner_hooks(NULL);rsx_commands_set_owner_hooks(NULL);
    puts("Real common owner ingress: CPU-only BEGIN/state, nested scopes, and retained lifecycle refusal passed");
    return 0;
}
