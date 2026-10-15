/* Manufactured RI10 encodings exercise all byte/halfword lanes independently
 * of decoder names. Literal vectors distinguish word-immediate substitution,
 * sign extension, missing execution, register clobber, and destination alias. */
#include "../spu_interp.h"
#include "../spu_helpers.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
u128 spu_rdch(spu_context*c,uint32_t ch){(void)c;(void)ch;abort();}
void spu_wrch(spu_context*c,uint32_t ch,u128 v){(void)c;(void)ch;(void)v;abort();}
uint32_t spu_rchcnt(spu_context*c,uint32_t ch){(void)c;(void)ch;abort();}
int g_spu_ls_watch_n,g_spu_ls_probe,g_wws_read_probe,g_wws_code_probe,g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context*c,uint32_t a){(void)c;(void)a;abort();}
int spu_lifted_entry_maybe(uint32_t a) { (void)a; return 1; }
void spu_spurs_taskset_syscall(spu_context*c){(void)c;abort();}
void spu_ls_watch_slow(uint32_t l,int w,const uint8_t*p,uint32_t pc,uint32_t lr){(void)l;(void)w;(void)p;(void)pc;(void)lr;abort();}
static spu_context c;
static const uint8_t input[16]={0x00,0x80,0xFF,0x7F,0x01,0x02,0x03,0x04,0x10,0x20,0x40,0x08,0x55,0xAA,0xFE,0x81};
static void be32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void run(unsigned op,int imm,const uint8_t want[16],int alias){
 const unsigned ra=19,rt=alias?ra:33;
 const uint32_t insn=(op<<24)|(((uint32_t)imm&1023)<<14)|(ra<<7)|rt;
 memset(&c,0,sizeof c);spu_context_init(&c,0);c.image_id=-1;
 for(unsigned r=0;r<128;r++)memset(&c.gpr[r],0x5A,16);
 memcpy(c.gpr[ra]._u8,input,16);be32(c.ls+0x100,insn);be32(c.ls+0x104,0);
 spu_interp_run(&c,0x100);
 assert(c.status==SPU_STATUS_STOPPED_BY_STOP && c.pc==0x108);
 assert(!memcmp(c.gpr[rt]._u8,want,16));
 for(unsigned r=0;r<128;r++)if(r!=ra&&r!=rt)for(unsigned b=0;b<16;b++)assert(c.gpr[r]._u8[b]==0x5A);
 if(!alias)assert(!memcmp(c.gpr[ra]._u8,input,16));
 u128 src;memcpy(src._u8,input,16);
 u128 helper=op==6?spu_orbi(src,imm):spu_xorhi(src,imm);
 assert(!memcmp(helper._u8,want,16));
#ifdef RI10_LIFTED
 memcpy(c.gpr[ra]._u8,input,16);lifted_run(insn,&c);
 assert(!memcmp(c.gpr[rt]._u8,want,16));
 if(!alias)assert(!memcmp(c.gpr[ra]._u8,input,16));
#endif
}
int main(void){
 const uint8_t or4[16]={0x04,0x84,0xFF,0x7F,0x05,0x06,0x07,0x04,0x14,0x24,0x44,0x0C,0x55,0xAE,0xFE,0x85};
 const uint8_t all[16]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
 /* u128 halfwords use host-native storage, so these literals describe the
  * source byte vector XOR the low signed 16-bit immediate on this host. */
 const uint16_t x4[8]={0x8004,0x7FFB,0x0205,0x0407,0x2014,0x0844,0xAA51,0x81FA};
 const uint16_t xm1[8]={0x7FFF,0x8000,0xFDFE,0xFBFC,0xDFEF,0xF7BF,0x55AA,0x7E01};
 for(int alias=0;alias<2;alias++){
  run(6,4,or4,alias);run(6,260,or4,alias);run(6,-252,or4,alias);run(6,-1,all,alias);
  run(0x45,4,(const uint8_t*)x4,alias);run(0x45,-1,(const uint8_t*)xm1,alias);
 }
 puts("PASS: encoded ORBI/XORHI, PC advance, literal lane results, helper/lifted consistency, alias and untouched registers");
}
