/* Synthetic content IDs and files only; no signed content or keys. */
#include "../libs/network/sceNpDrm.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
uint8_t memory[256];
uint8_t* vm_base = memory;
uint32_t ppu_vm_size = sizeof memory;
static const char* root;
static unsigned lookups;
static int mapping_error;
int cellfs_translate_path(const char* guest, char* host, size_t cap)
{
    assert(strncmp(guest,"/dev_hdd0/",10)==0); lookups++;
    if (mapping_error) return -1;
    int n=snprintf(host,cap,"%s/%s",root,guest+10);
    return n<0 || (size_t)n>=cap ? -1 : 0;
}
static unsigned registered;
void ps3_hle_register(uint32_t nid,const char* name,void* handler)
{
    if (nid==0xBE0E3EE2u) assert(handler==(void*)sceNpDrmVerifyUpgradeLicense2);
    else { assert(nid==0x2ECD48EDu); assert(handler==(void*)sceNpDrmVerifyUpgradeLicense); }
    assert(strstr(name,"sceNpDrmVerifyUpgradeLicense")); registered++;
}
extern void ppu_hle_register_all(void);
static s32 query(const char* id)
{
    memset(memory,0,sizeof memory); strcpy((char*)memory+16,id);
    s32 a=sceNpDrmVerifyUpgradeLicense((void*)(uintptr_t)16);
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)16)==a); return a;
}
int main(int argc,char** argv)
{
    assert(argc==3);root=argv[1]; int mode=atoi(argv[2]);
    ppu_hle_register_all();assert(registered==2);
    assert(sceNpDrmVerifyUpgradeLicense2(NULL)==SCE_NP_DRM_ERROR_INVALID_PARAM);
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)256)==SCE_NP_DRM_ERROR_INVALID_PARAM);
#if UINTPTR_MAX > UINT32_MAX
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)0x100000010ull)==SCE_NP_DRM_ERROR_INVALID_PARAM);
#endif
    memset(memory+250,'A',6);
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)250)==SCE_NP_DRM_ERROR_INVALID_PARAM);
    memset(memory+16,'A',48);
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)16)==SCE_NP_DRM_ERROR_INVALID_PARAM);
    memory[255]=0;
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)255)==SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    vm_base=NULL;
    assert(sceNpDrmVerifyUpgradeLicense2((void*)(uintptr_t)16)==SCE_NP_DRM_ERROR_INVALID_PARAM);
    vm_base=memory;
    char longest[48];memset(longest,'A',47);longest[47]=0;
    assert(query(longest)==(mode==2 ? SCE_NP_DRM_ERROR_IO : SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND));
    ppu_vm_size=0;
    assert(query("ANOTHER_SAFE_ID")== (mode==2 ? SCE_NP_DRM_ERROR_IO : SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND));
    ppu_vm_size=sizeof memory;
    unsigned before=lookups;
    assert(query("../escape")==SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    assert(query("")==SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND);
    assert(lookups==before);
    s32 expected=mode==1 ? SCE_NP_DRM_ERROR_INTERNAL : mode==2 ? SCE_NP_DRM_ERROR_IO : SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND;
    assert(query("UP0000-TEST00000_00-SYNTHETIC0000000")==expected);
    mapping_error=1;assert(query("UP0000-TEST00000_00-SYNTHETIC0000000")==SCE_NP_DRM_ERROR_IO);
    puts("NP DRM upgrade fixture passed");return 0;
}
