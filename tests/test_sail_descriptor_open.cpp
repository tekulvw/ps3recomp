/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ppu_hle_register_all();
int ps3_hle_has(uint32_t);
void ps3_hle_call(uint32_t,ppu_context*);
int cellfs_translate_path(const char*path,char*out,size_t size) {
 const char*prefix="/dev_bdvd/";if(std::strncmp(path,prefix,10))return -1;
 int n=std::snprintf(out,size,"%s",path+10);return n>=0&&size_t(n)<size?0:-1;
}
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
static uint32_t next_alloc=0x5000,alloc_count=0,free_count=0,reentry_count=0;
uint64_t ppu_guest_call(uint32_t opd,uint64_t arg,uint64_t boundary,uint64_t size,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) {
 if(opd==0x3010) {if(arg!=0x11223344||boundary!=128||size<0x5000)std::abort();++free_count;return 0;}
 if(opd==0x3000) {
 ppu_context nested{};nested.gpr[3]=0;ps3_hle_call(0xC044FAB1,&nested);
 if(uint32_t(nested.gpr[3])!=0x80610702)std::abort();++reentry_count;
if(arg!=0x11223344||boundary!=128)std::abort();++alloc_count;uint32_t out=next_alloc;next_alloc+=uint32_t(size);return out;} std::abort();
}

void ppu_prof_stamp(void*,unsigned) {}
void ppu_gcm_pump() {}
uint32_t ppu_prof_resolve_host(void*) { return 0; }
void ps3_msf(const char*,...) {}
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) {}
void spu_lockline_lock() {}
void spu_lockline_unlock() {}
void ppu_resv_break_store(uint64_t) {}
void ps3_ww_report_inline(uint32_t,uint64_t,int) {}
uint32_t vm_read32(uint64_t a) { uint32_t x;std::memcpy(&x,vm_base+a,4);return __builtin_bswap32(x); }
void vm_write32(uint64_t a,uint32_t x) { x=__builtin_bswap32(x);std::memcpy(vm_base+a,&x,4); }
}
extern "C" uint64_t vm_read64(uint64_t a) { uint64_t x;std::memcpy(&x,vm_base+a,8);return __builtin_bswap64(x); }
void vm_write64(uint64_t a,uint64_t x) { x=__builtin_bswap64(x);std::memcpy(vm_base+a,&x,8); }
extern "C" void ppu_dump_guest_stack(ppu_context*,const char*) {}
extern "C" void ppu_dump_bctrl_ring(uint32_t,const char*) {}
extern "C" void ppu_guest_callstack(const char*) {}
uint32_t g_ydkj_real_spurs_ea=0,g_ydkj_real_taskset_ea=0;

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1; } } while(0)
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0,uint64_t e=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ctx.gpr[7]=e;
 ps3_hle_call(nid,&ctx);if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}
int main(int argc,char**argv) {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x23654375,ALLOC=0x346EBBA3,CREATE=0xD7938B8D,OPEN=0xC044FAB1,TYPE=0xD5F9A15B,URI=0x4C191088,DB=0x0ABB318B,DEL=0xFC839BD4;
 constexpr uint32_t ARG=0x80610701,STATE=0x80610702,UNAVAILABLE=0x80610708;
 CHECK(argc==2);CHECK(ps3_hle_has(OPEN));
 vm_write32(0x3000,0x1234);vm_write32(0x3010,0x5678);
 vm_write32(0x2000,0x3000);vm_write32(0x2004,0x3010);vm_write32(0x2104,0x11223344);
 CHECK(call(ALLOC,0x2100,0x2000)==0);CHECK(call(INIT,0x1000,0x2100)==0);
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",argv[1]);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);uint32_t desc=vm_read32(0x4200);
 CHECK(call(URI,desc)==0);CHECK(call(OPEN,99)==ARG);CHECK(call(OPEN,desc)==0);CHECK(alloc_count==1);
 CHECK(call(OPEN,desc)==STATE);CHECK(alloc_count==1);
 CHECK(call(TYPE,desc)==0);uint32_t uri=call(URI,desc);CHECK(uri>=0x5000);CHECK(std::strcmp((char*)memory+uri,(char*)memory+0x4000)==0);
 memory[0x4000]='Z';CHECK(memory[uri]=='x');memory[0x4000]='x';
 std::memset(memory+0x4300,0xA5,136);CHECK(call(DB,desc,0x4300,127)==ARG);CHECK(memory[0x4300]==0xA5);
 CHECK(call(DB,desc,0x4300,128,1)==ARG);CHECK(call(DB,desc,0x4301,128)==ARG);
 CHECK(call(DB,desc,0xFFFC,128)==ARG);CHECK(call(DB,desc,0x4300,128)==0);CHECK(memory[0x4380]==0xA5);
 CHECK(vm_read32(0x4324)>=0x5000); // Actual public reader points into descriptor-owned header.
 vm_write32(0x2104,0xBADCAFE);CHECK(call(DEL,0x1000,desc)==0);CHECK(free_count==1);vm_write32(0x2104,0x11223344);CHECK(reentry_count==1);CHECK(call(OPEN,desc)==ARG);
 std::strcpy((char*)memory+0x4000,"https://example.invalid/movie.pam");CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);desc=vm_read32(0x4200);CHECK(call(OPEN,desc)==UNAVAILABLE);CHECK(call(DEL,0x1000,desc)==0);
 std::strcpy((char*)memory+0x4000,"x-cell-fs:///dev_bdvd/missing.pam");CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);desc=vm_read32(0x4200);CHECK(call(OPEN,desc)!=0);CHECK(call(DEL,0x1000,desc)==0);

 for(const char*bad:{"bad-magic.pam","bad-inner.pam","truncated.pam"}) {
  std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",bad);
  CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);desc=vm_read32(0x4200);
  CHECK(call(OPEN,desc)!=0);CHECK(call(DB,desc,0x4300,128)==STATE);CHECK(call(DEL,0x1000,desc)==0);
 }
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",argv[1]);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);desc=vm_read32(0x4200);
 const uint32_t saves=next_alloc;next_alloc=0;CHECK(call(OPEN,desc)==0x806107F0);
 next_alloc=0xFF80;CHECK(call(OPEN,desc)==0x806107F0);
 next_alloc=0x5001;CHECK(call(OPEN,desc)==0x806107F0);next_alloc=saves;
 CHECK(call(OPEN,desc)==0);CHECK(call(0x7C8DFF3B,0x1000,desc)==0);
 CHECK(call(0xF220E64F)==0); // verified Term NID substituted by script
 CHECK(call(OPEN,desc)==ARG);CHECK(free_count==5); // successful opens + malformed nested-header cleanup
 std::printf("%u descriptor open checks PASS\n",checks);return 0;
}
