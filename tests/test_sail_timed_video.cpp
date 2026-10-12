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
#include <array>
#include "../libs/codec/sail_graphics_state.h"
#include "../libs/video/rsx_picture_identity.h"

extern "C" {
uint8_t* vm_base;
uint32_t ppu_vm_size=0x2000000;
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
static uint64_t fake_clock=5000000, test_anchor=5000000;
static unsigned picture_count=179, expected_session=1;
static std::array<bool,4> owned{};
static unsigned allocations=0,decoder_closes=0;
static uint32_t buffer_ea(unsigned n){return 0x400000+n*0x400000;}
extern "C" uint64_t ppu_system_time_us(){return fake_clock;}


static const char* mode="happy";
static bool is_mode(const char* value){return !std::strcmp(mode,value);}
extern "C" int cellGcm_checked_reference_ready(){return !is_mode("checked-failure");}
extern "C" int ppu_guest_call_ct_checked(uint32_t code,uint32_t toc,
 uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t*out) {
 if(!out||toc!=0x12345678)std::abort();*out=0;
 if(code==0x5678) {++scratch_frees;return 1;}
 if(code==0x1234) {*out=b==128 && c==128?0x20000:0x24000;return 1;}
 if(code>=0x200&&code<=0x202){if(a!=0xAABBCC || (code==0x202 && (!b || c!=1)))std::abort();return 1;}
 if(code==0x104&&missing_free)return 0;
 if(code<0x100||code>0x104||a!=0xABCDEF)std::abort();
 graphics.push_back(code);
 // Original allocation/cleanup callbacks may attempt player/adapter teardown.
 if(call(0x76488BB1,0x1800)!=0x80610702||call(0x18B4629D,0x1000)!=0x80610702)std::abort();
 if(code==0x103) {
  if(b!=3686400 || (c!=4 && c!=1) || !d)std::abort();
  ++allocations;
  unsigned actual=0;
  for(unsigned i=0;i<c;++i){
   unsigned n=0;while(n<4 && owned[n])++n;
   if(n==4)std::abort();
   owned[n]=true;++actual;vm_write32(d+4*i,buffer_ea(n));
  }
  if(is_mode("duplicate-buffer"))vm_write32(d+4,vm_read32(d));
  if(is_mode("invalid-buffer"))vm_write32(d,0x1FFFFF0);
  if(c==4 && is_mode("partial-two")){owned[2]=owned[3]=false;vm_write32(d+8,0);vm_write32(d+12,0);*out=2;}
  if(c==4 && is_mode("partial-three")){owned[3]=false;vm_write32(d+12,0);*out=3;}
  if(is_mode("allocation-failure")){owned.fill(false);for(unsigned i=0;i<c;++i)vm_write32(d+4*i,0);*out=uint64_t(-1);}
 }
 if(code==0x102) {if(c!=expected_session||vm_base[b]!=1||vm_read32(b+8)!=5120)std::abort();}
 if(code==0x104) {
  if(b!=1)std::abort();
  uint32_t ea=vm_read32(c);unsigned n=0;while(n<4 && buffer_ea(n)!=ea)++n;
  if(n==4 || !owned[n])std::abort();
  if(is_mode("uncertain-free"))throw std::runtime_error("interrupted original free");
  if(is_mode("negative-free")){*out=uint64_t(-1);return 1;}
  owned[n]=false;++frame_frees;
 }
 return 1;
}
extern "C" int32_t cellGcmAddressToOffset(uint32_t a,uint32_t*out) {
 if(a!=0x400000)std::abort();vm_write32(uintptr_t(out),0x1000);return 0;
}
extern "C" int32_t cellGcmGetConfiguration(void* out) {
 uint32_t ea=uintptr_t(out);vm_write32(ea,is_mode("unmapped-tail")?0:0x400000);vm_write32(ea+8,0x1000000);return 0;
}
extern "C" int32_t cellGcmGetOffsetTable(void* out) {
 uint32_t ea=uintptr_t(out);vm_write32(ea,0x30000);std::memset(vm_base+0x30000,0xFF,8192);
 vm_base[0x30008]=0;vm_base[0x30009]=1;return 0;
}
extern "C" int sail_video_available() {return 1;}
struct SailVideoDecoder {unsigned ordinal;};
extern "C" int sail_video_open(FILE*,uint64_t off,uint64_t length,unsigned sid,SailVideoDecoder** out) {
 if(off!=2048||length!=2048||sid!=0x1E0)std::abort();
 if(is_mode("decode-failure"))return SAIL_VIDEO_DECODE;
 *out=new SailVideoDecoder{};return 0;
}
extern "C" int sail_video_next(SailVideoDecoder* decoder,SailVideoFrame*out) {
 unsigned ordinal=++decoder->ordinal;
 if((is_mode("truncated") || is_mode("intro-truncated")) && ordinal==70)return SAIL_VIDEO_EOF;
 if(ordinal==picture_count+1)return (is_mode("drain-failure") || is_mode("intro-drain-failure"))?SAIL_VIDEO_DECODE:SAIL_VIDEO_EOF;
 if(ordinal>picture_count+1)std::abort();
 out->rgba=(uint8_t*)std::malloc(3686400);std::memset(out->rgba,ordinal,3686400);
 out->width=1280;out->height=720;out->pitch=5120;out->ordinal=ordinal;
 out->duration=3003;out->pts_valid=ordinal<picture_count;out->pts=ordinal<picture_count?90000+(ordinal-1)*3003:INT64_MIN;
 if(is_mode("unknown-nonterminal") && ordinal==15){out->pts_valid=0;out->pts=INT64_MIN;}
 out->time_base_num=1;out->time_base_den=90000;out->rate_num=30000;out->rate_den=1001;
 out->colorspace=1;out->color_range=1;return 0;
}
extern "C" void sail_video_close(SailVideoDecoder* decoder){++decoder_closes;delete decoder;}
extern "C" int sail_video_decode_picture(FILE*,uint64_t,uint64_t,unsigned,unsigned,SailVideoFrame*) {return SAIL_VIDEO_UNAVAILABLE;}
extern "C" void sail_video_frame_release(SailVideoFrame*f) {std::free(f->rgba);std::memset(f,0,sizeof(*f));}
int main(int argc,char**argv) {
 static uint8_t memory[0x2000000];vm_base=memory;ppu_hle_register_all();
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
 for(unsigned pass=0;pass<(is_mode("sequence")?2u:1u);++pass) {
 picture_count=(pass || !std::strncmp(mode,"intro",5) || is_mode("audio-intro"))?1806:179;
 expected_session=pass+1;test_anchor=fake_clock;
 std::snprintf((char*)memory+0x4000,512,"x-cell-fs:///dev_bdvd/%s",is_mode("unsupported-source")?"other.pam":picture_count==1806?"mag_intro.pam":argv[1]);
 CHECK(call(CREATE,0x1000,0,0,0x4000,0x4200)==0);active_desc=vm_read32(0x4200);
 CHECK(call(OPEN,active_desc)==0);CHECK(call(0x7C8DFF3B,0x1000,active_desc)==0);
 CHECK(call(0x0ABB318B,active_desc,0x4400,128,0)==0);
 CHECK(call(0x0D0C2F0C,active_desc,1,0,1,0x4400)==0); // SetEs replaced below
 if(!std::strncmp(mode,"audio",5)){
  for(unsigned i=0;i<3;++i){vm_write32(0x2500+i*4,0x2600+i*16);vm_write32(0x2600+i*16,0x200+i);vm_write32(0x2604+i*16,0x12345678);}
  CHECK(call(0x3D0D3B72,0x1900,0x2500,0xAABBCC)==0);CHECK(call(0x1139A206,0x1000,0,0x1900)==0);
  extern int32_t fixture_select_audio(uint32_t);
  vm_write32(vm_read32(0x4424)+0x58,90000);
  CHECK(fixture_select_audio(0x4400)==0);CHECK(call(0x0D0C2F0C,active_desc,0,0,1,0x4400)==0);
 }
 CHECK(call(0x34ECC1B9,0x1000,0x1111222233334444ULL)==0);events.clear();
 vm_write32(0x4500,1);
 if(is_mode("bad-command"))vm_write32(0x4504,1);
 uint32_t started=call(START,0x1000,0x4500,0xAABBCCDDEEFF0011ULL);
 if(is_mode("bad-command")){CHECK(started==0x80610708);CHECK(graphics.empty());return 0;}
 if(is_mode("duplicate-buffer") || is_mode("invalid-buffer")) {
  CHECK(started==0x80610702);CHECK(frame_frees==0);CHECK(call(STOP,0x1000,0,1)==0x80610702);return 0;
 }
 if(is_mode("decode-failure") || is_mode("checked-failure") || is_mode("unsupported-source") ||
    is_mode("partial-two") || is_mode("unmapped-tail") || is_mode("allocation-failure")){
  CHECK(started!=0);CHECK(!owned[0]&&!owned[1]&&!owned[2]&&!owned[3]);return 0;
 }
 CHECK(started==0);
 if(!std::strncmp(mode,"audio",5)){
  CHECK(call(0xD1462438,0x1900)==0x80610702);
  CHECK(call(0xF25F197D,0x1900,0x4a00)==0);CHECK(memory[0x4a00]==1 && memory[0x4a01]==(picture_count==179?2:6));
  CHECK(memory[0x4a02]==1 && memory[0x4a03]==0 && vm_read32(0x4a04)==48000);
  CHECK(call(0x7EB8D6B5,0x1900,256,0x4a40)==0x214);uint32_t pcm=vm_read32(0x4a40);CHECK(pcm && vm_read64(0x4a50)==85470);
  CHECK(call(0x4AE979DF,0x1900,85470,0x4a80)==0);CHECK(int64_t(vm_read64(0x4a80))==-50333);
  CHECK(vm_read32(pcm)==0x3f000000);if(picture_count==1806){CHECK(vm_read32(pcm+4)==0x3f800000);CHECK(vm_read32(pcm+8)==0x3f400000);}
  CHECK(call(0x7EB8D6B5,0x1900,256,0x4a40)==0x214);CHECK(vm_read32(0x4a40)==pcm && vm_read64(0x4a50)==85950);
  fake_clock+=1000000;for(unsigned k=0;k<188;++k)CHECK(call(0x7EB8D6B5,0x1900,256,0x4a40)==0x214);
  CHECK(call(0x4AE979DF,0x1900,vm_read64(0x4a50),0x4a80)==0);CHECK(int64_t(vm_read64(0x4a80))>950000);
  CHECK(call(STOP,0x1000,0x1122)==0);CHECK(call(0x7EB8D6B5,0x1900,256,0x4a40)==0 && vm_read32(0x4a40)==0);
  CHECK(call(0x85BEFFCC,0x1000,0x3344)==0);CHECK(scratch_frees==2);CHECK(call(0xD1462438,0x1900)==0);
  printf("PASS bounded real sound adapter lifecycle, scratch reuse, PCM lanes, preroll and advancing movie clock (%u checks)\n",checks);return 0;
 }
 CHECK(events.size()==3 && events[1].major==2 && events[1].arg1==0xAABBCCDDEEFF0011ULL);
 CHECK(call(0x0247C69E,0x1800,0x4600)==0x200);
 CHECK(vm_read32(0x4600)!=0 && memory[vm_read32(0x4600)]==1);
 sail_graphics_picture_capture_begin();CHECK(call(0x0247C69E,0x1800,0x4600)==0x100);
 uint64_t old=sail_graphics_picture_capture_end();CHECK(old && rsx_picture_bind_descriptor(0x6000,old)==1);
 rsx_picture_release(old);events.clear();
 if(is_mode("backpressure")){
  fake_clock+=10000000;
  for(unsigned i=0;i<8;++i)CHECK(call(0x9e0b7aa6,0x1000,0x4700)==0); // actual GetState NID below
  CHECK(allocations==1 && frame_frees==0);
  CHECK(memory[buffer_ea(0)]==1 && memory[buffer_ea(1)]==2 && memory[buffer_ea(2)]==3 && memory[buffer_ea(3)]==4);
  CHECK(call(STOP,0x1000,0x12345678)==0);
  CHECK(frame_frees==4 && decoder_closes==1);return 0;
 }
 if(is_mode("intro-stop")){
  fake_clock+=30000000;CHECK(call(0x0247C69E,0x1800,0x4600)==0x200);
  CHECK(call(STOP,0x1000,0xFEDCBA9876543210ULL)==0);
  CHECK(decoder_closes==1 && scratch_frees==1 && frame_frees==4);return 0;
 }
 if(is_mode("stop") || is_mode("uncertain-free") || is_mode("negative-free")){
  try {uint32_t rc=call(STOP,0x1000,0xFEDCBA9876543210ULL);
   if(is_mode("negative-free")){CHECK(rc==0x80610702);CHECK(call(STOP,0x1000,0)==0x80610702);return 0;}
   CHECK(rc==0);CHECK(events.size()==3 && events[1].arg1==0xFEDCBA9876543210ULL);
  }catch(const std::runtime_error&){CHECK(is_mode("uncertain-free"));CHECK(call(STOP,0x1000,0)==0x80610702);return 0;}
  CHECK(frame_frees==4 && decoder_closes==1 && scratch_frees==1);return 0;
 }
 for(unsigned ordinal=2;ordinal<=picture_count;++ordinal){
  fake_clock=test_anchor+uint64_t(ordinal-1)*1001000/30-1;
  CHECK(call(0x0247C69E,0x1800,0x4600)==0x100);
  fake_clock+=1;
  if(is_mode("delayed-final") && ordinal==picture_count)fake_clock+=250000;
  uint32_t status=call(0x0247C69E,0x1800,0x4600);
  if(((is_mode("truncated") || is_mode("intro-truncated")) && ordinal==70) || (is_mode("unknown-nonterminal") && ordinal==15) ||
     ((is_mode("drain-failure") || is_mode("intro-drain-failure")) && ordinal==picture_count)){
   CHECK(status==0);CHECK(decoder_closes==1 && scratch_frees==1);CHECK(!owned[0]&&!owned[1]&&!owned[2]&&!owned[3]);return 0;
  }
  CHECK(status==0x200);
  CHECK(memory[vm_read32(0x4600)]==uint8_t(ordinal));
  CHECK(vm_read64(0x4610)==(ordinal<picture_count?90000+uint64_t(ordinal-1)*3003:uint64_t(INT64_MIN)));
  uint32_t repeated=call(0x0247C69E,0x1800,0x4600);
  if(repeated==0){CHECK((is_mode("unknown-nonterminal") && ordinal==14) || ((is_mode("truncated") || is_mode("intro-truncated")) && ordinal==69) || ((is_mode("drain-failure") || is_mode("intro-drain-failure")) && ordinal==picture_count-1));CHECK(decoder_closes==1 && scratch_frees==1);return 0;}
  CHECK(repeated==0x100);
 }
 CHECK(rsx_picture_capture_command(0x7000,0x6000,0,0)==1);
 uint64_t captured=0;CHECK(rsx_picture_take_command(0x7000,0,0,&captured)==1);
 CHECK(rsx_picture_data(captured,0,0,3686400)[0]==1);rsx_picture_release(captured);
 CHECK(events.empty());
 if(is_mode("delayed-final")){CHECK(call(0x0247C69E,0x1800,0x4600)==0x100);CHECK(events.empty());}
 fake_clock=test_anchor+uint64_t(picture_count)*1001000/30-1+(is_mode("delayed-final")?250000:0);
 CHECK(call(0x0247C69E,0x1800,0x4600)==0x100);CHECK(events.empty());
 fake_clock+=1;CHECK(call(0x0247C69E,0x1800,0x4600)==0);
 CHECK(events.size()==2 && events[0].major==3 && events[0].arg0==7 && events[1].arg0==4);
 CHECK(decoder_closes==1 && scratch_frees==1 && frame_frees==allocations+(is_mode("partial-three")?2:3));
 CHECK(!owned[0]&&!owned[1]&&!owned[2]&&!owned[3]);
 CHECK(rsx_picture_bind_descriptor(0x6000,0)==1);CHECK(rsx_picture_live_count()==0 && rsx_picture_ready());
 CHECK(call(0x85BEFFCC,0x1000,0x87654321)==0);
 CHECK(call(0xFC839BD4,0x1000,active_desc)==0);
 events.clear();allocations=frame_frees=decoder_closes=scratch_frees=0;
 fake_clock+=1000000;
 }
 std::printf("%u timed playback checks PASS, %u pictures, true drain, final duration and close\n",checks,picture_count);return 0;
}

extern "C" int sail_audio_decode_pcm(FILE*,uint64_t,uint64_t,unsigned,SailAudioPCM*a){
 a->channels=picture_count==179?2:6;a->rate=48000;a->count=picture_count==179?290816:2897920;a->pts=85470;
 a->samples=(float*)std::malloc(size_t(a->count)*a->channels*4);if(!a->samples)return SAIL_VIDEO_LIMIT;
 for(unsigned n=0;n<a->count;++n)for(unsigned c=0;c<a->channels;++c)a->samples[n*a->channels+c]=0.5f+c*0.25f;return 0;
}
extern "C" int32_t cellPamfReaderSetStreamWithIndex(void*,uint32_t);
int32_t fixture_select_audio(uint32_t p){return cellPamfReaderSetStreamWithIndex((void*)(uintptr_t)p,1);}
extern "C" void sail_audio_pcm_release(SailAudioPCM*a){std::free(a->samples);std::memset(a,0,sizeof(*a));}
