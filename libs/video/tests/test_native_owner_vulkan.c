/* Actual public SDK owner lifecycle and CPU ingress; no game/window. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
static VkPhysicalDeviceFeatures created_features;
static VkResult capture_create_device(VkPhysicalDevice,const VkDeviceCreateInfo *,const VkAllocationCallbacks *,VkDevice *);
#define PS3RECOMP_VK_TEST_CREATE_DEVICE capture_create_device
#include "../rsx_vulkan_backend.c"
static VkResult capture_create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo *info,const VkAllocationCallbacks *allocator,VkDevice *device){
    created_features=*info->pEnabledFeatures;
    return pvkCreateDevice(physical,info,allocator,device);
}
#include <assert.h>
u8* vm_base;u32 ppu_vm_size,ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;(void)offset;assert(!"fixture has no guest memory");return 0;}
u32 cellGcmResolveIO(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t a,uint32_t v){(void)a;(void)v;assert(!"fixture cannot publish guest memory");}
void ps3_ms(const char* key){(void)key;assert(!"fixture issues no guest draws/flips");}
unsigned long long ps3_ms_now(void){assert(!"fixture has no guest notification clock");return 0;}
void cellGcm_replay_display_buffers_to_engine(void){}
/* The legacy SDK compute implementation is deliberately absent here. */
rsx_frontend_device* rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner* owner){(void)owner;assert(!"legacy frontend must not be created");return NULL;}
void rsx_frontend_close(rsx_frontend_device* device){assert(!device);}
static void require_validation(void){
    void* library=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);assert(library);
    PFN_vkEnumerateInstanceLayerProperties enumerate=(PFN_vkEnumerateInstanceLayerProperties)dlsym(library,"vkEnumerateInstanceLayerProperties");assert(enumerate);
    uint32_t count=0;assert(enumerate(&count,NULL)==VK_SUCCESS && count && count<=256);
    VkLayerProperties* layers=calloc(count,sizeof(*layers));assert(layers);assert(enumerate(&count,layers)==VK_SUCCESS);
    int found=0;for(uint32_t i=0;i<count;++i)found|=!strcmp(layers[i].layerName,"VK_LAYER_KHRONOS_validation");
    free(layers);dlclose(library);assert(found);
}
int main(int argc,char** argv){
    if(argc!=4){fprintf(stderr,"usage: owner_fixture REQUIRE_HARDWARE VENDOR_ID DEVICE_ID\n");return 2;}
    int hardware=atoi(argv[1]);uint32_t vendor=(uint32_t)strtoul(argv[2],NULL,0),device=(uint32_t)strtoul(argv[3],NULL,0);
    setenv("PS3RECOMP_RSX_NATIVE_OWNER","1",1);setenv("PS3RECOMP_RSX_ENGINE","dispatch",1);
    setenv("PS3RECOMP_VK_WINDOW","0",1);setenv("PS3RECOMP_VK_GUEST_PROGRAMS","0",1);
    require_validation();setenv("VK_INSTANCE_LAYERS","VK_LAYER_KHRONOS_validation",1);
    assert(rsx_vulkan_backend_init(8,8,"native owner synthetic")==0 && s_vk.eng_active);
    VkPhysicalDeviceProperties properties;pvkGetPhysicalDeviceProperties(s_vk.phys,&properties);
    if(hardware){assert(vendor && device && properties.vendorID==vendor && properties.deviceID==device);
        assert(properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU||properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);}
    rsx_native_owner first={.abi_version=1,.struct_size=sizeof(first)};assert(rsx_vulkan_backend_borrow_owner(&first)==0);
    assert(first.device==(void*)s_vk.device && first.queue==(void*)s_vk.queue);
    rsx_native_owner_v2 extended={.abi_version=2,.struct_size=sizeof(extended)};
    assert(rsx_vulkan_backend_borrow_owner_v2(&extended)==0);
    assert(extended.base.lifetime==first.lifetime && extended.base.device==first.device);
    assert(extended.known_features==(RSX_NATIVE_OWNER_FEATURE_BC|RSX_NATIVE_OWNER_FEATURE_ANISOTROPY|RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS));
    const uint64_t expected_features=(created_features.textureCompressionBC?RSX_NATIVE_OWNER_FEATURE_BC:0)|
        (created_features.samplerAnisotropy?RSX_NATIVE_OWNER_FEATURE_ANISOTROPY:0)|
        (created_features.depthBounds?RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS:0);
    assert(extended.enabled_features==expected_features);
    assert(!rsx_vulkan_backend_frontend());assert(first.retain(first.userdata,first.lifetime)==0);
    assert(first.try_begin(first.userdata,first.lifetime)==-4);
    rsx_draw_engine_flush();assert(first.try_begin(first.userdata,first.lifetime)==0);first.end(first.userdata,first.lifetime);
    rsx_draw_engine_method_at(NV4097_SET_BEGIN_END,5,0);
    assert(first.try_begin(first.userdata,first.lifetime)==-4);
    s_vk_engine_backend.submit_and_wait(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);
    assert(first.try_begin(first.userdata,first.lifetime)==-4); /* direct backend flush cannot bypass CPU BEGIN */
    rsx_draw_engine_method(NV4097_SET_BEGIN_END,0);rsx_draw_engine_flush();
    assert(first.try_begin(first.userdata,first.lifetime)==0);first.end(first.userdata,first.lifetime);
    void* original=first.device;rsx_vulkan_backend_shutdown();assert((void*)s_vk.device==original);
    assert(rsx_vulkan_backend_owner_shutdown_requested(first.lifetime)==1);
    assert(rsx_vulkan_backend_owner_retry_shutdown(first.lifetime)==-4);
    assert(rsx_vulkan_backend_init(8,8,"refused replacement")==-1 && (void*)s_vk.device==original);
    first.release(first.userdata,first.lifetime);assert((void*)s_vk.device==original);
    assert(rsx_vulkan_backend_owner_retry_shutdown(first.lifetime)==1 && !s_vk.device);
    assert(rsx_vulkan_backend_init(8,8,"new owner lifetime")==0);
    rsx_native_owner next={.abi_version=1,.struct_size=sizeof(next)};assert(rsx_vulkan_backend_borrow_owner(&next)==0 && next.lifetime!=first.lifetime);
    assert(first.retain(first.userdata,first.lifetime)==-3);
    assert(next.retain(next.userdata,next.lifetime)==0);next.release(next.userdata,next.lifetime);
    assert(rsx_vulkan_backend_owner_shutdown_requested(next.lifetime)==0);
    assert(rsx_vulkan_backend_owner_retry_shutdown(next.lifetime)==0 && s_vk.device); /* ordinary drop never tears down */
    rsx_vulkan_backend_shutdown();assert(!s_vk.device);
    printf("PASS Vulkan actual public owner/BEGIN/retention/retry/reinit hardware=%d GPU=%s vendor=0x%x device=0x%x\n",hardware,properties.deviceName,properties.vendorID,properties.deviceID);
    return 0;
}
