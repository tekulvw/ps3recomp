/* Compiles the actual GCM implementation; no graphics backend is exercised. */
#include "../cellGcmSys.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w) { (void)a;(void)v;(void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a;return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
static int failures,checks;
#define CHECK(x) do { checks++; if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);failures++;} } while(0)
static void setup(u32 begin,u32 end,u32 cur,u32 get)
{
 memset(vm_base,0,ppu_vm_size);memset(s_io_address_table,0xFF,sizeof(s_io_address_table));
 s_io_address_table[1]=0;s_display_buffer_set[0]=1;s_fifo_getoff=get;
 vm_write32(0x100,begin);vm_write32(0x104,end);vm_write32(0x108,cur);
}
static void no_wrap(u32 begin,u32 end,u32 cur,u32 get)
{
 setup(begin,end,cur,get);vm_write32(begin,0xDEADBEEF);
 CHECK(gcm_flip_into_fifo(0x100,0)==1);
 CHECK(vm_read32(cur)==GCM_FLIP_MARKER);
 CHECK(vm_read32(0x108)==cur+4);
 CHECK(vm_read32(begin)==0xDEADBEEF);
 CHECK(vm_read32(cur+4)==0);
 /* The next closing reference remains adjacent to the flip, not at a
  * prematurely rewound head that the consumer has not reached. */
 const u32 next=vm_read32(0x108);vm_write32(next,0x00040050);vm_write32(next+4,0x3C8C1);
 CHECK(next==cur+4);CHECK(vm_read32(cur+4)==0x40050);
 CHECK(vm_read32(cur+8)==0x3C8C1);
}
int main(void)
{
 ppu_vm_size=0x200000;vm_base=calloc(1,ppu_vm_size);assert(vm_base);
 /* MAG: consumer is still in previous segment when title has switched. */
 no_wrap(0x104980,0x10696C,0x104A90,0xA0000);
 /* A full-size ring must also reject a consumer outside its own extent. */
 no_wrap(0x110000,0x150000,0x14D000,0x90000);
 /* A valid consumed head in the same large ring retains existing wrap. */
 setup(0x110000,0x150000,0x14D000,0x40000);
 CHECK(gcm_flip_into_fifo(0x100,0)==1);
 CHECK(vm_read32(0x14D000)==GCM_FLIP_MARKER);
 CHECK(vm_read32(0x14D004)==(0x20000000|0x10000));
 CHECK(vm_read32(0x108)==0x110000);
 free(vm_base);printf("GCM flip-wrap checks: %d, failures: %d\n",checks,failures);return failures?1:0;
}
