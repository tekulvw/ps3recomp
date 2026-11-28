#include "prx_loader.h"
#include "sysPrxForUser.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif
static uint8_t memory[1024*1024];
uint8_t* vm_base=memory;
uint32_t ppu_vm_size=sizeof(memory);
static int fail_malloc,fail_realloc;
void* fixture_malloc(size_t size){if(fail_malloc){fail_malloc=0;return NULL;}return malloc(size);}
void* fixture_realloc(void* ptr,size_t size){if(fail_realloc){fail_realloc=0;return NULL;}return realloc(ptr,size);}
static void allocation_failures(void){
    const prx_loaded_export e={0x71,0x8000,PRX_EXPORT_FUNCTION};
    fail_realloc=1;
    assert(prx_register_loaded_library(71,"allocation",&e,1,sizeof(memory))==PRX_REG_NO_MEMORY);
    assert(prx_export_registry_count()==0 && !prx_resolve_export(e.nid));
}
static void put16(uint32_t a,uint16_t v){memory[a]=v>>8;memory[a+1]=v;}
static void put32(uint32_t a,uint32_t v){for(unsigned i=0;i<4;i++)memory[a+i]=v>>(24-i*8);}
static void descriptor(uint32_t a,const char* name){
    memset(memory+a,0,28);memory[a]=28;put16(a+2,1);put16(a+4,1);
    put16(a+6,2);put16(a+8,1);put32(a+16,a+0x40);strcpy((char*)memory+a+0x40,name);
    put32(a+20,a+0x80);put32(a+24,a+0x90);
    put32(a+0x80,0x101);put32(a+0x84,0x102);put32(a+0x88,0x101);
    put32(a+0x90,0x8000);put32(a+0x94,0x8010);put32(a+0x98,0x9001);
}
static void synthetic_batches(void){
    const uint32_t count=3500;
    prx_loaded_export* entries=calloc(count,sizeof(*entries));assert(entries);
    for(uint32_t i=0;i<count;i++)entries[i]=(prx_loaded_export){0x10000+i,0x10000+8*i,PRX_EXPORT_FUNCTION};
    assert(prx_register_loaded_library(1,"synthetic",entries,count,sizeof(memory))==PRX_REG_OK);
    assert(prx_export_registry_count()==count);
    assert(prx_resolve_export(0x10000+count-1)==0x10000+8*(count-1));
    assert(prx_register_loaded_library(1,"synthetic",entries,count,sizeof(memory))==PRX_REG_OK);
    assert(prx_export_registry_count()==count);
    fail_malloc=1;
    assert(prx_register_loaded_library(2,"allocation",entries,count,sizeof(memory))==PRX_REG_NO_MEMORY);
    assert(prx_export_registry_count()==count);
    assert(prx_register_loaded_library(2,"synthetic",entries,count,sizeof(memory))==PRX_REG_CONFLICT);
    assert(prx_export_registry_count()==count);
    entries[1].nid=entries[0].nid;
    assert(prx_register_loaded_library(2,"bad-duplicate",entries,count,sizeof(memory))==PRX_REG_INVALID);
    assert(prx_export_registry_count()==count);
    entries[1].nid=0x10001;entries[1].guest_addr=sizeof(memory)-4;
    assert(prx_register_loaded_library(2,"bad-range",entries,count,sizeof(memory))==PRX_REG_INVALID);
    assert(prx_export_registry_count()==count);
    assert(prx_register_loaded_library(2,"too-large",entries,PRX_LOADED_EXPORT_LIMIT+1,sizeof(memory))==PRX_REG_CAPACITY);
    assert(prx_export_registry_count()==count);
    assert(prx_unregister_loaded_library(1)==PRX_REG_OK);
    assert(prx_export_registry_count()==0);
    assert(prx_unregister_loaded_library(1)==PRX_REG_NOT_FOUND);
    free(entries);
}
static void capacity_and_shadowing(void){
    const uint32_t count=PRX_LOADED_EXPORT_LIMIT-4;
    prx_loaded_export* entries=calloc(count,sizeof(*entries));assert(entries);
    for(uint32_t i=0;i<count;i++)entries[i]=(prx_loaded_export){i+1,0x8000,PRX_EXPORT_FUNCTION};
    assert(prx_register_loaded_library(3,"capacity",entries,count,sizeof(memory))==PRX_REG_OK);
    assert(prx_register_loaded_library(4,"overflow",entries,8,sizeof(memory))==PRX_REG_CAPACITY);
    assert(prx_export_registry_count()==count && prx_resolve_export(count)==0x8000);
    assert(!prx_resolve_library_export("overflow",1,PRX_EXPORT_FUNCTION));
    assert(prx_unregister_loaded_library(3)==PRX_REG_OK);free(entries);
    const prx_loaded_export first={0x77,0x8000,PRX_EXPORT_FUNCTION},second={0x77,0x8010,PRX_EXPORT_FUNCTION},variable={0x78,0x9001,PRX_EXPORT_VARIABLE};
    assert(prx_register_loaded_library(5,"first",&first,1,sizeof(memory))==PRX_REG_OK);
    assert(prx_register_loaded_library(6,"second",&second,1,sizeof(memory))==PRX_REG_OK);
    assert(prx_register_loaded_library(7,"variable",&variable,1,sizeof(memory))==PRX_REG_OK);
    assert(prx_resolve_export(0x77)==0x8010 && prx_resolve_export(0x78)==0);
    assert(prx_resolve_library_export("first",0x77,PRX_EXPORT_FUNCTION)==0x8000);
    assert(prx_resolve_library_export("variable",0x78,PRX_EXPORT_VARIABLE)==0x9001);
    assert(prx_unregister_loaded_library(6)==PRX_REG_OK && prx_resolve_export(0x77)==0x8000);
    assert(prx_unregister_loaded_library(5)==PRX_REG_OK);assert(prx_unregister_loaded_library(7)==PRX_REG_OK);
}
static void legacy_module(void){
    uint8_t image[128];memset(image,0x53,sizeof(image));
    const prx_export exports[]={{0x91,64},{0x92,72}};
    const prx_module module={"legacy-fixture",0x5000,image,sizeof(image),NULL,0,exports,2};
    assert(prx_load_module(&module,NULL).ok);
    assert(!memcmp(memory+0x5000,image,sizeof(image)));
    assert(prx_resolve_export(0x91)==0x5040 && prx_resolve_export(0x92)==0x5048);
    assert(prx_unregister_loaded_library(UINT64_C(0x8000000000005000))==PRX_REG_OK);
}
static void guest_descriptors(void){
    descriptor(0x1000,"fixture-library");
    assert(sys_prx_register_library((void*)(uintptr_t)0x1000)==0);
    assert(prx_resolve_export(0x101)==0x8000);
    assert(prx_resolve_library_export("fixture-library",0x101,PRX_EXPORT_FUNCTION)==0x8000);
    assert(prx_resolve_library_export("fixture-library",0x101,PRX_EXPORT_VARIABLE)==0x9001);
    assert(prx_resolve_library_export("fixture-library",0x102,PRX_EXPORT_VARIABLE)==0);
    assert(prx_export_registry_count()==3);
    assert(sys_prx_register_library((void*)(uintptr_t)0x1000)==0);
    put32(0x1084,0x101);
    assert(sys_prx_register_library((void*)(uintptr_t)0x1000)!=0);
    assert(prx_export_registry_count()==3 && prx_resolve_export(0x102)==0x8010);
    put32(0x1084,0x102);
    descriptor(0x2000,"bad-library");put16(0x2000+10,1);
    assert(sys_prx_register_library((void*)(uintptr_t)0x2000)!=0);
    assert(prx_export_registry_count()==3);
    descriptor(0x2000,"bad-library");put32(0x2000+24,sizeof(memory)-2);
    assert(sys_prx_register_library((void*)(uintptr_t)0x2000)!=0);
    assert(prx_export_registry_count()==3);
    descriptor(0x2000,"bad-library");memory[0x2000]=29;
    assert(sys_prx_register_library((void*)(uintptr_t)0x2000)!=0);
    descriptor(0x2000,"bad-library");memset(memory+0x2040,'x',PRX_LOADED_LIBRARY_NAME_MAX);
    assert(sys_prx_register_library((void*)(uintptr_t)0x2000)!=0);
    descriptor(0x2000,"bad-library");put16(0x2006,65535);put16(0x2008,65535);
    assert(sys_prx_register_library((void*)(uintptr_t)0x2000)!=0);
    assert(sys_prx_register_library((void*)(uintptr_t)0x100000100ull)!=0);
    assert(sys_prx_register_library((void*)(uintptr_t)(sizeof(memory)-2))!=0);
    assert(sys_prx_unregister_library((void*)(uintptr_t)0x1000)==0);
    assert(!prx_resolve_export(0x101));assert(prx_export_registry_count()==0);
}
#ifndef _WIN32
static void* writer(void* arg){
    uint64_t owner=(uintptr_t)arg;
    const prx_loaded_export e={0x60000+(uint32_t)owner,0x8000,PRX_EXPORT_FUNCTION};
    char name[32];snprintf(name,sizeof name,"thread-%u",(unsigned)owner);
    for(unsigned i=0;i<1000;i++){
        assert(prx_register_loaded_library(owner,name,&e,1,sizeof(memory))==PRX_REG_OK);
        assert(prx_resolve_export(e.nid)==e.guest_addr);
        assert(prx_unregister_loaded_library(owner)==PRX_REG_OK);
    }return 0;
}
static void concurrent_batches(void){
    pthread_t threads[4];for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],0,writer,(void*)(i+10)));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],0));
    assert(prx_export_registry_count()==0);
}
#endif
int main(void){allocation_failures();synthetic_batches();capacity_and_shadowing();legacy_module();guest_descriptors();
#ifndef _WIN32
    concurrent_batches();
#endif
    return 0;
}
