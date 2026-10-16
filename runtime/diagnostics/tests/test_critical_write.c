#define _POSIX_C_SOURCE 200809L
#include "critical_trace.h"
#include "critical_write_trace.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){assert(argc==2);setenv("PS3RECOMP_CRITICAL_WATCH_CONTAINER","1e82e00",1);setenv("PS3RECOMP_CRITICAL_WATCH_ARRAY","3",1);unsigned char mem[128]={0},src[128]={0};assert(!ps3_critical_begin(20,1000000000));ps3_critical_watch_bind(20,17,0x1e82e00,3,0x155ed570);assert(ps3_critical_write_watched(0x155ed500,128));assert(!ps3_critical_write_watched(0x155ed580,16));ps3_critical_write_token t={0};src[0x73]=17;ps3_critical_write_begin(&t,99,0x155ed500,128,mem,src,9,0xB4,0xA50,0xC10,0x80000001,1);memcpy(mem,src,128);ps3_critical_write_end(&t,mem);src[0x73]=33;ps3_critical_write_begin(&t,99,0x155ed500,128,mem,src,9,0xB4,0xA50,0xC10,0x80000001,1);/* failed conditional: no end/copy, therefore no publication */assert(!ps3_critical_finish(argv[1]));assert(!ps3_critical_write_watched(0x155ed500,128));return 0;}

uint32_t spurs_policy_service_identity(const void *ctx){(void)ctx;return 0;}
