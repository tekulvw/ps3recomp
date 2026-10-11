/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x10000;
uint32_t g_ww_lo=0,g_ww_hi=0;
int g_resv_store_active=0;
ps3_guest_caller_fn g_ps3_guest_caller=nullptr;
void ppu_hle_register_all();
int ps3_hle_has(uint32_t);
void ps3_hle_call(uint32_t,ppu_context*);
uint32_t prx_resolve_export(uint32_t) { return 0; }
void ps3_indirect_call(ppu_context*) { std::abort(); }
uint64_t ppu_guest_call(uint32_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t) { std::abort(); }
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
static uint32_t call(uint32_t nid,uint64_t a=0,uint64_t b=0,uint64_t c=0,uint64_t d=0) {
 ppu_context ctx{};ctx.gpr[1]=0x8000;ctx.gpr[2]=0x12345678;
 ctx.gpr[3]=a;ctx.gpr[4]=b;ctx.gpr[5]=c;ctx.gpr[6]=d;ps3_hle_call(nid,&ctx);
 if(ctx.gpr[2]!=0x12345678)std::abort();return uint32_t(ctx.gpr[3]);
}
static bool fill(uint32_t a,size_t n,uint8_t v) {for(size_t i=0;i<n;i++)if(vm_base[a+i]!=v)return false;return true;}
static void w16(uint32_t a,uint16_t x){vm_base[a]=x>>8;vm_base[a+1]=x;}
int main(int argc,char**argv) {
 static uint8_t memory[0x10000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t R=0x1000,H=0x2000,O=0x4000;
 constexpr uint32_t INIT=0xb8436ee5,COUNT=0x37f723f7,SPEC=0xd0230671,SET=0x461534b4,INFO=0x67fd273b,START=0x4de501b1,END=0xf61609d6;
 constexpr uint32_t INVALID=0x80610503,BAD=0x80610502;
 // Independent synthetic 0041 single-group header, M2V + ATRAC3+.
 memcpy(vm_base+H,"PAMF0041",8);vm_write32(H+8,1);vm_write32(H+12,0x535);
 vm_write32(H+0x50,0x94);vm_write32(H+0x58,0x15cde);vm_write32(H+0x5e,0x99351);
 vm_write32(H+0x62,0x1d4c0);vm_write32(H+0x6a,2);vm_base[H+0x6f]=1;
 vm_write32(H+0x70,0x74);vm_write32(H+0x76,0x15cde);vm_write32(H+0x7c,0x99351);
 vm_base[H+0x81]=1;vm_write32(H+0x82,0x62);vm_base[H+0x87]=2;
 vm_base[H+0x88]=2;vm_base[H+0x8c]=0xe0;vm_base[H+0x98]=0x44;vm_base[H+0x9a]=0xc4;vm_base[H+0x9b]=1;
 vm_base[H+0xa1]=80;vm_base[H+0xa3]=45;w16(H+0xa4,1280);w16(H+0xa6,720);
 vm_base[H+0xac]=0xa0;vm_base[H+0xad]=1;vm_base[H+0xae]=1;vm_base[H+0xaf]=1;
 vm_base[H+0xb8]=0xdc;vm_base[H+0xbc]=0xbd;vm_base[H+0xca]=2;vm_base[H+0xcb]=1;
 uint64_t fileSize=2732032;
 if(argc==2){FILE*f=fopen(argv[1],"rb");CHECK(f);fseek(f,0,SEEK_END);fileSize=ftell(f);rewind(f);CHECK(fread(vm_base+H,1,2048,f)==2048);fclose(f);}
 memset(vm_base+R-4,0xa5,136);
 CHECK(call(INIT,R,H,fileSize,1)==0);
 CHECK(vm_read32(R+0x24)==H); // baseline falsely keeps host-only reader state
 CHECK(vm_read64(R)==2048&&vm_read64(R+8)==2729984);
 CHECK(vm_read32(R+0x28)==H+0x50&&vm_read32(R+0x34)==H+0x88);
 CHECK(fill(R-4,4,0xa5)&&fill(R+128,4,0xa5));
 CHECK(call(COUNT,R)==2);CHECK(call(SPEC,R,20)==1);CHECK(call(SPEC,R,21)==1);
 CHECK(call(SPEC,R,1)==1);CHECK(call(SPEC,R,2)==1);CHECK(call(SPEC,R,0)==0);
 CHECK(call(START,R,O)==0&&vm_read32(O)==0&&vm_read32(O+4)==0x15cde);
 CHECK(call(END,R,O)==0&&vm_read32(O)==0&&vm_read32(O+4)==0x99351);
 memset(vm_base+O-4,0xa5,40);CHECK(call(INFO,R,O,24)==0);
 CHECK(vm_base[O]==3&&vm_base[O+1]==1&&vm_base[O+2]==1&&vm_base[O+3]==4);
 CHECK(vm_base[O+10]==5&&vm_base[O+11]==0&&vm_base[O+12]==2&&vm_base[O+13]==0xd0);
 CHECK(fill(O-4,4,0xa5)&&fill(O+24,12,0xa5));
 CHECK(call(SET,R,1)==0);CHECK(call(INFO,R,O,8)==0);CHECK(vm_read32(O)==1&&vm_base[O+4]==2);
 memcpy(vm_base+0x1200,vm_base+R,128);CHECK(call(COUNT,0x1200)==2);CHECK(call(INFO,0x1200,O,8)==0);
 CHECK(call(SET,R,2)==INVALID);CHECK(call(INFO,R,O,7)==INVALID);
 CHECK(call(INIT,0,H,fileSize,1)==INVALID);CHECK(call(INIT,0xffc0,H,fileSize,1)==INVALID);
 CHECK(call(INIT,R,0xfffffff0,fileSize,1)==INVALID);CHECK(call(INIT,R,H,fileSize-1,1)==BAD);
 CHECK(call(INIT,R,H,fileSize+(1ull<<32),1)==BAD); // real HLE must preserve u64 fileSize
 vm_write32(H+8,0xffffffff);CHECK(call(INIT,R,H,0,1)==BAD);vm_write32(H+8,1);
 vm_base[H+0x87]=255;CHECK(call(INIT,R,H,fileSize,1)==BAD);vm_base[H+0x87]=2;
 vm_write32(H+0x82,0xffffffff);CHECK(call(INIT,R,H,fileSize,1)==BAD);vm_write32(H+0x82,0x62);
 vm_base[H+7]='9';CHECK(call(INIT,R,H,fileSize,1)==0x80610505);vm_base[H+7]='1';
 vm_base[H]='X';CHECK(call(INIT,R,H,fileSize,1)==0x80610504);vm_base[H]='P';
 CHECK(call(INIT,R,H,fileSize,1)==0);CHECK(call(INFO,R,0xfffffff0,24)==INVALID);
 CHECK(call(COUNT,0xfff0)==INVALID);CHECK(call(COUNT,0x1800)==BAD);
 CHECK(ps3_hle_has(0x28b4e2c1));
 CHECK(call(0x28b4e2c1,R,21,0)==1);CHECK(vm_read32(R+0x20)==1);
 CHECK(call(0x28b4e2c1,R,20,0)==0);CHECK(call(0x28b4e2c1,R,21,2)==INVALID);
 CHECK(call(0x28b4e2c1,R,20,1)==0x80610501);
 // Type-group selection used by retail: audio/video are categories, not coding bytes.
 CHECK(call(0xb1de0854,R,21,0)==0);CHECK(vm_read32(R+0x20)==1);CHECK(vm_read32(R+0x34)==H+0xb8);
 CHECK(call(0xb1de0854,R,20,0)==0);CHECK(vm_read32(R+0x20)==0);
 CHECK(call(0xb1de0854,R,20,1)==0x80610501);CHECK(vm_read32(R+0x20)==0);
 CHECK(call(0x41cc708,R)==0);CHECK(call(0xdb70296c,R)==120000);
 CHECK(call(0xd9ea3457,R)==0); // API returns count directly, no output pointer.
 memset(vm_base+O-4,0xa5,24);CHECK(call(0x1067e22,0,3,O)==0);
 CHECK(vm_read32(O)==0xe3&&vm_read32(O+4)==0&&vm_read32(O+8)==1&&vm_read32(O+12)==0);
 CHECK(fill(O-4,4,0xa5)&&fill(O+16,4,0xa5));CHECK(call(0x1067e22,3,7,O)==0);
 CHECK(vm_read32(O)==0xbd&&vm_read32(O+4)==0x47);CHECK(call(0x1067e22,0,16,O)==INVALID);
 CHECK(call(0x1067e22,0,0,0xfffffff0)==INVALID);
 // AVC metadata layout, including big-endian crop and SAR fields.
 vm_base[H+0x88]=0x1b;vm_base[H+0x98]=100;vm_base[H+0x99]=41;vm_base[H+0x9a]=0xc4;vm_base[H+0x9b]=255;
 w16(H+0x9c,4);w16(H+0x9e,3);w16(H+0xa4,1);w16(H+0xa6,2);w16(H+0xa8,3);w16(H+0xaa,4);
 vm_base[H+0xb0]=0xd2;vm_base[H+0xb1]=0x55;
 memset(vm_base+O-4,0xa5,40);CHECK(call(INFO,R,O,31)==INVALID);CHECK(fill(O-4,40,0xa5));
 CHECK(call(INFO,R,O,32)==0);CHECK(vm_base[O]==100&&vm_base[O+1]==41&&vm_base[O+4]==3);
 CHECK(vm_base[O+6]==0&&vm_base[O+7]==4&&vm_base[O+9]==3);
 CHECK(vm_base[O+15]==1&&vm_base[O+17]==2&&vm_base[O+19]==3&&vm_base[O+21]==4);
 CHECK(vm_base[O+27]==1&&vm_base[O+28]==1&&vm_base[O+29]==1&&vm_base[O+30]==2&&vm_base[O+31]==0x55);
 CHECK(fill(O-4,4,0xa5)&&fill(O+32,4,0xa5));
 // LPCM output returns enum codes, including16-bit bitsPerSample at+6.
 vm_base[H+0xb8]=0x80;vm_base[H+0xcd]=0;vm_base[H+0xcc]=0xc0;
 CHECK(call(SET,R,1)==0);CHECK(call(INFO,R,O,8)==0);CHECK(vm_read32(O)==1&&vm_base[O+4]==2&&vm_base[O+6]==0&&vm_base[O+7]==3);
 CHECK(call(START,R,0xfffc)==INVALID);CHECK(call(END,R,0)==INVALID);
 CHECK(call(INIT,R,H,fileSize,4)==INVALID);CHECK(call(INIT,H,H,fileSize,1)==INVALID);
 // Querying a copied reader never consumes one of the old eight host slots.
 for(unsigned i=0;i<12;i++){memcpy(vm_base+0x5000+i*128,vm_base+R,128);CHECK(call(COUNT,0x5000+i*128)==2);}
 // Nested and unknown-type failures leave the caller-owned reader untouched.
 memset(vm_base+0x1800,0x5a,128);vm_base[H+0x6f]=0;
 CHECK(call(INIT,0x1800,H,fileSize,1)==BAD);CHECK(fill(0x1800,128,0x5a));vm_base[H+0x6f]=2;
 CHECK(call(INIT,0x1800,H,fileSize,1)==0x80610508);CHECK(fill(0x1800,128,0x5a));vm_base[H+0x6f]=1;
 vm_base[H+0x88]=0xff;CHECK(call(INIT,0x1800,H,fileSize,1)==BAD);CHECK(fill(0x1800,128,0x5a));vm_base[H+0x88]=0x1b;
 vm_write32(H+0x90,2040);vm_write32(H+0x94,1);CHECK(call(INIT,0x1800,H,fileSize,1)==BAD);
 vm_write32(H+0x90,0);vm_write32(H+0x94,0);
 // MINIMUM_HEADER needs only structural metadata, not all declared padding.
 memcpy(vm_base+0xf000,vm_base+H,0xe8);vm_write32(0xf008,3);
 CHECK(call(INIT,R,0xf000,0,2)==0);CHECK(call(COUNT,R)==2);
 CHECK(call(INIT,R,0xf000,0,1)==BAD);
 printf("PAMF metadata: %u checks PASS%s\n",checks,argc==2?" (external PAMF header)":"");return 0;
}
