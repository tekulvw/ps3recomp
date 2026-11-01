/* Synthetic registration only: no guest function is ever executed. */
#include "spu_context.h"
#include "spu_overlay_residency.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
struct entry { uint64_t addr; void* func; const char* name; };
const struct entry function_table[1]={{0,0,0}};
const uint64_t function_table_count=0;
extern void spu_begin_image(int image);
extern void spu_register_function(uint32_t addr,void(*fn)(spu_context*));
extern void (*spu_lookup(uint32_t addr,int image))(spu_context*);
static void first(spu_context*c){(void)c;abort();}
static void second(spu_context*c){(void)c;abort();}
int main(int argc,char**argv){
    assert(argc==2);const char*mode=argv[1];
    const uint8_t bytes[4]={1,2,3,4};
    const spu_overlay_code_segment segment={0x4030,4,bytes};
    spu_begin_image(201);
    spu_register_function(0x4030,first);
    assert(spu_lookup(0x4030,201)==first);
    if(!strcmp(mode,"basic")){
        spu_register_function(0x4040,second);
        assert(spu_overlay_register_verified_region(0x1000,0x4000,0x100,201,&segment,1));
    }else if(!strcmp(mode,"duplicate")){
        spu_register_function(0x4030,second);
        assert(spu_lookup(0x4030,201)==first);
        assert(spu_overlay_register_verified_region(0x1000,0x4000,0x100,201,&segment,1));
        assert(spu_overlay_register_verified_region(0x1000,0x4000,0x100,201,&segment,1));
    }else if(!strcmp(mode,"reject")){
        assert(!spu_overlay_register_verified_region(0x1000,0x4000,0,201,&segment,1));
        const spu_overlay_code_segment bad={0x3FFC,4,bytes};
        assert(!spu_overlay_register_verified_region(0x1000,0x4000,0x100,201,&bad,1));
    }else if(!strcmp(mode,"function-capacity")){
        for(unsigned i=1;i<262144;i++)spu_register_function(0x4030,first);
        spu_register_function(0x4040,second);
        assert(!spu_lookup(0x4040,201));
        assert(spu_overlay_register_verified_region(0x1000,0x4000,0x100,201,&segment,1));
    }else if(!strcmp(mode,"region-capacity")){
        for(unsigned i=0;i<128;i++)assert(spu_overlay_register_verified_region(0x1000+i*0x100,0x4000,0x100,201,&segment,1));
        assert(!spu_overlay_register_verified_region(0x10000,0x4000,0x100,201,&segment,1));
    }else if(!strcmp(mode,"limit")){
        for(int i=0;i<600;i++){spu_begin_image(300+i);spu_register_function(0x4030,first);}
    }else if(!strcmp(mode,"mismatch")){
        assert(spu_overlay_register_verified_region(0x1000,0x4000,0x100,202,&segment,1));
    }else if(!strcmp(mode,"null")){
        spu_register_function(0x4040,NULL);
        assert(!spu_lookup(0x4040,201));
    }else if(!strcmp(mode,"many-images")){
        for(int i=0;i<30;i++){
            const int image=1000+i;
            spu_begin_image(image);
            spu_register_function(0x4030,first);spu_register_function(0x4040,second);
            assert(spu_lookup(0x4030,image)==first&&spu_lookup(0x4040,image)==second);
            assert(spu_overlay_register_verified_region(0x1000+i*0x100,0x4000,0x100,image,&segment,1));
        }
    }else abort();
    spu_begin_image(0);
    return 0;
}
