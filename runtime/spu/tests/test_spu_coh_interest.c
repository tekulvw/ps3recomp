/* Manufactured reservation lines through the actual interest publication. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../spu_coherency.h"
#include "../spu_coh_ops.h"
#include "../spu_coh_bitmap.h"
struct spu_context;
void spu_ch_wake(struct spu_context* c) { (void)c; }
static int interest(uint32_t ea, int expected)
{
    int api=spu_coh_is_reserved(ea), inl=spu_coh_line_reserved(ea);
    if(api==expected && inl==expected) return 1;
    fprintf(stderr,"FAIL interest ea=%x expected=%d api=%d inline=%d\n",ea,expected,api,inl);
    return 0;
}
int main(int argc,char** argv)
{
    if(argc!=2 || (g_spu_coh_ops!=0)!=atoi(argv[1])) return 2;
    if(!interest(0x4000,0) || !interest(0x4080,0)) return 1;
    spu_lockline_lock();spu_coh_reserve(0,0x4000);spu_lockline_unlock();
    if(!interest(0x4000,1) || !interest(0x407F,1) || !interest(0x4080,0)) return 1;
    spu_lockline_lock();spu_coh_reserve(0,0x4080);spu_lockline_unlock();
    if(!interest(0x4000,1) || !interest(0x4080,1) || !interest(0x4100,0)) return 1;
    puts("PASS inline and out-of-line coherence interest publication");
    return 0;
}
