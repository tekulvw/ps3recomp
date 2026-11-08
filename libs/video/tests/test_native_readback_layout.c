#include "../rsx_metal_backend.h"
#include <assert.h>
#include <stdint.h>
int main(void){
 uint32_t row=0,padded=0;uint64_t bytes=0;
 assert(rsx_metal_native_readback_layout(2560,720,&row,&padded,&bytes)==0);
 assert(row==10240&&padded==10240&&bytes==7372800);
 assert(rsx_metal_native_readback_layout(1280,720,&row,&padded,&bytes)==0);
 assert(row==5120&&padded==5120&&bytes==3686400);
 assert(rsx_metal_native_readback_layout(1,1,&row,&padded,&bytes)==0);
 assert(row==4&&padded==256&&bytes==4);
 assert(rsx_metal_native_readback_layout(1,32768,&row,&padded,&bytes)==0);
 assert(rsx_metal_native_readback_layout(1,32769,&row,&padded,&bytes)==-1);
 assert(rsx_metal_native_readback_layout(0,720,&row,&padded,&bytes)==-1);
 assert(rsx_metal_native_readback_layout(UINT32_MAX,UINT32_MAX,&row,&padded,&bytes)==-1);
 assert(rsx_metal_native_readback_layout(1280,0,&row,&padded,&bytes)==-1);
 assert(rsx_metal_native_readback_layout(1280,720,0,&padded,&bytes)==-1);
 return 0;
}
