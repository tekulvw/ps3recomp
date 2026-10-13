/* Replay the original retail transfer packet through the real FIFO drain. */
#include "../cellGcmSys.c"
#include <assert.h>
#include <sys/mman.h>
#include "../../../runtime/spu/spu_coherency.h"
extern uint32_t ppu_load_reserve32(ppu_context*,uint64_t);
extern uint64_t ppu_load_reserve64(ppu_context*,uint64_t);
extern int ppu_store_conditional32(ppu_context*,uint64_t,uint32_t);
extern int ppu_store_conditional64(ppu_context*,uint64_t,uint64_t);
uint8_t* vm_base; extern uint32_t ppu_vm_size;
ps3_guest_caller_fn g_ps3_guest_caller; u32 g_rsx_last_reference;
static unsigned alpha_words,blend_words,shader_words,dirty,flushes,refs,wakes;
static u32 dst_ea,src_ea,bytes;static int copying;
void spu_ch_wake(spu_context*c){(void)c;wakes++;}
int spu_raw_reg_load(uint32_t a,uint32_t*v){(void)a;(void)v;abort();}
void spu_raw_reg_store(uint32_t a,uint32_t v,unsigned n){(void)a;(void)v;(void)n;abort();}
int rsx_draw_engine_enabled(void){return 1;} int rsx_live_draw_enabled(void){return 1;}
void rsx_draw_engine_method(u32 m,u32 v){(void)v;m&=0x1ffc;if(m==0x30c)alpha_words++;if(m==0x310)blend_words++;if(m==0x8e4)shader_words++;}
void rsx_draw_engine_method_at(u32 m,u32 v,u32 ea){(void)ea;rsx_draw_engine_method(m,v);}
void rsx_live_draw_method(u32 m,u32 v){(void)m;(void)v;}
void rsx_draw_engine_flush(void){flushes++;}
void rsx_live_draw_flush(void){flushes++;}
int rsx_draw_engine_flush_checked(void){if(copying){assert(memcmp(vm_base+dst_ea,vm_base+src_ea,bytes)==0);refs++;}return 1;}
int rsx_draw_engine_checked_ready(void){return 1;}
void rsx_draw_engine_mark_guest_dirty(u32 ea,u32 len){assert(ea&&len);dirty++;}
void rsx_state_init(rsx_state*s){memset(s,0,sizeof(*s));}
int rsx_process_method(rsx_state*s,u32 m,u32 v){(void)s;if(m==0x50)g_rsx_last_reference=v;return 0;}
void rsx_raise_user_cmd(u32 a){(void)a;}
void rsx_live_draw_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
void rsx_draw_engine_set_display_buffer(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;}
int rsx_live_draw_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0;}
int rsx_live_draw_resolve_blit(u32 a,u32 b,u32 c,u32 d,u32 e,u32 f,u32 g,u32 h,u32 i,u32 j,u32 k,u32 l){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;return 0;}
void rsx_live_draw_note_resolve(u32 a,u32 b,u32 c,u32 d){(void)a;(void)b;(void)c;(void)d;}
static void raw(u32 ea,u32 v){v=ps3_bswap32(v);memcpy(vm_base+ea,&v,4);}
static u32 cursor;
static void word(u32 v){raw(0x100000+cursor,v);cursor+=4;}
static void packet(u32 sub,u32 method,u32 n,const u32*vs){word((n<<18)|(sub<<13)|method);for(u32 i=0;i<n;i++)word(vs[i]);}
static void copy_packet(u32 sd,u32 dd,u32 so,u32 doff,s32 sp,s32 dp,u32 len,u32 rows,u32 fmt){u32 dma[]={sd,dd};u32 cp[]={so,doff,(u32)sp,(u32)dp,len,rows,fmt,0};packet(1,0x184,2,dma);packet(1,0x30c,8,cp);}
static void drain(void){cellGcm_syscall_set_fifo(cursor,0);cellGcm_rsx_process_fifo();}
static void pattern(u32 ea,u32 n){for(u32 i=0;i<n;i++)vm_base[ea+i]=(u8)(i*37+(i>>8));}
int main(int argc,char**argv){
 assert(argc==2);setenv("PS3RECOMP_GCM_CHECKED_REF","1",1);
 vm_base=mmap(NULL,0x100000000ULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(vm_base!=MAP_FAILED);
 ppu_vm_size=0x4000000;ppu_hle_inject_base=0x03000000;s_config.localAddress=0xc0000000;s_config.localSize=0x10000000;
 memset(s_ea_address_table,0xff,sizeof(s_ea_address_table));s_ea_address_table[0]=1;s_ea_address_table[2]=0x20;s_ea_address_table[3]=0x21;s_gcm_syscall_mode=1;
 if(!strcmp(argv[1],"retail")){
  src_ea=0xca247900;dst_ea=0xc3fc5000;bytes=0x1ea680;pattern(src_ea,bytes);memset(vm_base+dst_ea,0xcc,bytes);copying=1;
  copy_packet(0xfeed0000,0xfeed0000,0x0a247900,0x03fc5000,0,0,bytes,1,0x101);
  u32 z=0;packet(1,0x310,1,&z);u32 ref=0xcafebabe;packet(0,0x50,1,&ref);drain();
  assert(memcmp(vm_base+dst_ea,vm_base+src_ea,bytes)==0);assert(!alpha_words&&!blend_words);assert(dirty&&flushes&&refs);
 }else if(!strcmp(argv[1],"3d")){
  u32 alpha=0x3f800000,blend=1,shader=0x1001;packet(1,0x30c,1,&alpha);packet(1,0x310,1,&blend);packet(1,0x8e4,1,&shader);u32 ordinary[8]={0};packet(1,0x30c,8,ordinary);drain();
  assert(alpha_words==2&&blend_words==2&&shader_words==1&&!dirty);
 }else if(!strcmp(argv[1],"rebind")){
  u32 handle=0x31337000;packet(1,0,1,&handle);pattern(0xc0010000,32);memset(vm_base+0xc0020000,0xcc,32);
  copy_packet(0xfeed0000,0xfeed0000,0x10000,0x20000,0,0,32,1,0x101);drain();assert(vm_base[0xc0020000]==0xcc&&!dirty);assert(alpha_words==1&&blend_words==1);
 }else if(!strcmp(argv[1],"rows")||!strcmp(argv[1],"negative")){
  int neg=!strcmp(argv[1],"negative");pattern(0xc0010000,64);memset(vm_base+0x02000100,0xcc,64);
  copy_packet(0xfeed0000,0xfeed0001,0x10000+(neg?32:0),0x200100+(neg?32:0),neg?-16:16,neg?-16:16,9,3,0x101);drain();
  for(u32 y=0;y<3;y++){assert(!memcmp(vm_base+0xc0010000+16*y,vm_base+0x02000100+16*y,9));assert(vm_base[0x02000100+16*y+9]==0xcc);}assert(dirty==3&&!alpha_words&&!blend_words);
 }else if(!strcmp(argv[1],"overlap_down")||!strcmp(argv[1],"overlap_up")){
  u8 original[96];pattern(0xc0010000,96);memcpy(original,vm_base+0xc0010000,96);int down=!strcmp(argv[1],"overlap_down");
  copy_packet(0xfeed0000,0xfeed0000,0x10000+(down?16:0),0x10000+(down?0:16),16,16,16,4,0x101);drain();assert(!memcmp(vm_base+0xc0010000+(down?0:16),original+(down?16:0),64));
 }else if(!strcmp(argv[1],"unmapped")||!strcmp(argv[1],"range")){
  u32 so=!strcmp(argv[1],"unmapped")?0x2ffff8:0x0ffffff8;u32 sd=!strcmp(argv[1],"unmapped")?0xfeed0001:0xfeed0000;
  if(sd==0xfeed0001)s_ea_address_table[3]=0xffff;memset(vm_base+0xc0020000,0xcc,32);
  copy_packet(sd,0xfeed0000,so,0x20000,16,16,16,2,0x101);drain();assert(vm_base[0xc0020000]==0xcc&&!dirty);assert(atomic_load(&s_checked_ref_failed));
 }else if(!strcmp(argv[1],"format")||!strcmp(argv[1],"notify")){
  memset(vm_base+0xc0020000,0xcc,32);copy_packet(0xfeed0000,0xfeed0000,0x10000,0x20000,0,0,32,1,!strcmp(argv[1],"format")?0x201:0x101);
  if(!strcmp(argv[1],"notify"))raw(0x100000+cursor-4,1);u32 ref=0xcafebabe;packet(0,0x50,1,&ref);drain();assert(vm_base[0xc0020000]==0xcc&&!dirty&&!alpha_words&&!blend_words);assert(atomic_load(&s_checked_ref_failed));assert(!refs);
 }else if(!strcmp(argv[1],"chunks")){
  u32 n=0x3fffff;pattern(0xc1000000,n+17);memset(vm_base+0xc2000000,0xcc,n+17);
  copy_packet(0xfeed0000,0xfeed0000,0x1000000,0x2000000,0,0,n,1,0x101);
  u32 cp[]={0x1000000+n,0x2000000+n,0,0,17,1,0x101,0};packet(1,0x30c,8,cp);drain();assert(!memcmp(vm_base+0xc1000000,vm_base+0xc2000000,n+17));assert(dirty==2&&!alpha_words&&!blend_words);
 }else if(!strcmp(argv[1],"io_pages")){
  /* IO2 end and IO3 beginning are deliberately non-contiguous EAs. */
  s_ea_address_table[3]=0x24;pattern(0xc0010000,32);
  copy_packet(0xfeed0000,0xfeed0001,0x10000,0x2ffff8,0,0,32,1,0x101);drain();assert(!memcmp(vm_base+0xc0010000,vm_base+0x020ffff8,8));assert(!memcmp(vm_base+0xc0010008,vm_base+0x02400000,24));
 }else if(!strcmp(argv[1],"unknown_dma")||!strcmp(argv[1],"budget")){
  memset(vm_base+0xc0020000,0xcc,32);int budget=!strcmp(argv[1],"budget");
  copy_packet(budget?0xfeed0000:0xfeed0002,0xfeed0000,0x10000,0x20000,0,0,budget?0x4000001:32,1,0x101);drain();
  assert(vm_base[0xc0020000]==0xcc&&!dirty&&!alpha_words&&!blend_words&&atomic_load(&s_checked_ref_failed));
 }else if(!strcmp(argv[1],"main_source")){
  s_ea_address_table[3]=0x24;pattern(0x020ffff8,8);pattern(0x02400000,24);u8 expect[32];memcpy(expect,vm_base+0x020ffff8,8);memcpy(expect+8,vm_base+0x02400000,24);
  copy_packet(0xfeed0001,0xfeed0000,0x2ffff8,0x20000,0,0,32,1,0x101);drain();assert(!memcmp(expect,vm_base+0xc0020000,32));
 }else if(!strcmp(argv[1],"zero")){
  copy_packet(0xfeed0000,0xfeed0000,0xffffffff,0xffffffff,0,0,0,1,0x101);drain();assert(!dirty&&!flushes&&!atomic_load(&s_checked_ref_failed));
 }else if(!strcmp(argv[1],"reservation")){
  /* Real native LL/SC epochs: same bytes, another word, and two granules. */
  ppu_context c={0};spu_context spe={0};
  memset(vm_base+0xc0010000,0xaa,256);memset(vm_base+0x02020000,0xaa,256);
  spe.resv_valid=1;spe.resv_ea=0x02020080;spu_lockline_lock();spu_coh_reserve(&spe,spe.resv_ea);spu_lockline_unlock();
  ppu_load_reserve64(&c,0x02020088);
  copy_packet(0xfeed0000,0xfeed0001,0x10000,0x22007f,0,0,4,1,0x101);drain();
  assert(!ppu_store_conditional64(&c,0x02020088,1));assert(!spe.resv_valid&&(spe.event_status&SPU_EVENT_LR)&&wakes==1);
  cursor=0;ppu_load_reserve32(&c,0x02020008);copy_packet(0xfeed0000,0xfeed0001,0x10000,0x220000,0,0,4,1,0x101);drain();
  assert(!ppu_store_conditional32(&c,0x02020008,1));
  cursor=0;ppu_load_reserve32(&c,0x02020008);copy_packet(0xfeed0000,0xfeed0001,0x10000,0x220080,0,0,4,1,0x101);drain();
  assert(ppu_store_conditional32(&c,0x02020008,1));spu_coh_unregister(&spe);
 }else if(!strcmp(argv[1],"bad_ea")||!strcmp(argv[1],"main_end")||!strcmp(argv[1],"page_zero")){
  int zero=!strcmp(argv[1],"page_zero");s_ea_address_table[2]=zero?0:(!strcmp(argv[1],"bad_ea")?0x1001:0x40);
  pattern(0xc0010000,32);memset(vm_base+(zero?0x10000:0x100100),0xcc,32);
  copy_packet(0xfeed0000,0xfeed0001,0x10000,0x200000+(zero?0x10000:0x100),0,0,32,1,0x101);drain();
  if(zero)assert(!memcmp(vm_base+0xc0010000,vm_base+0x10000,32)&&dirty);
  else assert(vm_base[0x100100]==0xcc&&!dirty&&atomic_load(&s_checked_ref_failed));
 }else if(!strcmp(argv[1],"incomplete")){
  pattern(0xc0010000,32);memset(vm_base+0xc0020000,0xcc,32);copy_packet(0xfeed0000,0xfeed0000,0x10000,0x20000,0,0,32,1,0x101);
  cellGcm_syscall_set_fifo(cursor-4,0);for(unsigned i=0;i<12;i++)cellGcm_rsx_process_fifo();assert(vm_base[0xc0020000]==0xcc&&!alpha_words&&!blend_words&&!dirty);assert(!atomic_load(&s_checked_ref_failed));
  src_ea=0xc0010000;dst_ea=0xc0020000;bytes=32;copying=1;u32 ref=0xcafebabe;packet(0,0x50,1,&ref);
  cellGcm_syscall_set_fifo(cursor,s_fifo_getoff);cellGcm_rsx_process_fifo();assert(!memcmp(vm_base+src_ea,vm_base+dst_ea,32)&&refs);
 }else assert(0);
 puts("memory-to-memory scenario passed");munmap(vm_base,0x100000000ULL);return 0;
}

/* Upstream renderer hooks are outside this focused FIFO fixture. */
void rsx_draw_engine_guest_read(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_guest_wrote(u32 a,u32 b,u32 c,u32 d,u32 e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void rsx_draw_engine_note_flip(u32 a) {(void)a;}
