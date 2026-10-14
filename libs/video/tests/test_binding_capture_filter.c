/* Title-independent bounded configuration and actual structural hash controls. */
#include "rsx_binding_capture_filter.h"
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
static int checks,failures;
#define CHECK(c,n) do{++checks;if(!(c)){++failures;fprintf(stderr,"FAIL %s\n",n);}}while(0)
int main(void){rsx_binding_capture_filter f[8];memset(f,0x55,sizeof f);
 CHECK(rsx_binding_capture_parse(NULL,8,f)==0,"absent retains legacy");CHECK(rsx_binding_capture_matches(f,0,0,0),"default needs no resident program");
 CHECK(rsx_binding_capture_parse("",1,f)==0,"empty retains legacy");CHECK(rsx_binding_capture_parse("*,*",2,f)==0,"wildcards retain legacy");
 CHECK(rsx_binding_capture_parse("0123456789abcdef:ffffffff,*",2,f)==1,"mixed selectors");CHECK(f[0].fp_hash==0x123456789abcdefull&&f[0].control==0xffffffffu&&!f[1].active,"full width lossless parse");
 CHECK(rsx_binding_capture_matches(f,1,0x123456789abcdefull,0xffffffffu),"exact selector match");CHECK(!rsx_binding_capture_matches(f,0,0x123456789abcdefull,0xffffffffu),"invalid resident program rejected");CHECK(!rsx_binding_capture_matches(f,1,0x123456789abcde0ull,0xffffffffu),"other program rejected");CHECK(!rsx_binding_capture_matches(f,1,0x123456789abcdefull,0xfffffffeu),"all raw control bits required");CHECK(rsx_binding_capture_matches(f+1,0,0,0),"mixed wildcard needs no program");
 const char*bad[]={"0:0","-1:0","+1:0","0x1:0","1:0x0","1:",":1","1:100000000","10000000000000000:0","1:0,","1:0,2:0","*,*","1:0junk","1:0 "," 1:0","*:",",","1"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){rsx_binding_capture_filter old[8];memcpy(old,f,sizeof f);CHECK(rsx_binding_capture_parse(bad[i],1,f)==-1,"malformed config rejected");CHECK(!memcmp(old,f,sizeof f),"invalid config changes no output entries");}
 char huge[300];memset(huge,'1',sizeof huge);huge[299]=0;CHECK(rsx_binding_capture_parse(huge,1,f)==-1,"bounded oversized config");CHECK(rsx_binding_capture_parse("*",0,f)==-1,"zero requests rejected");CHECK(rsx_binding_capture_parse("*",9,f)==-1,"oversized request count rejected");CHECK(rsx_binding_capture_parse("*,*,*,*,*,*,*,*",8,f)==0,"all eight requests supported");
 u8 a[32],b[32];rsx_test_fp_instr(a,RSX_TEST_FP_OPC(1)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END,2|RSX_TEST_FP_SWZ_IDENT);memset(a+16,0x23,16);memcpy(b,a,32);memset(b+16,0xa7,16);
 const u64 seed=1469598103934665603ull;u64 ah=rsx_fp_structural_hash(a,32,seed),bh=rsx_fp_structural_hash(b,32,seed);CHECK(ah&&ah==bh,"inline constant updates preserve actual structural identity");
 rsx_test_fp_put_word(b,RSX_TEST_FP_OPC(2)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_END);CHECK(ah!=rsx_fp_structural_hash(b,32,seed),"structural instruction changes alter identity");CHECK(rsx_fp_structural_hash(a,16,seed)==0,"truncated constant program invalid");
 printf("consumer filter: %d checks, %d failures\n",checks,failures);return failures?1:0;}
