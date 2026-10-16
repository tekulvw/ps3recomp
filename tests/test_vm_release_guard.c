/* Exercise actual allocator/free, shared-memory zero and protection entrypoints. */
#include "../runtime/syscalls/sys_memory.c"
#include <assert.h>
#include <stdlib.h>
uint8_t *vm_base;
uint32_t ppu_vm_size;
static unsigned begins,ends;
static int reject=1;
static uint64_t seen_ea,seen_size;
static int guard(uint64_t ea,uint64_t size){begins++;seen_ea=ea;seen_size=size;return !reject;}
static void finish(void){ends++;}
int main(void){
 vm_base=calloc(1,0x10000);assert(vm_base);
 ps3_vm_release_guard.begin=guard;ps3_vm_release_guard.end=finish;
 ppu_context ctx={0};ctx.gpr[3]=0x1000;
 g_sys_mem_allocs[0].active=1;g_sys_mem_allocs[0].addr=0x1000;g_sys_mem_allocs[0].size=0x1000;s_total_allocated=0x1000;
 assert(sys_memory_free(&ctx)==(int64_t)(int32_t)CELL_EBUSY);
 assert(g_sys_mem_allocs[0].active&&s_total_allocated==0x1000&&!ends);
 reject=0;assert(sys_memory_free(&ctx)==CELL_OK);assert(!g_sys_mem_allocs[0].active&&ends==1);
 reject=1;g_sys_mmapper_shared[0].active=1;g_sys_mmapper_shared[0].size=0x1000;g_sys_mmapper_shared[0].addr=0x8000;
 memset(vm_base+0x1000,0xa5,0x1000);ctx.gpr[4]=1;
 assert(sys_mmapper_map_shared_memory(&ctx)==(int64_t)(int32_t)CELL_EBUSY);
 assert(g_sys_mmapper_shared[0].addr==0x8000&&vm_base[0x1000]==0xa5&&ends==1);
 assert(vm_protect(0x1001,0x1000,0,0,0)==CELL_EBUSY);
 assert(seen_ea<=0x1001&&seen_ea+seen_size>=0x2001&&ends==1);
 uint8_t *saved=vm_base;vm_shutdown();assert(vm_base==saved&&ends==1);
 assert(seen_ea==0&&seen_size==UINT64_C(0x100000000));
 ps3_vm_release_guard.begin=NULL;ps3_vm_release_guard.end=NULL;free(vm_base);
 puts("PASS actual VM lifetime gates: rejected free/map/protect/shutdown preserve state; lease balanced");
}
