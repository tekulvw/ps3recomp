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
#include <pthread.h>
#include <time.h>
#include <unistd.h>
u8* vm_base;u32 ppu_vm_size,ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;(void)offset;assert(!"fixture has no guest memory");return 0;}
u32 cellGcmResolveIO(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t a,uint32_t v){(void)a;(void)v;assert(!"fixture cannot publish guest memory");}
void ps3_ms(const char* key){(void)key;assert(!"fixture issues no guest draws/flips");}
unsigned long long ps3_ms_now(void){assert(!"fixture has no guest notification clock");return 0;}
void cellGcm_replay_display_buffers_to_engine(void){}
u32 cellGcm_flip_request_count(void){return 0;}
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

/* A stand-in native worker: after a delay, clears the image green on the
 * shared queue through the reservation's submit(). */
typedef struct { rsx_native_queue_order order; uint64_t token; VkImage image; VkCommandPool pool; VkCommandBuffer cb; VkFence fence; int status, abandon; } native_job;
static void* native_worker(void* arg){
    native_job* j=(native_job*)arg;
    struct timespec d={0,150*1000*1000};nanosleep(&d,NULL);
    if(j->abandon){j->status=j->order.submit(j->order.userdata,j->order.lifetime,j->token,NULL,NULL);return NULL;}
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    assert(pvkBeginCommandBuffer(j->cb,&bi)==VK_SUCCESS);
    vk_cmd_after_everything(j->cb);
    VkClearColorValue green={.float32={0,1,0,1}};VkImageSubresourceRange r={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    pvkCmdClearColorImage(j->cb,j->image,VK_IMAGE_LAYOUT_GENERAL,&green,1,&r);
    vk_cmd_after_everything(j->cb);
    assert(pvkEndCommandBuffer(j->cb)==VK_SUCCESS);
    VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&j->cb};
    j->status=j->order.submit(j->order.userdata,j->order.lifetime,j->token,&si,(void*)j->fence);
    return NULL;
}
static double ms_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
/* Original work recorded after a reservation runs after the native batch that
 * fills it, though that batch is submitted later from another thread; an
 * abandoned reservation releases the same way. */
static void check_queue_order(rsx_native_owner* first){
    const rsx_draw_backend* be=&s_vk_engine_backend;
    native_job j={.order={.abi_version=RSX_NATIVE_QUEUE_ORDER_ABI,.struct_size=sizeof(j.order)}};
    assert(rsx_vulkan_backend_queue_order(&j.order)==0&&j.order.lifetime==first->lifetime);
    uint64_t token=0;assert(j.order.reserve(j.order.userdata,j.order.lifetime,&token)==-4&&!token); /* lease required */
    uint32_t image=be->color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,16,16,NULL,0);assert(image);
    rsx_draw_engine_flush();
    j.image=s_eobj[image].im.img;
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=s_vk.queue_family};
    assert(pvkCreateCommandPool(s_vk.device,&pci,NULL,&j.pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=j.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    assert(pvkAllocateCommandBuffers(s_vk.device,&cai,&j.cb)==VK_SUCCESS);
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};assert(pvkCreateFence(s_vk.device,&fci,NULL,&j.fence)==VK_SUCCESS);
    assert(first->try_begin(first->userdata,first->lifetime)==0);
    assert(j.order.reserve(j.order.userdata,j.order.lifetime,&j.token)==0&&j.token);
    assert(j.order.reserve(j.order.userdata,j.order.lifetime,&token)==-4);                   /* one at a time */
    first->end(first->userdata,first->lifetime);
    assert(first->try_begin(first->userdata,first->lifetime)==-4);                          /* nothing overtakes */
    pthread_t worker;assert(pthread_create(&worker,NULL,native_worker,&j)==0);
    const double t0=ms_now();
    const float red[4]={1,0,0,1};be->clear_color(NULL,image,red);                       /* recorded after the reservation */
    uint32_t pixel=0;be->readback(NULL,image,8,8,1,1,&pixel,4);
    const double waited=ms_now()-t0;
    pthread_join(worker,NULL);
    assert(j.status==VK_SUCCESS&&pvkWaitForFences(s_vk.device,1,&j.fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
    printf("reserved order: original clear after a native batch submitted %.0f ms later reads %08x (bytes R,G,B,A = ff,00,00,ff: the original red, after the native green)\n",waited,pixel);
    assert(pixel==0xff0000ffu);                         /* R,G,B,A bytes: red, so the native green ran first */
    assert(waited>=100.0);                              /* the readback waited for the native submission */
    assert(j.order.submit(j.order.userdata,j.order.lifetime,j.token,NULL,NULL)==-3);    /* consumed */
    /* Abandoned from the worker: the held clear reaches the queue at release. */
    assert(first->try_begin(first->userdata,first->lifetime)==0);
    assert(j.order.reserve(j.order.userdata,j.order.lifetime,&j.token)==0&&j.token);
    first->end(first->userdata,first->lifetime);
    j.abandon=1;assert(pthread_create(&worker,NULL,native_worker,&j)==0);
    const float blue[4]={0,0,1,1};be->clear_color(NULL,image,blue);
    be->readback(NULL,image,8,8,1,1,&pixel,4);
    pthread_join(worker,NULL);
    assert(j.status==VK_SUCCESS&&pixel==0xffff0000u);
    rsx_draw_engine_flush();assert(first->try_begin(first->userdata,first->lifetime)==0);first->end(first->userdata,first->lifetime);
    pvkDestroyFence(s_vk.device,j.fence,NULL);pvkDestroyCommandPool(s_vk.device,j.pool,NULL);
    be->color_target_release(NULL,image);rsx_draw_engine_flush();
}
/* PS3RECOMP_VK_PIN_RELEASE_NOWAIT=1: the original release of a natively
 * pinned colour target returns without waiting for the GPU or presentation;
 * the image survives until both its last native pin and the GPU work recorded
 * before the release are done; bounded parking falls back to waiting. Use
 * after release (destroying an image a submission still uses) is caught by
 * the required validation layer. */
static void check_pin_release_nowait(rsx_native_owner* owner){
    if(!vk_pin_release_nowait())return;
    const rsx_draw_backend* be=&s_vk_engine_backend;
    if(!s_bat_on){printf("pin release without waiting: not batched, release waits as before\n");return;}
    /* Keep the GPU busy so that work recorded before the release is pending. */
    uint32_t busy=be->depth_target_create(NULL,4096,4096);assert(busy);
    uint32_t busy_view=be->depth_z24_view(NULL,busy,4096,4096,0xAAE4u,0xA5u);assert(busy_view);
    uint32_t color=be->color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,64,64,NULL,0);assert(color);
    rsx_native_image image={.abi_version=RSX_NATIVE_IMAGE_ABI,.struct_size=sizeof(image)};
    assert(rsx_vulkan_backend_borrow_image(owner->lifetime,color,&image)==0);
    assert(image.retain(image.userdata,image.owner_lifetime,image.resource_id,image.resource_generation)==0);
    const float green[4]={0,1,0,1};be->clear_color(NULL,color,green);          /* recorded, still in the batch */
    const unsigned long long nowait=s_pin_nowait,fallback=s_pin_fallback;
    const double t0=ms_now();be->color_target_release(NULL,color);const double released=ms_now()-t0;
    assert(s_pin_nowait==nowait+1&&s_pin_fallback==fallback);
    assert(s_eobj_retired[color]&&s_eobj_pin_ticket[color]);
    const u64 ticket=s_eobj_pin_ticket[color];
    /* The last native pin: destroyed now only if the release's ticket is done. */
    const unsigned long long parked=s_pin_deferred,immediate=s_pin_immediate;
    image.release(image.userdata,image.owner_lifetime,image.resource_id,image.resource_generation);
    assert(s_pin_deferred+s_pin_immediate==parked+immediate+1);
    assert(!s_eobj[color].im.img&&!s_eobj_retired[color]&&!s_eobj_pin_ticket[color]);
    assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
    assert(s_e5.completed>=ticket&&s_pin_retire_n==0);                        /* reaped once complete */
    printf("pinned release without waiting: %.2f ms, then %s at the last pin\n",released,
           s_pin_deferred>parked?"parked until its ticket completed":"destroyed at once");
    /* Bounds: more pinned releases than parking room. */
    enum{N=VK_PIN_RETIRE+8};rsx_native_image held[N];uint32_t handles[N];
    const unsigned long long fallback0=s_pin_fallback;
    for(int i=0;i<N;i++){
        handles[i]=be->color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,16,16,NULL,0);assert(handles[i]);
        held[i]=(rsx_native_image){.abi_version=RSX_NATIVE_IMAGE_ABI,.struct_size=sizeof(held[i])};
        assert(rsx_vulkan_backend_borrow_image(owner->lifetime,handles[i],&held[i])==0);
        assert(held[i].retain(held[i].userdata,held[i].owner_lifetime,held[i].resource_id,held[i].resource_generation)==0);
        be->clear_color(NULL,handles[i],green);be->color_target_release(NULL,handles[i]);
        assert(s_pin_retire_n+s_pin_retire_reserved<=VK_PIN_RETIRE);
    }
    assert(s_pin_fallback==fallback0+(N-VK_PIN_RETIRE));                      /* the rest waited */
    for(int i=0;i<N;i++)held[i].release(held[i].userdata,held[i].owner_lifetime,held[i].resource_id,held[i].resource_generation);
    assert(s_pin_retire_reserved==0);
    assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1&&s_pin_retire_n==0);
    be->texture_release(NULL,busy_view);be->depth_target_release(NULL,busy);
    printf("pin release bounds: %d releases, %llu waited when the parking was full, all destroyed\n",N,s_pin_fallback-fallback0);
}
/* The offscreen target's centre pixel, bytes R,G,B,A. */
static uint32_t offscreen_center(void){
    vk_buffer st={0};assert(!vk_create_host_buffer(&st,4,VK_BUFFER_USAGE_TRANSFER_DST_BIT));
    uint32_t pixel=0;assert(vulkan_owner_begin(1));const uint64_t before=s_vulkan_commits;   /* an original read, scoped */
    assert(!vk_begin());
    vk_barrier_image(s_vk.color.img,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageOffset={(int32_t)(s_vk.width/2),(int32_t)(s_vk.height/2),0},.imageExtent={1,1,1}};
    pvkCmdCopyImageToBuffer(s_vk.cmd,s_vk.color.img,VK_IMAGE_LAYOUT_GENERAL,st.buf,1,&region);
    VkBufferMemoryBarrier hb={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=st.buf,.offset=0,.size=VK_WHOLE_SIZE};
    pvkCmdPipelineBarrier(s_vk.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&hb,0,NULL);
    assert(!vk_sync());vulkan_owner_finish(vulkan_owner_boundary(before));
    vk_invalidate(&st);memcpy(&pixel,st.ptr,4);vk_destroy_buffer(&st);
    return pixel;
}
/* PS3RECOMP_VK_DEFER_HELD_PRESENT=1: a present made while a reservation holds
 * original submissions returns without waiting for it; its capture is queued
 * behind the held work (so it shows the frame after the native batch), the
 * show stays pending while the hold lasts and is made by the pump after it,
 * and a second capture during the hold supersedes the first. Off: presents
 * never touch the deferred state. */
static void check_deferred_present(rsx_native_owner* first){
    const rsx_draw_backend* be=&s_vk_engine_backend;const int on=vk_defer_held_present();
    native_job j={.order={.abi_version=RSX_NATIVE_QUEUE_ORDER_ABI,.struct_size=sizeof(j.order)}};
    assert(rsx_vulkan_backend_queue_order(&j.order)==0);
    uint32_t image=be->color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,s_vk.width,s_vk.height,NULL,0);assert(image);
    const float red[4]={1,0,0,1};be->clear_color(NULL,image,red);
    assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);rsx_draw_engine_flush();   /* a settled boundary */
    j.image=s_eobj[image].im.img;
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=s_vk.queue_family};
    assert(pvkCreateCommandPool(s_vk.device,&pci,NULL,&j.pool)==VK_SUCCESS);
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=j.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    assert(pvkAllocateCommandBuffers(s_vk.device,&cai,&j.cb)==VK_SUCCESS);
    VkFenceCreateInfo fci={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};assert(pvkCreateFence(s_vk.device,&fci,NULL,&j.fence)==VK_SUCCESS);
    const unsigned long long deferred=s_show.deferred,served=s_show.served,superseded=s_show.superseded;
    assert(first->try_begin(first->userdata,first->lifetime)==0);
    assert(j.order.reserve(j.order.userdata,j.order.lifetime,&j.token)==0&&j.token);
    first->end(first->userdata,first->lifetime);
    pthread_t worker;assert(pthread_create(&worker,NULL,native_worker,&j)==0);   /* green, 150 ms later */
    const double t0=ms_now();be->present(NULL,image);const double presented=ms_now()-t0;
    assert(presented<100.0);                                       /* never waited for the native batch */
    pthread_mutex_lock(&s_vk.queue_mu);const int held=vk_hold_has_locked(s_vk.present_fence);pthread_mutex_unlock(&s_vk.queue_mu);
    assert(held);                                                  /* the capture is queued behind the hold */
    if(on){
        assert(s_show.pending&&s_show.deferred==deferred+1&&s_show.served==served);
        vk_show_pump();assert(s_show.pending&&s_show.served==served);           /* still held: not shown */
        be->present(NULL,image);                                                /* a newer frame during the hold */
        assert(s_show.pending&&s_show.deferred==deferred+2&&s_show.superseded==superseded+1);
    }else{
        assert(!s_show.pending&&!s_show.deferred&&!s_show.served&&!s_show.superseded);
    }
    pthread_join(worker,NULL);
    assert(j.status==VK_SUCCESS&&pvkWaitForFences(s_vk.device,1,&j.fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
    if(on){vk_show_pump();assert(!s_show.pending&&s_show.served==served+1);}  /* shown once the hold ended */
    else{vk_show_pump();assert(!s_show.pending&&!s_show.served);}
    vk_fence_wait(s_vk.present_fence);
    const uint32_t pixel=offscreen_center();
    fflush(stdout);printf("deferred held present (%s): present returned in %.1f ms during a 150 ms hold; offscreen target %08x after the native green (R,G,B,A = 00,ff,00,ff); deferred=%llu served=%llu superseded=%llu\n",
           on?"on":"off",presented,pixel,s_show.deferred,s_show.served,s_show.superseded);fflush(stdout);
    assert(pixel==0xff00ff00u);                                    /* captured after the native batch */
    /* A read-back present (a frame dump), split as the host splits it: with the
     * option its completion -- the dump -- waits for the pump after the hold
     * instead of blocking present_finish; without it, present_finish waits. */
    {
        char dump[256];snprintf(dump,sizeof dump,"%s/owner-fixture-%d.ppm",getenv("TMPDIR")?getenv("TMPDIR"):"/tmp",(int)getpid());
        const float blue[4]={0,0,1,1};be->clear_color(NULL,image,blue);
        assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);rsx_draw_engine_flush();
        assert(first->try_begin(first->userdata,first->lifetime)==0);
        assert(j.order.reserve(j.order.userdata,j.order.lifetime,&j.token)==0&&j.token);
        first->end(first->userdata,first->lifetime);
        assert(pvkAllocateCommandBuffers(s_vk.device,&cai,&j.cb)==VK_SUCCESS&&pvkResetFences(s_vk.device,1,&j.fence)==VK_SUCCESS);  /* fresh job */
        assert(pthread_create(&worker,NULL,native_worker,&j)==0);                 /* green again, 150 ms later */
        s_vk.dump_path=dump;s_vk.dump_every=1;
        const unsigned long long readbacks=s_vk.present_readback_count,served2=s_show.served;
        const double t1=ms_now();
        s_vk.present_split=1;be->present(NULL,image);s_vk.present_split=0;      /* the host's present_begin */
        vulkan_original_present_finish();
        const double finished=ms_now()-t1;
        if(on){
            assert(finished<100.0&&s_show.readback_pending&&s_vk.present_inflight&&s_vk.present_readback_count==readbacks);
            vk_show_pump();assert(s_show.readback_pending&&s_vk.present_readback_count==readbacks);   /* still held */
        }else{
            assert(finished>=100.0&&!s_show.readback_pending&&s_vk.present_readback_count==readbacks+1); /* waited */
        }
        pthread_join(worker,NULL);
        assert(j.status==VK_SUCCESS&&pvkWaitForFences(s_vk.device,1,&j.fence,VK_TRUE,UINT64_MAX)==VK_SUCCESS);
        vk_show_pump();
        assert(!s_show.readback_pending&&!s_vk.present_inflight&&s_vk.present_readback_count==readbacks+1);
        assert(s_show.served==served2+(on?1:0));
        const uint32_t center=rsx_vulkan_backend_presented_center();
        printf("deferred read-back present (%s): present_finish returned in %.1f ms; dumped centre %08x (0xAARRGGBB: the native green)\n",on?"on":"off",finished,center);fflush(stdout);
        assert(center==0xff00ff00u);
        FILE* f=fopen(dump,"rb");assert(f);fclose(f);remove(dump);
        s_vk.dump_path=NULL;s_vk.dump_every=0;
    }
    /* An ordinary present (no hold) with nothing pending changes nothing. */
    {const unsigned long long before=s_show.served,deferred_before=s_show.deferred;
     be->present(NULL,image);assert(!s_show.pending&&s_show.served==before&&s_show.deferred==deferred_before);}
    uint64_t boundary=0;assert(rsx_vulkan_backend_native_boundary(&boundary)==0&&boundary);
    assert(first->try_begin(first->userdata,first->lifetime)==0);first->end(first->userdata,first->lifetime);
    assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
    pvkDestroyFence(s_vk.device,j.fence,NULL);pvkDestroyCommandPool(s_vk.device,j.pool,NULL);
    be->color_target_release(NULL,image);rsx_draw_engine_flush();
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
    /* E5: an asynchronous ticket, even one still pending behind GPU work, is
     * an ordered native boundary; it certifies once it completes. */
    {
        const rsx_draw_backend* be=&s_vk_engine_backend;
        uint32_t depth=be->depth_target_create(NULL,4096,4096);assert(depth);
        uint32_t view=be->depth_z24_view(NULL,depth,4096,4096,0xAAE4u,0xA5u);assert(view);
        const uint64_t pending=be->submit_async(NULL,RSX_BE_FLUSH_GUEST_REFERENCE);assert(pending);
        const int was_pending=be->completion_status(NULL,pending,0)==0;
        uint64_t boundary=0;assert(rsx_vulkan_backend_native_boundary(&boundary)==0&&boundary);
        assert(first.try_begin(first.userdata,first.lifetime)==0);first.end(first.userdata,first.lifetime);
        assert(be->completion_status(NULL,pending,1)==1&&be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
        be->texture_release(NULL,view);be->depth_target_release(NULL,depth);
        printf("async boundary accepted (ticket pending at boundary: %d)\n",was_pending);
    }
    /* A direct present still running on the GPU is an ordered boundary too:
     * it is already on the queue, so native work queued after it runs after
     * it. Queued behind a large GPU job so that it is still pending. */
    {
        const rsx_draw_backend* be=&s_vk_engine_backend;
        uint32_t depth=be->depth_target_create(NULL,4096,4096);assert(depth);
        uint32_t view=be->depth_z24_view(NULL,depth,4096,4096,0xAAE4u,0xA5u);assert(view);
        uint32_t color=be->color_target_create(NULL,RSX_BE_FMT_R8G8B8A8,8,8,NULL,0);assert(color);
        assert(be->submit_async(NULL,RSX_BE_FLUSH_GUEST_REFERENCE));
        const uint64_t direct=s_vk.present_direct;
        be->present(NULL,color);
        assert(s_vk.present_direct==direct+1);       /* not read back */
        const int was_pending=pvkGetFenceStatus(s_vk.device,s_vk.present_fence)==VK_NOT_READY;
        uint64_t boundary=0;assert(rsx_vulkan_backend_native_boundary(&boundary)==0&&boundary);
        assert(be->submit_and_wait_checked(NULL,RSX_BE_FLUSH_GUEST_REFERENCE)==1);
        be->texture_release(NULL,view);be->depth_target_release(NULL,depth);be->color_target_release(NULL,color);
        printf("direct present boundary accepted (present pending at boundary: %d)\n",was_pending);
    }
    check_pin_release_nowait(&first);
    check_queue_order(&first);
    check_deferred_present(&first);
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
