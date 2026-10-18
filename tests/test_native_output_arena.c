/* Synthetic actual allocator/GCM registry lifecycle; no title data. */
#include "../libs/video/cellGcmSys.c"
#include "../runtime/syscalls/sys_memory.h"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t g_ww_lo,g_ww_hi;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;abort();}
void ppu_resv_break_store(uint64_t a){(void)a;}
int spu_coh_is_reserved(uint32_t a){(void)a;return 0;}
void spu_coh_notify_write(uint32_t a){(void)a;abort();}
void spu_lockline_lock(void){abort();}
void spu_lockline_unlock(void){abort();}
static int quiet(void* owner,const CellGcmNativeOutputMapping* mapping){assert(mapping->generation);return *(int*)owner;}
static int deny_vm(uint64_t ea,uint64_t bytes){(void)ea;(void)bytes;return 0;}
static unsigned allocations(void){unsigned n=0;for(unsigned i=0;i<SYS_MEMORY_ALLOC_MAX;++i)n+=g_sys_mem_allocs[i].active!=0;return n;}
int main(void){
    assert(vm_init()==CELL_OK);static lv2_syscall_table table;sys_memory_init(&table);
    assert(s_deferred_reports);s_gcm_initialized=1;
    memset(s_io_address_table,0xff,sizeof s_io_address_table);memset(s_ea_address_table,0xff,sizeof s_ea_address_table);
    s_config.ioSize=0x200000;s_ea_address_table[0]=s_ea_address_table[1]=0; /* Init window occupies io 0..2MB. */
    sys_memory_native_allocation a={0};assert(sys_memory_native_allocate(0x100001,0x100000,0x112233,&a)==CELL_OK);
    assert(a.ea>=0x40000000&&a.ea<0xc0000000&&a.size==0x200000&&a.vm_epoch&&a.generation&&!(a.ea&0xfffff));
    assert(allocations()==1);
    ppu_context budget={0};budget.gpr[3]=0x10000;
    assert(sys_memory_get_user_memory_size(&budget)==CELL_OK);assert(vm_read32(0x10004)==213u*1024u*1024u);
    assert(g_sys_mem_bump_ptr==0);
    ppu_context mapper_free={0};mapper_free.gpr[3]=a.ea;
    assert(sys_mmapper_free_address(&mapper_free)==(int64_t)(int32_t)CELL_EBUSY);
    uint32_t stolen=0;assert(sys_mmapper_search_map_address(a.ea,0x100000,&stolen)!=CELL_OK&&stolen==0);
    const uint64_t windows[][2]={{VM_HLE_HEAP_BASE,(uint64_t)VM_HLE_HEAP_BASE+VM_HLE_HEAP_SIZE},{VM_AUDIO_BASE,(uint64_t)VM_AUDIO_BASE+VM_AUDIO_SIZE},{VM_SYS_VM_BASE,(uint64_t)VM_SYS_VM_BASE+VM_SYS_VM_SIZE},{VM_HLE_INJECT_BASE,(uint64_t)VM_HLE_INJECT_BASE+VM_HLE_INJECT_SIZE}};
    for(unsigned i=0;i<4;++i)assert((uint64_t)a.ea+a.size<=windows[i][0]||a.ea>=windows[i][1]);
    assert(sys_memory_native_acquire(&a)==CELL_OK);assert(sys_memory_native_release(&a)==(int32_t)CELL_EBUSY);assert(sys_memory_native_unpin(&a)==CELL_OK);
    assert(sys_memory_native_range_live(a.ea,a.size));assert(!sys_memory_native_range_live(0x80000,0x1000));
    ppu_context c={0};c.gpr[3]=a.ea;assert(sys_memory_free(&c)==(int64_t)(int32_t)CELL_EBUSY);
    sys_memory_native_allocation bad=a;bad.generation++;assert(sys_memory_native_acquire(&bad)!=CELL_OK);
    CellGcmNativeOutputMapping mapping={0};assert(cellGcmNativeMapMainMemory(&a,&mapping)==CELL_OK);
    assert(mapping.allocation.ea==a.ea&&mapping.io==0x200000&&mapping.generation);
    assert(gcm_io2ea(mapping.io+123)==a.ea+123);
    assert(sys_memory_native_release(&a)==(int32_t)CELL_EBUSY);
    /* Native ownership makes teardown refusal explicit and leaves storage live. */
    vm_shutdown();assert(vm_base&&allocations()==1);
    sys_memory_init(&table);assert(sys_memory_native_acquire(&a)==CELL_OK);assert(sys_memory_native_unpin(&a)==CELL_OK);
    cellGcmTerminate();assert(s_gcm_initialized&&gcm_io2ea(mapping.io+123)==a.ea+123);
    assert(cellGcmUnmapEaIoAddress(a.ea)!=CELL_OK);assert(cellGcmUnmapIoAddress(mapping.io)!=CELL_OK);
    cellGcm_syscall_iounmap(mapping.io,a.size);assert(gcm_io2ea(mapping.io+123)==a.ea+123);
    int idle=0;assert(cellGcmNativeUnmapMainMemory(&mapping,quiet,&idle)!=CELL_OK);assert(gcm_io2ea(mapping.io+123)==a.ea+123);
    CellGcmNativeOutputMapping stale=mapping;idle=1;assert(cellGcmNativeUnmapMainMemory(&mapping,quiet,&idle)==CELL_OK);assert(!mapping.generation);
    assert(cellGcmNativeUnmapMainMemory(&stale,quiet,&idle)!=CELL_OK);
    const uint32_t ea=a.ea;const uint64_t generation=a.generation;
    assert(sys_memory_native_release(&a)==CELL_OK&&a.generation==0);assert(allocations()==0);
    assert(!sys_memory_native_range_live(ea,0x200000));
    assert(sys_memory_native_allocate(0x100000,0x100000,0x112233,&a)==CELL_OK&&a.ea==ea&&a.generation!=generation);
    assert(sys_memory_native_acquire(&stale.allocation)!=CELL_OK);
    /* Mapping failure leaves the caller's real allocation and IO page table intact. */
    static u16 io_pages[sizeof s_ea_address_table/sizeof s_ea_address_table[0]];memcpy(io_pages,s_ea_address_table,sizeof io_pages);
    for(unsigned i=0;i<CELL_GCM_MAX_IO_MAPPINGS;++i)s_io_mappings[i].active=1;
    assert(cellGcmNativeMapMainMemory(&a,&mapping)!=CELL_OK);assert(!memcmp(io_pages,s_ea_address_table,sizeof io_pages)&&allocations()==1);
    memset(s_io_mappings,0,sizeof s_io_mappings);s_io_mapping_count=0;
    /* Reserved IO space bounds the owned mapping. */
    s_io_map_reserved=0x10000000u-0x200000u;assert(cellGcmNativeMapMainMemory(&a,&mapping)!=CELL_OK);
    assert(!memcmp(io_pages,s_ea_address_table,sizeof io_pages)&&allocations()==1);s_io_map_reserved=0;
    assert(sys_memory_native_release(&a)==CELL_OK&&allocations()==0);
    /* Successful reset advances the allocator epoch; stale handles cannot pin. */
    sys_memory_init(&table);assert(sys_memory_native_acquire(&stale.allocation)!=CELL_OK);
    /* A full guest user budget does not prevent a registered native mapper arena. */
    ppu_context full={0};full.gpr[3]=213u*1024u*1024u;full.gpr[4]=SYS_MEMORY_PAGE_SIZE_64K;full.gpr[5]=0x10010;
    assert(sys_memory_allocate(&full)==CELL_OK);uint32_t guest_ea=vm_read32(0x10010);
    assert(sys_memory_get_user_memory_size(&budget)==CELL_OK&&vm_read32(0x10004)==0);
    ps3_vm_release_hooks original_guard=ps3_vm_release_guard;ps3_vm_release_guard.begin=deny_vm;
    assert(sys_memory_native_allocate(0x100000,0x100000,7,&a)==(int32_t)CELL_EBUSY&&!a.generation&&allocations()==1);
    ps3_vm_release_guard=original_guard;
    assert(sys_memory_native_allocate(0x100000,0x100000,7,&a)==CELL_OK);
    assert(a.ea>=guest_ea+213u*1024u*1024u||a.ea+a.size<=guest_ea);
    assert(sys_memory_get_user_memory_size(&budget)==CELL_OK&&vm_read32(0x10004)==0);
    s_deferred_reports=0;assert(cellGcmNativeMapMainMemory(&a,&mapping)!=CELL_OK);s_deferred_reports=1;
    assert(sys_memory_native_release(&a)==CELL_OK);
    full.gpr[3]=guest_ea;assert(sys_memory_free(&full)==CELL_OK);
    vm_shutdown();assert(!vm_base);
    puts("PASS native host output arena: real allocation/map generations, pins, GPU quiescence, failure rollback, reuse and epoch expiry");
}
