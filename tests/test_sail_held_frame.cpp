/* Exercise generated NID registration and the real PPU HLE bridge. */
#include "ppu_context.h"
#include "ps3emu/guest_call.h"
#include "../libs/codec/sail_video_decoder.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include <stdexcept>

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x800000;
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
 if(opd==0x3010) {if(arg!=0x11223344||boundary!=128||!size)std::abort();ppu_context nested{};nested.gpr[3]=0x1800;ps3_hle_call(0x18B4629D,&nested);if(uint32_t(nested.gpr[3])!=0x80610702)std::abort();++free_count;return 0;}
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

struct Event { uint32_t major,minor;uint64_t arg0,arg1; };
static std::vector<Event> events;
static bool destroy_on_close=false, callback_ok=true;
static uint32_t active_desc;
static void event_callback(uint32_t,uint64_t,uint64_t ev,uint64_t a,uint64_t b,uint64_t,uint64_t,uint64_t,uint64_t) {
 events.push_back({uint32_t(ev>>32),uint32_t(ev),a,b});
}
static std::vector<unsigned> graphics;
static unsigned frame_frees=0, scratch_frees=0;
static bool missing_free=false;
static const char* mode="happy";
static bool is_mode(const char* value){return !std::strcmp(mode,value);}
extern "C" int ppu_guest_call_ct_checked(uint32_t code,uint32_t toc,
 uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t*out) {
 if(!out||toc!=0x12345678)std::abort();*out=0;
 if(code==0x5678) {++scratch_frees;return 1;}
 if(code==0x1234) {*out=0x20000;return 1;}
 if(code==0x104&&missing_free)return 0;
 if(code<0x100||code>0x104||a!=0xABCDEF)std::abort();
 graphics.push_back(code);
 // Original allocation/cleanup callbacks may attempt player/adapter teardown.
 if(call(0x76488BB1,0x1800)!=0x80610702||call(0x18B4629D,0x1000)!=0x80610702)std::abort();
 if(code==0x103) {if(b!=3686400||c!=1||!d)std::abort();vm_write32(d,is_mode("invalid-buffer")?0x7FFFF0:0x400000);
 if(is_mode("allocation-failure")){vm_write32(d,0);*out=uint64_t(-1);}}
 if(code==0x102) {if(c!=1||vm_base[b]!=1||vm_read32(b+8)!=5120)std::abort();}
 if(code==0x104) {if(b!=1||(vm_read32(c)!=0x400000&&!is_mode("invalid-buffer")))std::abort();++frame_frees;
 if(is_mode("negative-free"))*out=uint64_t(-1);
 if(is_mode("uncertain-free"))throw std::runtime_error("interrupted original free");}
 return 1;
}
extern "C" int32_t cellGcmAddressToOffset(uint32_t a,uint32_t*out) {
 if(a!=0x400000)std::abort();vm_write32(uintptr_t(out),0x1000);return 0;
}
extern "C" int32_t cellGcmGetConfiguration(void* out) {
 uint32_t ea=uintptr_t(out);vm_write32(ea,is_mode("unmapped-tail")?0:0x400000);vm_write32(ea+8,0x400000);return 0;
}
extern "C" int32_t cellGcmGetOffsetTable(void* out) {
 uint32_t ea=uintptr_t(out);vm_write32(ea,0x30000);std::memset(vm_base+0x30000,0xFF,8192);
 vm_base[0x30008]=0;vm_base[0x30009]=1;return 0;
}
extern "C" int sail_video_available() {return 1;}
extern "C" int sail_video_decode_picture(FILE*,uint64_t off,uint64_t length,unsigned sid,unsigned ordinal,SailVideoFrame*out) {
 if(is_mode("decode-failure"))return SAIL_VIDEO_DECODE;
 if(off!=2048||length!=2048||sid!=0x1E0||ordinal!=30)std::abort();
 out->rgba=(uint8_t*)std::malloc(3686400);std::memset(out->rgba,0xAD,3686400);
 out->width=1280;out->height=720;out->pitch=5120;out->ordinal=30;
 out->pts=177087;out->pts_valid=1;out->time_base_num=1;out->time_base_den=90000;
 out->rate_num=30000;out->rate_den=1001;out->colorspace=1;out->color_range=1;return 0;
}
extern "C" void sail_video_frame_release(SailVideoFrame*f) {std::free(f->rgba);std::memset(f,0,sizeof(*f));}
int main(int argc,char**argv) {
 static uint8_t memory[0x800000];vm_base=memory;ppu_hle_register_all();
 constexpr uint32_t INIT=0x23654375,ALLOC=0x346EBBA3,CREATE=0xD7938B8D,OPEN=0xC044FAB1;
 constexpr uint32_t START=0xE535B0D3,STOP=0xEBA8D4EC; // replaced below from NID database
 CHECK(argc==2);if(const char* value=std::getenv("SAIL_TEST_MODE"))mode=value;
 vm_write32(0x3000,0x1234);vm_write32(0x3004,0x12345678);
 vm_write32(0x3010,0x5678);vm_write32(0x3014,0x12345678);
 vm_write32(0x2000,0x3000);vm_write32(0x2004,0x3010);
 CHECK(call(ALLOC,0x2100,0x2000,0x11223344)==0);
 CHECK(call(INIT,0x1000,0x2100,0x3200,0x99887766)==0);
 g_ps3_guest_caller=event_callback;
 for(unsigned i=0;i<5;++i){vm_write32(0x2200+4*i,0x2300+16*i);vm_write32(0x2300+16*i,0x100+i);vm_write32(0x2304+16*i,0x12345678);}
 CHECK(call(0x1C983864,0x1800,0x2200,0xABCDEF)==0);CHECK(call(0x18BCD21B,0x1000,0,0x1800)==0);
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",is_mode("unsupported-source")?"mag_intro.pam":argv[1]);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);active_desc=vm_read32(0x4200);
 CHECK(call(OPEN,active_desc)==0);CHECK(call(0x7C8DFF3B,0x1000,active_desc)==0);
 CHECK(call(0x0ABB318B,active_desc,0x4400,128,0)==0);
 CHECK(call(0x0D0C2F0C,active_desc,1,0,1,0x4400)==0); // SetEs replaced below
 CHECK(call(0x34ECC1B9,0x1000,0x1111222233334444ULL)==0);events.clear();
 if(is_mode("decode-failure")||is_mode("allocation-failure")||is_mode("unsupported-source")||is_mode("unmapped-tail")) {
  CHECK(call(START,0x1000,0x4500,0xAABBCCDDEEFF0011ULL)!=0);
  CHECK(is_mode("unmapped-tail")?frame_frees==1:frame_frees==0);CHECK(call(0x0247C69E,0x1800,0x4600)==0);
  size_t closed=events.size(),error=events.size();
  for(size_t i=0;i<events.size();++i){CHECK(!(events[i].major==3&&events[i].arg0==6));if(events[i].major==5)closed=i;if(events[i].major==1)error=i;}
  CHECK(closed<error&&error<events.size());
  CHECK(call(0xFC839BD4,0x1000,active_desc)==0);CHECK(call(0x18B4629D,0x1000)==0);CHECK(call(0x76488BB1,0x1800)==0);
  CHECK((is_mode("decode-failure")||is_mode("unsupported-source"))?scratch_frees==0:scratch_frees==1);
  std::printf("%s %u held failure/cleanup checks PASS\n",mode,checks);return 0;
 }
 if(is_mode("invalid-buffer")) {
  CHECK(call(START,0x1000,0x4500,0xAABBCCDDEEFF0011ULL)==0x80610702);
  CHECK(frame_frees==0&&scratch_frees==0);CHECK(call(STOP,0x1000,0)==0x80610702);
  CHECK(call(0x0247C69E,0x1800,0x4600)==0);CHECK(call(0x76488BB1,0x1800)==0x80610702);
  std::printf("invalid buffer %u quarantine checks PASS\n",checks);return 0;
 }
 CHECK(call(START,0x1000,0x4500,0xAABBCCDDEEFF0011ULL)==0);
 CHECK(graphics==std::vector<unsigned>({0x100,0x103,0x102}));
 CHECK(events.size()==3&&events[0].arg0==5&&events[1].minor==10&&events[1].arg1==0xAABBCCDDEEFF0011ULL&&events[2].arg0==6);
 CHECK(call(0x0247C69E,0x1800,0x4600)==0x200);CHECK(vm_read32(0x4600)==0x400000&&vm_read32(0x4604)==1);
 CHECK(memory[0x400000]==0xAD&&memory[0x400000+3686399]==0xAD);
 if(is_mode("direct-close")) {
  CHECK(call(0x85BEFFCC,0x1000,0)==0);CHECK(frame_frees==1&&scratch_frees==1);
  CHECK(call(0x0247C69E,0x1800,0x4600)==0);CHECK(call(0xFC839BD4,0x1000,active_desc)==0);
  CHECK(call(0x18B4629D,0x1000)==0);CHECK(call(0x76488BB1,0x1800)==0);
  std::printf("direct close %u lifetime checks PASS\n",checks);return 0;
 }
 if(is_mode("negative-free")) {
  CHECK(call(STOP,0x1000,0)==0x80610702);CHECK(frame_frees==1);CHECK(call(STOP,0x1000,0)==0x80610702);CHECK(frame_frees==1);
  CHECK(call(0x0247C69E,0x1800,0x4600)==0);CHECK(call(0x76488BB1,0x1800)==0x80610702);
  std::printf("negative free %u quarantine checks PASS\n",checks);return 0;
 }
 if(is_mode("uncertain-free")) {
  bool caught=false;try{call(STOP,0x1000,0);}catch(const std::runtime_error&){caught=true;}
  CHECK(caught&&frame_frees==1);CHECK(call(STOP,0x1000,0)==0x80610702);CHECK(frame_frees==1);
  CHECK(call(0x0247C69E,0x1800,0x4600)==0);CHECK(call(0x76488BB1,0x1800)==0x80610702);
  std::printf("uncertain free %u quarantine checks PASS\n",checks);return 0;
 }
 missing_free=true;CHECK(call(STOP,0x1000,0x123456789ABCDEF0ULL)==0x80610708);
 CHECK(call(0x0247C69E,0x1800,0x4600)==0);CHECK(frame_frees==0);CHECK(call(0x76488BB1,0x1800)==0x80610702);
 missing_free=false;CHECK(call(STOP,0x1000,0x123456789ABCDEF0ULL)==0);
 CHECK(frame_frees==1&&scratch_frees==1);CHECK(graphics==std::vector<unsigned>({0x100,0x103,0x102,0x104,0x101}));
 CHECK(events.back().arg0==4);CHECK(events[events.size()-2].minor==11&&events[events.size()-2].arg1==0x123456789ABCDEF0ULL);
 CHECK(call(STOP,0x1000,0)==0);CHECK(frame_frees==1&&scratch_frees==1);
 CHECK(call(0x85BEFFCC,0x1000,0)==0);CHECK(call(0xFC839BD4,0x1000,active_desc)==0);
 CHECK(call(0x18B4629D,0x1000)==0);CHECK(call(0x76488BB1,0x1800)==0);
 std::printf("%u held frame callback/lifetime checks PASS\n",checks);return 0;
}
