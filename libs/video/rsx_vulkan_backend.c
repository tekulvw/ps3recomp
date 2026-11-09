/*
 * ps3recomp - Vulkan RSX Backend. See rsx_vulkan_backend.h.
 *
 * The file, in order:
 *   loader and device      libvulkan by dlopen, Vulkan 1.0 entry points only;
 *                          device choice, BC textures when the device has them
 *   memory, images, submit helpers
 *   vtable fallback path   the null backend's fixed-function contract:
 *                          position = attribute 0, flat colour = attribute 3
 *                          of each triangle's first vertex, texcoord0 =
 *                          attribute 8, fetched and expanded through the
 *                          shared rsx_fetch_attrib(); unit 0 decoded through
 *                          rsx_texture_decode(); depth test, no blend/stencil
 *   vtable guest programs  the same path running the decompilers' HLSL, with
 *                          whole mip chains and per-unit samplers
 *   optional window        SDL2 + swapchain; a present blits the frame into it
 *   draw engine backend    rsx_draw_backend: targets, clears, present and
 *                          readback (E1), pipelines and draws (E2), textures,
 *                          surface views and samplers (E3), depth snapshots
 *                          (E4), completion tickets and the GPU Z24S8 depth
                          store/alias/view (E5), opt-in batched recording
                          (E6) -- the stage names in the commit history
 *   public entry points    init picks the path, shutdown undoes everything
 *
 * Deliberately synchronous: every clear, draw and present is one one-shot
 * command buffer, submitted and waited on. Slow and trivially correct; it is
 * what lets the engine's submit_and_wait and staging reuse cost nothing, and
 * batching is the first thing to change once correctness is settled. The
 * exceptions are E5's completion tickets and GPU depth views, which are
 * submitted without a wait and retired in queue order, and E6's opt-in
 * batched recording (PS3RECOMP_VK_BATCHED_ENGINE=1), which records engine
 * operations and submits them only where something needs it (see there).
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL for their whole life: legal for
 * transfers, sampling and as attachments, so there is no layout bookkeeping.
 *
 * Shaders: libs/video/vulkan/rsx_vk_fallback.{vert,frag}, embedded as SPIR-V.
 * Regenerate after editing them with:
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_vert_spv \
 *       -o rsx_vk_fallback_vert.spv.h rsx_vk_fallback.vert
 *   glslangValidator -V --target-env vulkan1.0 --vn rsx_vk_fallback_frag_spv \
 *       -o rsx_vk_fallback_frag.spv.h rsx_vk_fallback.frag
 * then `expand -t 4` both outputs: glslang indents with tabs, the project
 * with spaces.
 */

#if !defined(_WIN32) || defined(PS3RECOMP_RSX_VULKAN)

#define VK_NO_PROTOTYPES
#ifdef _WIN32
/* Windows (opt-in): a Win32 window with VK_KHR_win32_surface instead of SDL,
 * and vulkan-1.dll loaded at run time like libvulkan elsewhere. */
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <vulkan/vulkan.h>

#include "rsx_vulkan_backend.h"
#include "rsx_fifo_busy.h"
#include "rsx_frontend_vulkan.h"
#include "rsx_frontend_internal.h"
#ifndef _WIN32
#define RSX_NATIVE_OWNER_PTHREAD   /* Windows: the gate's SRW lock */
#endif
#include "rsx_native_owner_gate.h"
#include <stdatomic.h>
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include "rsx_vp_decompiler.h"
#include "rsx_fp_decompiler.h"
#include "rsx_shader_spirv.h"
#include "rsx_draw_engine.h"
#include "rsx_present_thread.h"
#include "metal_frame_rate.h"   /* portable bounded present-rate statistics */
#include "vulkan/rsx_vk_fallback_vert.spv.h"
#include "vulkan/rsx_vk_fallback_frag.spv.h"
#include "vulkan/rsx_vk_depth_z24_comp.spv.h"

/* SDL2 is already required off Windows (cellPad, cellAudio); here it only
 * opens the optional window. Same include style as those two. */
#ifdef _WIN32
#include "../misc/osk_native.h"
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "../misc/osk_native.h"
#include "../input/pad_sdl_keyboard.h"

#include <dlfcn.h>
#endif
#include "rsx_vk_thread.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Run-time loaded entry points (Vulkan 1.0 core only)
 * -------------------------------------------------------------------------*/
#define VK_GLOBAL_FNS(X) X(vkCreateInstance)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkGetFenceStatus) X(vkResetFences) X(vkQueueSubmit) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdSetDepthBounds) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCmdBlitImage) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdCopyBuffer) X(vkResetDescriptorPool) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData)
/* Window-system entry points, loaded only when a window was requested, so a
 * headless run never depends on a driver that can present. */
#define VK_WSI_INSTANCE_FNS(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define VK_WSI_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

#define VK_DECLARE(name) static PFN_##name p##name;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
VK_WSI_INSTANCE_FNS(VK_DECLARE)
VK_WSI_DEVICE_FNS(VK_DECLARE)

/* ---------------------------------------------------------------------------
 * Backend state
 * -------------------------------------------------------------------------*/
typedef struct vk_vertex {
    float pos[4];
    float col[4];
    float uv[2];
} vk_vertex;

typedef struct vk_buffer {
    VkBuffer       buf;
    VkDeviceMemory mem;
    void*          ptr;
    VkDeviceSize   size;
    int            coherent;
} vk_buffer;

typedef struct vk_image {
    VkImage        img;
    VkDeviceMemory mem;
    VkImageView    view;
    VkFormat format;
    VkImageUsageFlags usage;
    VkImageAspectFlags aspect;
    u32 levels,layers,samples;
    u32 w, h;                        /* extent of level 0 */
    u8  pooled;                      /* returns to the image pool when destroyed */
} vk_image;

#define VK_SAMPLER_CACHE 32
#define VK_PIPELINE_SLOTS (2 * 2 * 8)   /* ztest x zwrite x compare op */

typedef struct vk_state {
    void*            lib;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    u32              queue_family;
    VkPhysicalDeviceMemoryProperties memprops;

    u32              width, height;
    vk_image         color;           /* R8G8B8A8_UNORM                 */
    vk_image         depth;           /* D32_SFLOAT or fallback         */
    VkFormat         depth_format;
    VkImageAspectFlags depth_aspect;  /* DEPTH, plus STENCIL if it has one  */

    VkRenderPass     render_pass;
    VkFramebuffer    framebuffer;
    VkShaderModule   vs, fs;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool desc_pool;
    VkDescriptorSet  desc_set;
    VkPipelineLayout pipe_layout;
    VkPipeline       pipelines[VK_PIPELINE_SLOTS];
    VkSampler        sampler;

    vk_image         dummy;           /* 1x1 magenta: "nothing bound"    */
    vk_image         tex[RSX_MAX_TEXTURES];   /* decoded guest textures  */
    int              tex_ready[RSX_MAX_TEXTURES];
    /* Guest path only: each unit's sampler, built from its wrap, filter and
     * LOD registers (the fallback path keeps `sampler`, the null backend's
     * nearest + repeat). Cached by register value; VK_NULL_HANDLE = use
     * `sampler`. */
    VkSampler        tex_sampler[RSX_MAX_TEXTURES];
    struct { u64 key; VkSampler s; } samp_cache[VK_SAMPLER_CACHE];
    u32              samp_count;

    vk_buffer        vertices;        /* expanded triangle list          */
    vk_vertex*       cpu_verts;
    size_t           cpu_cap, cpu_count;

    vk_buffer        readback;
    VkCommandPool    pool;
    VkCommandBuffer  cmd;
    VkFence          fence;
    int              frontend_enabled, frontend_recording;

    /* A present records and submits on its own pool, command buffer and
     * fences, so its second half can run on a host thread of its own while
     * the FIFO walker records the next frame (rsx_vulkan_backend.h,
     * present_begin/_finish). The queue is shared, and Vulkan wants every
     * vkQueueSubmit / vkQueuePresentKHR / vkDeviceWaitIdle on it externally
     * synchronised: queue_mu. present_mu keeps the present's own objects to
     * one present at a time, which matters when a flip in the command stream
     * presents on the walker's thread while a host thread finishes the last. */
    VkCommandPool    present_pool;
    /* A small ring, so a direct present never waits for the one before it:
     * a slot is reused only after its fence signalled. */
#define VK_PRESENT_SLOTS 3
    VkCommandBuffer  present_cmds[VK_PRESENT_SLOTS];
    VkFence          present_fences[VK_PRESENT_SLOTS];
    VkSemaphore      present_acquire[VK_PRESENT_SLOTS]; /* direct window presents */
    int              present_busy[VK_PRESENT_SLOTS];    /* submitted, not yet waited */
    VkImage          present_src[VK_PRESENT_SLOTS];     /* engine image the slot's present reads */
    u32              present_slot;    /* the slot the last present used     */
    VkCommandBuffer  present_cmd;     /* = present_cmds[present_slot]       */
    VkFence          present_fence;   /* = present_fences[present_slot]     */
    VkFence          window_fence;    /* the blit into the swapchain image     */
    int              present_inflight;/* a read-back present awaiting completion */
    u64              present_direct, present_readback_count;
    int              present_split;   /* inside rsx_vulkan_backend_present_begin */
    int              present_order;   /* the walker's next command buffer waits */
    pthread_mutex_t  queue_mu;
    pthread_mutex_t  present_mu;

    /* Optional window (PS3RECOMP_VK_WINDOW=1). Rendering stays offscreen
     * either way -- readback and the tests are unchanged -- and a present
     * additionally blits the frame into the window's swapchain. */
    int              eng_active;      /* the register-file engine drives us */
    int              bc_ok;           /* textureCompressionBC enabled       */
    int              depth_bounds_ok; /* depthBounds enabled                */
    int              anisotropy_ok;   /* samplerAnisotropy enabled          */
    u32              max_dim;         /* maxImageDimension2D of the device  */
    int              windowed;
#ifdef _WIN32
    HWND             window;
    int              window_closed;   /* WM_CLOSE or Esc                 */
#else
    SDL_Window*      window;
#endif
    const char*      inst_exts[16];
    u32              inst_ext_count;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkExtent2D       sc_extent;
    u32              sc_count;
    VkImage          sc_images[8];
    VkSemaphore      sc_done[8];      /* per image: the present waits on it */
    VkSemaphore      sem_acquire;
    double           hold_seconds;

    const rsx_state* state;           /* live RSX state, for draws       */
    u32              clear_argb;
    u32              last_center;
    u32              presented;
    const char*      dump_path;
    u32              dump_every;       /* write every nth presented frame */
    /* PS3RECOMP_VK_FPS=1: once a second, guest presents and window present
     * calls. Window times are host clock readings when vkQueuePresentKHR
     * returns, not compositor presentation timestamps. present_mu held. */
    int              fps_on;
    MetalFrameRate   fps;
    /* PS3RECOMP_VK_PIPELINE_CACHE=<directory>: every pipeline is created
     * through this cache, loaded from and saved to a file there. NULL (the
     * default) creates pipelines without one, as before. */
    VkPipelineCache  pcache;
    char*            pcache_path;
    u32              pcache_new;       /* pipelines created since the last save */
} vk_state;

static vk_state s_vk;
static void vk_pcache_created(void);
static rsx_frontend_device *s_vk_frontend;

#ifdef _WIN32
/* cellGcmSys.c is always part of the Windows runtime; COFF has no weak
 * undefined references, so these are ordinary declarations there. */
extern void cellGcm_rsx_consumer_begin(void);
extern void cellGcm_rsx_consumer_end(void);
#else
extern void cellGcm_rsx_consumer_begin(void) __attribute__((weak));
extern void cellGcm_rsx_consumer_end(void) __attribute__((weak));
#endif
static rsx_native_owner_gate s_vulkan_owner=RSX_NATIVE_OWNER_GATE_INIT;
static _Thread_local rsx_native_owner_thread s_vulkan_owner_thread;
static _Thread_local int s_vulkan_consumer;
static uint64_t s_vulkan_lifetime,s_vulkan_commits;
static VkFence s_vulkan_boundary_fence;
static uint64_t s_vulkan_boundary_commit,s_vulkan_boundary_generation;
static int s_vulkan_shutdown_pending;
static unsigned s_vulkan_partial_primitives;
static atomic_int s_vulkan_unscoped_submit;
/* Sticky engine failure: a submission, fence wait or recording call that did
 * not succeed. submit_and_wait_checked and completion tickets report it, the
 * same certificate Metal's command-buffer errors give. Cleared at engine init. */
static atomic_int s_vk_eng_failed;
/* Batched engine recording (E6, PS3RECOMP_VK_BATCHED_ENGINE=1); defined below. */
static int s_bat_on;
enum { VK_RET_IMAGE = 1, VK_RET_BUFFER, VK_RET_SPARE, VK_RET_FRAMEBUFFER, VK_RET_PIPELINE, VK_RET_STAGE };
static int vk_bat_begin_op(void);
static int vk_bat_end_op(void);
static int vk_bat_flush(int wait);
static int vk_bat_open(void);
static u64 s_bat_last_ticket;
static int vk_bat_retire(int kind, const void* object);
static void vk_bat_reap(void);
static int vk_sync(void);
static void vk_e5_fail(const char* what);
static int vk_bat_reserve(VkDeviceSize bytes, u32 sets);
static void* vk_bat_alloc(VkDeviceSize bytes, VkBuffer* buf, VkDeviceSize* off);
static VkDescriptorSet vk_bat_set(VkDescriptorSetLayout layout);
static int vk_bat_init(void);
static void vk_bat_shutdown(void);
#define VK_BAT_MAX_SLOTS 32u
typedef struct {
    int kind;
    u64 ticket;                 /* 0: the open batch's, set when it is submitted */
    union { vk_image im; vk_buffer buf; VkFramebuffer fb; VkPipeline pipe; } u;
} vk_bat_retired;
typedef struct {
    VkCommandPool    pool;
    VkCommandBuffer  cb;
    VkDescriptorPool dpool;
    u32              sets_left;
    vk_buffer        arena;
    VkDeviceSize     used;
    u64              ticket;    /* the submission that last used this slot */
} vk_bat_slot;
static struct {
    u32 nslots, cur, max_ops, ops;
    int open;
    VkCommandBuffer legacy_cmd;
    VkDeviceSize align;
    vk_bat_slot s[VK_BAT_MAX_SLOTS];
    vk_bat_retired* ret; u32 nret, cap;
    u8 *vsc, *psc; u32 vsc_cap, psc_cap;
    /* PS3RECOMP_RSX_INFLIGHT_LOG=1 counters */
    u64 batches, batch_ops, slot_waits, waits_for_cpu, retired, arena_grows;
} s_bat;

static int vulkan_owner_enabled(void){
    static atomic_int setting=-1;int value=atomic_load_explicit(&setting,memory_order_relaxed);
    if(value<0){const char* text=getenv("PS3RECOMP_RSX_NATIVE_OWNER");value=text&&!strcmp(text,"1");int expected=-1;
        if(!atomic_compare_exchange_strong_explicit(&setting,&expected,value,memory_order_relaxed,memory_order_relaxed))value=expected;}
    return value;
}
static int vulkan_owner_begin(int dirty){
    if(!vulkan_owner_enabled())return 1;
    if(s_vulkan_owner_thread.native){if(dirty)s_vulkan_owner.failed=1;return 0;}
    if(!s_vulkan_owner_thread.original_depth){s_vulkan_consumer=cellGcm_rsx_consumer_begin&&cellGcm_rsx_consumer_end;
        if(s_vulkan_consumer)cellGcm_rsx_consumer_begin();}
    const int outer=!s_vulkan_owner_thread.original_depth;
    if(outer)rsx_fifo_busy_enter(RSX_FIFO_BUSY_LEASE_WAIT);
    const int refused=rsx_owner_original_begin(&s_vulkan_owner,&s_vulkan_owner_thread);
    if(outer)rsx_fifo_busy_leave(RSX_FIFO_BUSY_LEASE_WAIT);
    if(refused)return 0;
    if(dirty)rsx_owner_dirty(&s_vulkan_owner,&s_vulkan_owner_thread);
    return 1;
}
static void vulkan_owner_finish(int preserve){
    if(!vulkan_owner_enabled())return;
    if(!preserve)rsx_owner_dirty(&s_vulkan_owner,&s_vulkan_owner_thread);
    if(atomic_load(&s_vulkan_unscoped_submit))rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,0);
    const int outer=s_vulkan_owner_thread.original_depth==1;
    rsx_owner_original_end(&s_vulkan_owner,&s_vulkan_owner_thread);
    if(outer&&s_vulkan_consumer){s_vulkan_consumer=0;cellGcm_rsx_consumer_end();}
}
static int vulkan_owner_common_begin(void* user,uint32_t flags){
    (void)user;if(!vulkan_owner_begin((flags&RSX_ORIGINAL_MUTATE)!=0))return 0;
    if((flags&RSX_ORIGINAL_LIFECYCLE_BIT)&&s_vulkan_owner.retained){vulkan_owner_finish(0);return 0;}
    return 1;
}
static void vulkan_owner_common_end(void* user,uint32_t flags,int delta,int preserve){
    (void)user;
    if(flags&RSX_ORIGINAL_FAILURE_BIT)rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,0);
    if(delta>0){if(s_vulkan_partial_primitives==UINT_MAX)rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,0);else ++s_vulkan_partial_primitives;}
    else if(delta<0){if(!s_vulkan_partial_primitives)rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,0);else --s_vulkan_partial_primitives;}
    vulkan_owner_finish(preserve&&!s_vulkan_partial_primitives);
}
static const rsx_original_owner_hooks s_vulkan_original_hooks={NULL,vulkan_owner_common_begin,vulkan_owner_common_end};
static void vulkan_owner_install_hooks(void){
    rsx_draw_engine_set_owner_hooks(&s_vulkan_original_hooks);
    rsx_commands_set_owner_hooks(&s_vulkan_original_hooks);
}
static void vulkan_owner_fault(void){
    if(vulkan_owner_enabled()&&s_vulkan_owner_thread.original_depth)
        rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,0);
}
static void vulkan_owner_submitted(VkResult status){
    if(!vulkan_owner_enabled())return;
    if(!s_vulkan_owner_thread.original_depth){atomic_store(&s_vulkan_unscoped_submit,1);return;}
    if(status!=VK_SUCCESS||s_vulkan_commits==UINT64_MAX)vulkan_owner_fault();else ++s_vulkan_commits;
}
static int vulkan_owner_boundary(uint64_t before){
    if(!vulkan_owner_enabled())return 1;
    const int clean=s_vulkan_commits>before&&!s_vulkan_partial_primitives&&!s_vk.frontend_recording&&!s_vulkan_owner.failed;
    if(clean)rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,1);
    return clean;
}
static int vulkan_owner_activate(void){
    if(s_vulkan_lifetime==UINT64_MAX)return -1;
    if(rsx_owner_generation(&s_vulkan_owner,&s_vulkan_owner_thread,++s_vulkan_lifetime))return -1;
    s_vulkan_shutdown_pending=0;s_vulkan_boundary_fence=VK_NULL_HANDLE;
    s_vulkan_boundary_commit=s_vulkan_boundary_generation=0;
    atomic_store(&s_vulkan_unscoped_submit,0);return 0;
}


#define VK_LOG(...)  fprintf(stderr, "[RSX vulkan] " __VA_ARGS__)
#define VK_CHECK(call, what)                                               \
    do { VkResult r_ = (call);                                             \
         if (r_ != VK_SUCCESS) {                                           \
             VK_LOG("%s failed (VkResult %d)\n", (what), (int)r_);         \
             vulkan_owner_fault();                                        \
             return -1; } } while (0)

/* ---------------------------------------------------------------------------
 * Pipeline cache (PS3RECOMP_VK_PIPELINE_CACHE=<existing directory>)
 *
 * Engine pipelines are created on the thread that walks the FIFO, on the
 * draw that first needs them, and a driver compile there is a visible
 * hitch. With the switch, every pipeline is created through one
 * VkPipelineCache. The cache is loaded at device creation from
 * <dir>/vk-pipelines-<vendor>-<device>.bin and saved there, through a
 * temporary file renamed over it, at shutdown and after every
 * VK_PCACHE_SAVE_EVERY new pipelines (a run that is killed keeps most of
 * them). A file whose header names another device, driver build or cache
 * version is not loaded: the cache starts empty and the file is replaced.
 * Unset, pipelines are created without a cache, as before.
 * -------------------------------------------------------------------------*/
#define VK_PCACHE_SAVE_EVERY 32u
#define VK_PCACHE_MAX_BYTES  (256u << 20)
static unsigned long long s_pcache_loaded_bytes, s_pcache_saves;

static void vk_pcache_save(void)
{
    if (!s_vk.pcache || !s_vk.pcache_path) return;
    size_t size = 0;
    if (pvkGetPipelineCacheData(s_vk.device, s_vk.pcache, &size, NULL) != VK_SUCCESS || !size ||
        size > VK_PCACHE_MAX_BYTES)
        return;
    void* data = malloc(size);
    if (!data) return;
    if (pvkGetPipelineCacheData(s_vk.device, s_vk.pcache, &size, data) == VK_SUCCESS) {
        const size_t n = strlen(s_vk.pcache_path);
        char* tmp = (char*)malloc(n + 5);
        if (tmp) {
            memcpy(tmp, s_vk.pcache_path, n); memcpy(tmp + n, ".tmp", 5);
            FILE* f = fopen(tmp, "wb");
            const int ok = f && fwrite(data, 1, size, f) == size;
            if (f && fclose(f)) { (void)0; }
            if (ok) {
#ifdef _WIN32
                remove(s_vk.pcache_path);
#endif
                if (rename(tmp, s_vk.pcache_path) == 0) s_pcache_saves++;
                else remove(tmp);
            } else remove(tmp);
            free(tmp);
        }
    }
    free(data);
    s_vk.pcache_new = 0;
}

/* A pipeline is about to be created through the cache. */
static void vk_pcache_created(void)
{
    if (s_vk.pcache && s_vk.pcache_path && ++s_vk.pcache_new >= VK_PCACHE_SAVE_EVERY) vk_pcache_save();
}

static void vk_pcache_open(void)
{
    const char* dir = getenv("PS3RECOMP_VK_PIPELINE_CACHE");
    if (!dir || !*dir || !s_vk.device) return;
    VkPhysicalDeviceProperties p; pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    const size_t n = strlen(dir) + 64;
    s_vk.pcache_path = (char*)malloc(n);
    if (!s_vk.pcache_path) return;
    snprintf(s_vk.pcache_path, n, "%s/vk-pipelines-%04x-%04x.bin", dir, p.vendorID, p.deviceID);
    void* data = NULL;
    size_t size = 0;
    FILE* f = fopen(s_vk.pcache_path, "rb");
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            const long len = ftell(f);
            if (len > 32 && (unsigned long)len <= VK_PCACHE_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0 &&
                (data = malloc((size_t)len)) != NULL && fread(data, 1, (size_t)len, f) == (size_t)len)
                size = (size_t)len;
        }
        fclose(f);
    }
    /* The header (VK_PIPELINE_CACHE_HEADER_VERSION_ONE): length, version,
     * vendor, device, then the driver's cache UUID. Anything else is not
     * this device's cache. */
    if (size) {
        const u8* h = (const u8*)data;
        u32 len, ver, vendor, device;
        memcpy(&len, h, 4); memcpy(&ver, h + 4, 4); memcpy(&vendor, h + 8, 4); memcpy(&device, h + 12, 4);
        if (len < 32 || len > size || ver != (u32)VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            vendor != p.vendorID || device != p.deviceID ||
            memcmp(h + 16, p.pipelineCacheUUID, VK_UUID_SIZE)) {
            VK_LOG("pipeline cache %s is for another device or driver build; starting empty\n", s_vk.pcache_path);
            size = 0;
        }
    }
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = size, .pInitialData = size ? data : NULL };
    if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_vk.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = NULL;
        if (pvkCreatePipelineCache(s_vk.device, &ci, NULL, &s_vk.pcache) != VK_SUCCESS) s_vk.pcache = VK_NULL_HANDLE;
        size = 0;
    }
    free(data);
    s_pcache_loaded_bytes = size;
    if (s_vk.pcache)
        VK_LOG("pipeline cache %s: %s (%zu bytes)\n", s_vk.pcache_path, size ? "loaded" : "new", size);
}

static void vk_pcache_close(void)
{
    if (s_vk.pcache) {
        vk_pcache_save();
        pvkDestroyPipelineCache(s_vk.device, s_vk.pcache, NULL);
        s_vk.pcache = VK_NULL_HANDLE;
    }
    free(s_vk.pcache_path);
    s_vk.pcache_path = NULL;
    s_vk.pcache_new = 0;
}

/* ---------------------------------------------------------------------------
 * Loading
 * -------------------------------------------------------------------------*/
#ifdef _WIN32
static int vk_load_library(void)
{
    s_vk.lib = (void*)LoadLibraryA("vulkan-1.dll");
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (LoadLibrary vulkan-1.dll: error %lu)\n", GetLastError());
        return -1;
    }
    FARPROC entry = GetProcAddress((HMODULE)s_vk.lib, "vkGetInstanceProcAddr");
    memcpy(&pvkGetInstanceProcAddr, &entry, sizeof entry);
    if (!pvkGetInstanceProcAddr) { VK_LOG("vulkan-1.dll has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}
#else
static int vk_load_library(void)
{
    static const char* names[] = { "libvulkan.so.1", "libvulkan.so" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !s_vk.lib; i++)
        s_vk.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!s_vk.lib) {
        VK_LOG("no Vulkan loader found (dlopen libvulkan.so.1: %s)\n", dlerror());
        return -1;
    }
    pvkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)dlsym(s_vk.lib, "vkGetInstanceProcAddr");
    if (!pvkGetInstanceProcAddr) { VK_LOG("libvulkan has no vkGetInstanceProcAddr\n"); return -1; }
#define LOAD_G(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(VK_NULL_HANDLE, #name))) \
        { VK_LOG("missing global function " #name "\n"); return -1; }
    VK_GLOBAL_FNS(LOAD_G)
    return 0;
}
#endif

static int vk_load_instance_functions(void)
{
#define LOAD_I(name) \
    if (!(p##name = (PFN_##name)pvkGetInstanceProcAddr(s_vk.instance, #name))) \
        { VK_LOG("missing instance function " #name "\n"); return -1; }
    VK_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_wsi_instance_functions(void)
{
    VK_WSI_INSTANCE_FNS(LOAD_I)
    return 0;
}

static int vk_load_device_functions(void)
{
#define LOAD_D(name) \
    if (!(p##name = (PFN_##name)pvkGetDeviceProcAddr(s_vk.device, #name))) \
        { VK_LOG("missing device function " #name "\n"); return -1; }
    VK_DEVICE_FNS(LOAD_D)
    return 0;
}

static int vk_load_wsi_device_functions(void)
{
    VK_WSI_DEVICE_FNS(LOAD_D)
    return 0;
}

/* ---------------------------------------------------------------------------
 * Device selection: PS3RECOMP_VK_DEVICE if set, else the first discrete GPU,
 * else integrated, else anything that is not a CPU rasteriser, else whatever
 * is there (llvmpipe/lavapipe on a GPU-less CI runner).
 * -------------------------------------------------------------------------*/
static int vk_device_rank(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 0;
    default:                                     return 1;
    }
}

static int vk_find_graphics_queue(VkPhysicalDevice pd, u32* family)
{
    u32 n = 0;
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (n == 0 || n > 64) return -1;
    VkQueueFamilyProperties props[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(pd, &n, props);
    for (u32 i = 0; i < n; i++) {
        if (!(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (s_vk.windowed) {           /* the same queue must also present */
            VkBool32 ok = VK_FALSE;
            if (pvkGetPhysicalDeviceSurfaceSupportKHR(pd, i, s_vk.surface, &ok) != VK_SUCCESS || !ok)
                continue;
        }
        *family = i; return 0;
    }
    return -1;
}

static int vk_has_device_extension(VkPhysicalDevice pd, const char* name)
{
    u32 n = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties* e = (VkExtensionProperties*)malloc(n * sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (pvkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (u32 i = 0; i < n && !found; i++) found = strcmp(e[i].extensionName, name) == 0;
    free(e);
    return found;
}

static int vk_pick_device(void)
{
    u32 n = 0;
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, NULL), "vkEnumeratePhysicalDevices");
    if (n == 0) { VK_LOG("no Vulkan physical devices\n"); return -1; }
    if (n > 16) n = 16;
    VkPhysicalDevice devs[16];
    VK_CHECK(pvkEnumeratePhysicalDevices(s_vk.instance, &n, devs), "vkEnumeratePhysicalDevices");

    int best = -1, best_rank = -1;
    const char* forced = getenv("PS3RECOMP_VK_DEVICE");
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        pvkGetPhysicalDeviceProperties(devs[i], &p);
        u32 fam;
        int usable = vk_find_graphics_queue(devs[i], &fam) == 0 &&
                     (!s_vk.windowed ||
                      vk_has_device_extension(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        VK_LOG("device %u: %s (type %d, api %u.%u.%u)%s\n", i, p.deviceName,
               (int)p.deviceType, VK_API_VERSION_MAJOR(p.apiVersion),
               VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
               usable ? "" : (s_vk.windowed ? " -- cannot present to the window"
                                            : " -- no graphics queue"));
        if (!usable) continue;
        if (forced) {
            if ((u32)atoi(forced) == i) { best = (int)i; break; }
            continue;
        }
        int rank = vk_device_rank(p.deviceType);
        if (rank > best_rank) { best = (int)i; best_rank = rank; }
    }
    if (best < 0) {
        if (forced) VK_LOG("PS3RECOMP_VK_DEVICE=%s is not a usable device\n", forced);
        else        VK_LOG("no device with a graphics queue\n");
        return -1;
    }
    s_vk.phys = devs[best];
    vk_find_graphics_queue(s_vk.phys, &s_vk.queue_family);
    pvkGetPhysicalDeviceMemoryProperties(s_vk.phys, &s_vk.memprops);
    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    /* Vulkan guarantees 4096; the engine's targets and textures may use what
     * the device really has (a title's 4608-wide target on RADV). */
    s_vk.max_dim = p.limits.maxImageDimension2D >= 4096u ? p.limits.maxImageDimension2D : 4096u;
    VK_LOG("using device %d: %s\n", best, p.deviceName);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Memory, images, buffers
 * -------------------------------------------------------------------------*/
static int vk_find_memory_type(u32 type_bits, VkMemoryPropertyFlags want, u32* out)
{
    for (u32 i = 0; i < s_vk.memprops.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) &&
            (s_vk.memprops.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return 0;
        }
    return -1;
}

static int vk_create_image_levels(vk_image* im, VkFormat fmt, u32 w, u32 h, u32 levels,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!levels) levels = 1;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt, .extent = { w, h, 1 }, .mipLevels = levels, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(pvkCreateImage(s_vk.device, &ici, NULL, &im->img), "vkCreateImage");
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) {
        VK_LOG("no memory type for an image\n"); return -1;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &im->mem), "vkAllocateMemory(image)");
    VK_CHECK(pvkBindImageMemory(s_vk.device, im->img, im->mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, levels, 0, 1 },
    };
    VK_CHECK(pvkCreateImageView(s_vk.device, &vci, NULL, &im->view), "vkCreateImageView");
    im->format=fmt;im->usage=usage;im->aspect=aspect;
    im->levels=levels;im->layers=1;im->samples=VK_SAMPLE_COUNT_1_BIT;
    im->w=w;im->h=h;
    return 0;
}

static int vk_create_image(vk_image* im, VkFormat fmt, u32 w, u32 h,
                           VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    return vk_create_image_levels(im, fmt, w, h, 1, usage, aspect);
}

/* Engine images a scene creates and releases over and over (colour targets,
 * the RGBA8 aliases GPU depth views fill) keep their memory: once the GPU is
 * done with a released one, its image goes back to a small pool, and a later
 * request of the same format, size and usage takes it with a fresh view. The
 * callers define the contents of what they get (a colour target is cleared,
 * an alias is filled whole), so a reused image is as good as a new one. */
#define VK_IMG_POOL       48u
#define VK_IMG_POOL_BYTES ((VkDeviceSize)256 << 20)
static vk_image     s_img_pool[VK_IMG_POOL];
static VkDeviceSize s_img_pool_size[VK_IMG_POOL];
static u32          s_img_pool_n;
static VkDeviceSize s_img_pool_bytes;
static int vk_img_pool_keep(vk_image* im)
{
    /* Not with the native owner, which may still hold views of engine images. */
    if (!im->pooled || !im->img || !im->mem || s_img_pool_n >= VK_IMG_POOL || vulkan_owner_enabled()) return 0;
    VkMemoryRequirements req;
    pvkGetImageMemoryRequirements(s_vk.device, im->img, &req);
    if (s_img_pool_bytes + req.size > VK_IMG_POOL_BYTES) return 0;
    if (im->view) { pvkDestroyImageView(s_vk.device, im->view, NULL); im->view = VK_NULL_HANDLE; }
    s_img_pool[s_img_pool_n] = *im;
    s_img_pool_size[s_img_pool_n++] = req.size;
    s_img_pool_bytes += req.size;
    memset(im, 0, sizeof *im);
    return 1;
}
static int vk_create_image_pooled(vk_image* im, VkFormat fmt, u32 w, u32 h,
                                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    for (u32 i = 0; i < s_img_pool_n; i++) {
        const vk_image* c = &s_img_pool[i];
        if (c->format != fmt || c->w != w || c->h != h || c->usage != usage || c->aspect != aspect || c->levels != 1)
            continue;
        *im = *c;
        s_img_pool_bytes -= s_img_pool_size[i];
        s_img_pool[i] = s_img_pool[--s_img_pool_n];
        s_img_pool_size[i] = s_img_pool_size[s_img_pool_n];
        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->img,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
            .subresourceRange = { aspect, 0, 1, 0, 1 },
        };
        if (pvkCreateImageView(s_vk.device, &vci, NULL, &im->view) != VK_SUCCESS) {
            pvkDestroyImage(s_vk.device, im->img, NULL); pvkFreeMemory(s_vk.device, im->mem, NULL);
            memset(im, 0, sizeof *im); return -1;
        }
        im->pooled = 1;
        return 0;
    }
    if (vk_create_image(im, fmt, w, h, usage, aspect)) return -1;
    im->pooled = 1;
    return 0;
}
static void vk_img_pool_drain(void)
{
    for (u32 i = 0; i < s_img_pool_n; i++) {
        pvkDestroyImage(s_vk.device, s_img_pool[i].img, NULL);
        pvkFreeMemory(s_vk.device, s_img_pool[i].mem, NULL);
    }
    s_img_pool_n = 0; s_img_pool_bytes = 0;
}

static void vk_present_wait_gpu(void);
static double vk_now_seconds(void);

/* PS3RECOMP_VK_PRESENT_TIMING=1 (diagnostic, default off): where a present's
 * wall time goes, split by stage, separately for presents the FIFO walker
 * makes at a flip and those the host frame clock makes. Every 240 presents
 * of a kind one line gives each stage's mean and max in ms. */
enum { VKPT_FLUSH, VKPT_LOCK, VKPT_COMPLETE, VKPT_SLOT, VKPT_ACQUIRE, VKPT_SUBMIT, VKPT_QPRESENT, VKPT_HOLD, VKPT_TOTAL, VKPT_N };
static const char* const s_vkpt_name[VKPT_N] = { "flush", "lock", "complete", "slot_wait", "acquire", "submit", "queue_present", "of_which_hold_wait", "total" };
static int s_vkpt_on = -1;
static uint64_t s_vkpt_cur[VKPT_N];
static uint64_t s_vkpt_sum[2][VKPT_N], s_vkpt_max[2][VKPT_N], s_vkpt_n[2];
static uint64_t vkpt_now(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int vkpt_enabled(void)
{
    if (s_vkpt_on < 0) { const char* e = getenv("PS3RECOMP_VK_PRESENT_TIMING"); s_vkpt_on = e && !strcmp(e, "1"); }
    return s_vkpt_on;
}
static void vkpt_add(int stage, uint64_t t0)
{
    if (s_vkpt_on > 0) s_vkpt_cur[stage] += vkpt_now() - t0;
}
static void vkpt_commit(int host)
{
    if (s_vkpt_on <= 0) return;
    for (int i = 0; i < VKPT_N; i++) {
        s_vkpt_sum[host][i] += s_vkpt_cur[i];
        if (s_vkpt_cur[i] > s_vkpt_max[host][i]) s_vkpt_max[host][i] = s_vkpt_cur[i];
        s_vkpt_cur[i] = 0;
    }
    if (++s_vkpt_n[host] < 240) return;
    char line[768]; int o = snprintf(line, sizeof line, "[vk present timing] by=%s n=%llu mean/max ms:",
                                     host ? "host" : "flip", (unsigned long long)s_vkpt_n[host]);
    for (int i = 0; i < VKPT_N && o > 0 && o < (int)sizeof line; i++)
        o += snprintf(line + o, sizeof line - (size_t)o, " %s=%.3f/%.3f", s_vkpt_name[i],
                      (double)s_vkpt_sum[host][i] / 1e6 / (double)s_vkpt_n[host], (double)s_vkpt_max[host][i] / 1e6);
    fprintf(stderr, "%s\n", line);
    memset(s_vkpt_sum[host], 0, sizeof s_vkpt_sum[host]);
    memset(s_vkpt_max[host], 0, sizeof s_vkpt_max[host]);
    s_vkpt_n[host] = 0;
}

static void vk_present_wait_image(VkImage img);
static void vk_destroy_image_now(vk_image* im)
{
    /* Never under a present's copy that may still read it. */
    if (im->img) vk_present_wait_image(im->img);
    if (vk_img_pool_keep(im)) return;
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* Released objects: destroyed at once, or -- batched -- when every batch
 * that may still use them has completed. */
static void vk_destroy_image(vk_image* im)
{
    if (!im->img && !im->view && !im->mem) return;
    if (vk_bat_retire(VK_RET_IMAGE, im)) { memset(im, 0, sizeof *im); return; }
    vk_destroy_image_now(im);
}
static void vk_destroy_buffer_now(vk_buffer* b);
static void vk_destroy_buffer(vk_buffer* b)
{
    if (!b->buf && !b->mem) return;
    if (vk_bat_retire(VK_RET_BUFFER, b)) { memset(b, 0, sizeof *b); return; }
    vk_destroy_buffer_now(b);
}

static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK_CHECK(pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    const VkMemoryPropertyFlags vis = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coh = vis | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    /* A buffer the GPU only copies into is one the CPU reads back, and the
     * first coherent type is usually write-combined and uncached (RADV's
     * GTT): there the readback memcpy was 82% of the FIFO thread's time in
     * Twisted Metal. Take a cached type for it when there is one; reads
     * invalidate, so a non-coherent one is fine. */
    const VkMemoryPropertyFlags cached = vis | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const int readback = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (readback && vk_find_memory_type(req.memoryTypeBits, cached | coh, &type) == 0) b->coherent = 1;
    else if (readback && vk_find_memory_type(req.memoryTypeBits, cached, &type) == 0) b->coherent = 0;
    else if (vk_find_memory_type(req.memoryTypeBits, coh, &type) == 0) b->coherent = 1;
    else if (vk_find_memory_type(req.memoryTypeBits, vis, &type) == 0) b->coherent = 0;
    else { VK_LOG("no host-visible memory for a buffer\n"); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    VK_CHECK(pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem), "vkAllocateMemory(buffer)");
    VK_CHECK(pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0), "vkBindBufferMemory");
    VK_CHECK(pvkMapMemory(s_vk.device, b->mem, 0, VK_WHOLE_SIZE, 0, &b->ptr), "vkMapMemory");
    b->size = size;
    return 0;
}

static void vk_destroy_buffer_now(vk_buffer* b)
{
    if (b->ptr) pvkUnmapMemory(s_vk.device, b->mem);
    if (b->buf) pvkDestroyBuffer(s_vk.device, b->buf, NULL);
    if (b->mem) pvkFreeMemory(s_vk.device, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host staging buffers for the engine's texture uploads and depth snapshots.
 * Each used to be created for one copy and destroyed after it, about a
 * hundred allocations a second in a busy scene. A released one is kept, once
 * the GPU has finished the copy that read it, and handed to the next request
 * it is large enough for. Sizes are rounded up to a power of two so they
 * recur; the pool is bounded by count and bytes, and anything beyond is
 * destroyed as before. */
#define VK_STAGE_POOL      32u
#define VK_STAGE_MIN       ((VkDeviceSize)64 << 10)
#define VK_STAGE_MAX_BYTES ((VkDeviceSize)256 << 20)
#define VK_STAGE_USAGE     (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
static vk_buffer    s_stage_free[VK_STAGE_POOL];
static u32          s_stage_nfree;
static VkDeviceSize s_stage_bytes;
static int vk_create_host_buffer(vk_buffer* b, VkDeviceSize size, VkBufferUsageFlags usage);
static void vk_stage_keep_or_destroy(vk_buffer* b)
{
    if (!b->buf) return;
    if (s_stage_nfree < VK_STAGE_POOL && s_stage_bytes + b->size <= VK_STAGE_MAX_BYTES) {
        s_stage_bytes += b->size;
        s_stage_free[s_stage_nfree++] = *b;
        memset(b, 0, sizeof *b);
    } else vk_destroy_buffer_now(b);
}
static int vk_stage_acquire(vk_buffer* out, VkDeviceSize size)
{
    u32 best = VK_STAGE_POOL;
    for (u32 i = 0; i < s_stage_nfree; i++)
        if (s_stage_free[i].size >= size && (best == VK_STAGE_POOL || s_stage_free[i].size < s_stage_free[best].size))
            best = i;
    if (best != VK_STAGE_POOL) {
        *out = s_stage_free[best];
        s_stage_bytes -= out->size;
        s_stage_free[best] = s_stage_free[--s_stage_nfree];
        return 0;
    }
    VkDeviceSize want = VK_STAGE_MIN;
    while (want < size) want <<= 1;
    memset(out, 0, sizeof *out);
    if (vk_create_host_buffer(out, want, VK_STAGE_USAGE) || !out->coherent) {
        vk_destroy_buffer_now(out); return -1;
    }
    return 0;
}
static int vk_bat_retire(int kind, const void* object);
/* After the copy that reads it is recorded: back to the pool once the GPU is
 * done with it (batched), or at once (per-operation submissions waited). */
static void vk_stage_release(vk_buffer* b)
{
    if (!b->buf) return;
    if (vk_bat_retire(VK_RET_STAGE, b)) { memset(b, 0, sizeof *b); return; }
    vk_stage_keep_or_destroy(b);
}
static void vk_stage_drain(void)
{
    for (u32 i = 0; i < s_stage_nfree; i++) vk_destroy_buffer_now(&s_stage_free[i]);
    s_stage_nfree = 0; s_stage_bytes = 0;
}

/* Host writes to non-coherent memory need a flush; there is no
 * vkFlushMappedMemoryRanges loaded, so writes only ever go to coherent
 * buffers (every desktop/L4T driver exposes one). Reads invalidate. */
static void vk_invalidate(vk_buffer* b)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                              .memory = b->mem, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkInvalidateMappedMemoryRanges(s_vk.device, 1, &r);
}

/* ---------------------------------------------------------------------------
 * One-shot command submission
 *
 * The queue is shared with the present (see present_mu in vk_state), so every
 * operation on it goes through these.
 * -------------------------------------------------------------------------*/
/* ---- Reserved queue order (rsx_vulkan_backend_queue_order) ----------------
 *
 * Metal fixes a native submission's place in the queue by enqueueing its
 * command buffer at the native boundary and committing it later. A Vulkan
 * queue has no such slot, and a timeline semaphore that later original
 * submissions wait on cannot stand in for one: the native signal would be
 * submitted after those waits on the same queue, and implementations process
 * a queue's submissions in order (Mesa's queue thread holds every later
 * submission behind a wait-before-signal), so it deadlocks.
 *
 * Instead the order is kept on the CPU. reserve(), called under the native
 * lease at the boundary, starts a hold: every original queue submission from
 * then on (engine batches and tickets, presents, depth views) is recorded
 * here, in order, with its fence, instead of reaching the queue. The owner
 * bookkeeping still runs as if it had been submitted. submit(), called once
 * from the native worker without the lease, submits the native batch, then
 * every held one in order, and ends the hold; submit() with no batch abandons
 * the reservation the same way. While a hold is active:
 *   - waiting on a held fence blocks until submit() runs (the GPU cannot run
 *     that work before the native batch either, which is Metal's behaviour);
 *   - a swapchain present and a device-wide idle wait first wait for the hold
 *     to end, since they depend on held submissions reaching the queue;
 *   - the native lease (try_begin) reports -4, so no other native submission
 *     can overtake held original work.
 * Every submit() entry also ends a hold whose submissions failed: the failure
 * latches like any other, and nothing is left held. */
typedef struct {
    VkFence fence;
    uint32_t nwait, ncb, nsig;
    VkSemaphore* wait; VkPipelineStageFlags* stage; VkCommandBuffer* cb; VkSemaphore* sig;
} vk_held_submit;
static struct {
    int active;
    uint64_t token, next_token;
    vk_held_submit* q; uint32_t n, cap;
    uint64_t reservations, held, abandoned, failures;
} s_hold;
static pthread_cond_t s_hold_cv = PTHREAD_COND_INITIALIZER;
/* The native boundary's bounded wait for a held original submission. */
#define VK_BOUNDARY_HOLD_WAIT_MS 50
static uint64_t s_boundary_hold_waits, s_boundary_hold_refusals;
static int vk_present_is_fence(VkFence f);

/* queue_mu held. */
static VkResult vk_hold_push(const VkSubmitInfo* si, VkFence fence)
{
    if (si->pNext) return VK_ERROR_FEATURE_NOT_PRESENT;        /* nothing here chains one */
    if (s_hold.n == s_hold.cap) {
        const uint32_t cap = s_hold.cap ? s_hold.cap * 2u : 64u;
        vk_held_submit* n = (vk_held_submit*)realloc(s_hold.q, (size_t)cap * sizeof *n);
        if (!n) return VK_ERROR_OUT_OF_HOST_MEMORY;
        s_hold.q = n; s_hold.cap = cap;
    }
    vk_held_submit h = { fence, si->waitSemaphoreCount, si->commandBufferCount, si->signalSemaphoreCount, 0, 0, 0, 0 };
    h.wait = h.nwait ? (VkSemaphore*)malloc(h.nwait * sizeof *h.wait) : NULL;
    h.stage = h.nwait ? (VkPipelineStageFlags*)malloc(h.nwait * sizeof *h.stage) : NULL;
    h.cb = h.ncb ? (VkCommandBuffer*)malloc(h.ncb * sizeof *h.cb) : NULL;
    h.sig = h.nsig ? (VkSemaphore*)malloc(h.nsig * sizeof *h.sig) : NULL;
    if ((h.nwait && (!h.wait || !h.stage)) || (h.ncb && !h.cb) || (h.nsig && !h.sig)) {
        free(h.wait); free(h.stage); free(h.cb); free(h.sig); return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (h.nwait) { memcpy(h.wait, si->pWaitSemaphores, h.nwait * sizeof *h.wait);
                   memcpy(h.stage, si->pWaitDstStageMask, h.nwait * sizeof *h.stage); }
    if (h.ncb) memcpy(h.cb, si->pCommandBuffers, h.ncb * sizeof *h.cb);
    if (h.nsig) memcpy(h.sig, si->pSignalSemaphores, h.nsig * sizeof *h.sig);
    s_hold.q[s_hold.n++] = h;
    s_hold.held++;
    return VK_SUCCESS;
}

/* queue_mu held: every held submission onto the queue, in order; ends the hold. */
static void vk_show_kick(void);
static void vk_hold_release_locked(void)
{
    for (uint32_t i = 0; i < s_hold.n; i++) {
        vk_held_submit* h = &s_hold.q[i];
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .waitSemaphoreCount = h->nwait, .pWaitSemaphores = h->wait, .pWaitDstStageMask = h->stage,
                            .commandBufferCount = h->ncb, .pCommandBuffers = h->cb,
                            .signalSemaphoreCount = h->nsig, .pSignalSemaphores = h->sig };
        if (pvkQueueSubmit(s_vk.queue, 1, &si, h->fence) != VK_SUCCESS) {
            atomic_store(&s_vk_eng_failed, 1);
            s_vulkan_owner.failed = 1;
            s_hold.failures++;
        }
        free(h->wait); free(h->stage); free(h->cb); free(h->sig);
    }
    s_hold.n = 0;
    s_hold.active = 0; s_hold.token = 0;
    pthread_cond_broadcast(&s_hold_cv);
    vk_show_kick();
}

/* queue_mu held: until no reservation holds original submissions. */
static void vk_hold_wait_locked(void)
{
    while (s_hold.active) pthread_cond_wait(&s_hold_cv, &s_vk.queue_mu);
}

/* A fence of a held submission is not on the queue yet, and the releasing
 * thread will pass it to vkQueueSubmit, which needs it externally
 * synchronised: read its status as pending, and wait for the release before
 * waiting on it. */
static int vk_hold_has_locked(VkFence f)
{
    if (!s_hold.active || !f) return 0;
    for (uint32_t i = 0; i < s_hold.n; i++) if (s_hold.q[i].fence == f) return 1;
    return 0;
}
static VkResult vk_fence_status(VkFence f)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const int held = vk_hold_has_locked(f);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return held ? VK_NOT_READY : pvkGetFenceStatus(s_vk.device, f);
}
static VkResult vk_fence_wait(VkFence f)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    while (vk_hold_has_locked(f)) pthread_cond_wait(&s_hold_cv, &s_vk.queue_mu);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return pvkWaitForFences(s_vk.device, 1, &f, VK_TRUE, UINT64_MAX);
}

static VkResult vk_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = s_hold.active ? vk_hold_push(si, fence) : pvkQueueSubmit(s_vk.queue, 1, si, fence);
    vulkan_owner_submitted(r);
    if(vulkan_owner_enabled()&&r==VK_SUCCESS&&s_vulkan_owner_thread.original_depth&&!s_vulkan_owner.failed){
        s_vulkan_boundary_fence=fence;
        s_vulkan_boundary_commit=s_vulkan_commits;
        s_vulkan_boundary_generation=s_vulkan_owner.generation;
    }
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

/* PS3RECOMP_RSX_FIFO_BUSY: swapchain acquire and present block the presenting
 * thread like Metal's drawable wait; both count as "drawable". */
static VkResult vk_busy_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAWABLE);
    const VkResult r = pvkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAWABLE);
    return r;
}
static VkResult vk_queue_present_core(const VkPresentInfoKHR* pi);
static VkResult vk_queue_present(const VkPresentInfoKHR* pi)
{
    rsx_fifo_busy_enter(RSX_FIFO_BUSY_DRAWABLE);
    const VkResult r = vk_queue_present_core(pi);
    rsx_fifo_busy_leave(RSX_FIFO_BUSY_DRAWABLE);
    return r;
}
static VkResult vk_queue_present_core(const VkPresentInfoKHR* pi)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const uint64_t th = s_vkpt_on > 0 ? vkpt_now() : 0;
    vk_hold_wait_locked();               /* it waits on a semaphore a held submission signals */
    vkpt_add(VKPT_HOLD, th);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static void vk_device_idle(void)
{
    if (!s_vk.device || !pvkDeviceWaitIdle) return;
    pthread_mutex_lock(&s_vk.queue_mu);
    vk_hold_wait_locked();               /* held work is not on the queue yet */
    VkResult status=pvkDeviceWaitIdle(s_vk.device);
    if(status!=VK_SUCCESS)vulkan_owner_fault();
    pthread_mutex_unlock(&s_vk.queue_mu);
}

/* A full execution and memory dependency on everything submitted before
 * this command buffer, the present's copy included. */
static void vk_cmd_after_everything(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static int vk_begin_core(void);
static int vk_begin(void)
{
    const int rc = s_bat_on ? vk_bat_begin_op() : vk_begin_core();
    if (rc && s_vk.eng_active) atomic_store(&s_vk_eng_failed, 1);
    return rc;
}
static int vk_begin_core(void)
{
    VK_CHECK(pvkResetCommandBuffer(s_vk.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_CHECK(pvkBeginCommandBuffer(s_vk.cmd, &bi), "vkBeginCommandBuffer");
    s_vk.frontend_recording = 1;
    /* A present's copy may still be reading the surface this frame is about
     * to draw over: order everything here after it. Set only by a present,
     * which never runs alongside the walker's recording. */
    if (s_vk.present_order) { vk_cmd_after_everything(s_vk.cmd); s_vk.present_order = 0; }
    return 0;
}

static int vk_submit_and_wait_core(void);
static int vk_submit_and_wait(void)
{
    /* Batched: the operation stays recorded; vk_sync() is the waiting end. */
    const int rc = s_bat_on ? vk_bat_end_op() : vk_submit_and_wait_core();
    if (rc && s_vk.eng_active) atomic_store(&s_vk_eng_failed, 1);
    return rc;
}
static int vk_submit_and_wait_core(void)
{
    VK_CHECK(pvkEndCommandBuffer(s_vk.cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &s_vk.cmd };
    VK_CHECK(pvkResetFences(s_vk.device, 1, &s_vk.fence), "vkResetFences");
    VK_CHECK(vk_queue_submit(&si, s_vk.fence), "vkQueueSubmit");
    s_vk.frontend_recording = 0;
    VK_CHECK(vk_fence_wait(s_vk.fence), "vkWaitForFences");
    return 0;
}

/* Order this command buffer's use of an image after everything submitted
 * before it; from UNDEFINED it is also the one-time move to GENERAL. */
static void vk_barrier_image_on(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                                VkImageLayout old_layout)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = old_layout, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void vk_barrier_image(VkImage img, VkImageAspectFlags aspect, VkImageLayout old_layout)
{
    vk_barrier_image_on(s_vk.cmd, img, aspect, old_layout);
}

/* ---------------------------------------------------------------------------
 * Texture unit 0
 * -------------------------------------------------------------------------*/
static void vk_write_texture_descriptor(void)
{
    VkDescriptorImageInfo ii = { .sampler = s_vk.sampler,
                                 .imageView = s_vk.tex_ready[0] ? s_vk.tex[0].view
                                                                : s_vk.dummy.view,
                                 .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = s_vk.desc_set, .dstBinding = 0,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                               .pImageInfo = &ii };
    pvkUpdateDescriptorSets(s_vk.device, 1, &w, 0, NULL);
}

/* Replace *dst with an RGBA8 image of `nlv` levels: level m is lw[m] x lh[m]
 * texels, tightly packed, the levels one after another in `rgba`. */
static int vk_upload_texture_levels(vk_image* dst, const u8* rgba,
                                    const u32* lw, const u32* lh, u32 nlv)
{
    VkBufferImageCopy regions[RSX_MAX_TEXTURE_LEVELS];
    if (!nlv || nlv > RSX_MAX_TEXTURE_LEVELS) return -1;
    VkDeviceSize total = 0;
    for (u32 m = 0; m < nlv; m++) {
        regions[m] = (VkBufferImageCopy){
            .bufferOffset = total,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1 },
            .imageExtent = { lw[m], lh[m], 1 },
        };
        total += (VkDeviceSize)lw[m] * lh[m] * 4u;
    }
    vk_buffer staging = {0};
    if (vk_create_host_buffer(&staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !staging.coherent) {
        vk_destroy_buffer(&staging); return -1;
    }
    memcpy(staging.ptr, rgba, (size_t)total);

    vk_image fresh = {0};
    if (vk_create_image_levels(&fresh, VK_FORMAT_R8G8B8A8_UNORM, lw[0], lh[0], nlv,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&fresh); vk_destroy_buffer(&staging); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(fresh.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        pvkCmdCopyBufferToImage(s_vk.cmd, staging.buf, fresh.img, VK_IMAGE_LAYOUT_GENERAL,
                                nlv, regions);
        rc = vk_submit_and_wait();
    }
    vk_destroy_buffer(&staging);
    if (rc) { vk_destroy_image(&fresh); return -1; }

    vk_destroy_image(dst);          /* idle: every submission is waited on */
    *dst = fresh;
    return 0;
}

static int vk_upload_texture(vk_image* dst, const u8* rgba, u32 w, u32 h)
{
    return vk_upload_texture_levels(dst, rgba, &w, &h, 1);
}

/* NV4097_SET_TEXTURE_ADDRESS wrap field, one per axis -- the table the Metal
 * backend and the live draw engine use: 1 WRAP, 2 MIRROR, 3 CLAMP_TO_EDGE,
 * 4 BORDER, 5 CLAMP, 6..8 MIRROR_ONCE. Vulkan 1.0 has MIRROR_CLAMP_TO_EDGE
 * only behind an extension, so the MIRROR_ONCE family clamps to edge here --
 * exact for texcoords in [0,1], the mirrored half is what is lost. */
static VkSamplerAddressMode vk_gcm_wrap(u32 w)
{
    switch (w & 0xFu) {
    case 1:  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;      /* 3, 5..8, unset */
    }
}

/* A unit's sampler from its registers, decoded as the Metal backend's
 * samp_slot_for does: SET_TEXTURE_FILTER min at [18:16] (1 NEAREST,
 * 2 LINEAR, 3..6 the four nearest/linear x nearest/linear-mip pairs), mag at
 * [26:24]; SET_TEXTURE_CONTROL0 max LOD at [18:7], min LOD at [30:19], 4.8
 * fixed point. The LOD bias (FILTER [12:0]) is not applied, as in Metal.
 * VK_NULL_HANDLE on failure: the caller falls back to `sampler`. */
static VkSampler vk_unit_sampler(const rsx_texture_state* t)
{
    const u32 minf = (t->filter >> 16) & 7u, magf = (t->filter >> 24) & 7u;
    const u32 lod  = (t->control0 >> 7) & 0xFFFFFFu;       /* max then min LOD */
    const u64 key  = (u64)minf | ((u64)magf << 3)
                   | ((u64)(t->address & 0x000F0F0Fu) << 6) | ((u64)lod << 26);
    for (u32 i = 0; i < s_vk.samp_count; i++)
        if (s_vk.samp_cache[i].key == key) return s_vk.samp_cache[i].s;
    if (s_vk.samp_count >= VK_SAMPLER_CACHE) return VK_NULL_HANDLE;

    const int mip_present = (minf >= 3);
    const int mip_linear  = (minf == 5 || minf == 6);
    /* A min filter with no mip term samples level 0 only: pin the range. */
    const float min_lod = (float)((t->control0 >> 19) & 0xFFFu) / 256.0f;
    float max_lod = mip_present ? (float)((t->control0 >> 7) & 0xFFFu) / 256.0f : 0.0f;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = (magf == 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (minf == 2 || minf == 4 || minf == 6) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(t->address),
        .addressModeV = vk_gcm_wrap(t->address >> 8),
        .addressModeW = vk_gcm_wrap(t->address >> 16),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    VkSampler smp = VK_NULL_HANDLE;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return VK_NULL_HANDLE;
    s_vk.samp_cache[s_vk.samp_count].key = key;
    s_vk.samp_cache[s_vk.samp_count].s   = smp;
    s_vk.samp_count++;
    return smp;
}

/* One level of a guest texture into tightly packed RGBA8, with the null
 * backend's decode (rsx_null_backend.c, nullsw_bind_texture): formats that
 * decode to fewer than four bytes are splayed, alpha forced opaque. */
static int vk_decode_level_rgba8(u8* dst, const u8* src, u32 w, u32 h,
                                 const rsx_tex_layout* tl)
{
    const u32 pitch = w * 4u;
    if (tl->fmt == RSX_TEXFMT_R8G8B8A8) {
        rsx_texture_decode(dst, pitch, src, w, h, tl, rsx_texture_argb_is_rgba());
        return 0;
    }
    const u32 srcp = tl->row_bytes;
    u8* tmp = (u8*)malloc((size_t)srcp * h);
    if (!tmp) return -1;
    rsx_texture_decode(tmp, srcp, src, w, h, tl, 0);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            const u8* sp = tmp + (size_t)y * srcp + (size_t)x * tl->bytes_per_texel;
            u8* dp = dst + (size_t)y * pitch + (size_t)x * 4u;
            dp[0] = sp[0];
            dp[1] = tl->bytes_per_texel > 1 ? sp[1] : sp[0];
            dp[2] = tl->bytes_per_texel > 2 ? sp[2] : sp[0];
            dp[3] = 255;
        }
    free(tmp);
    return 0;
}

/* Unit `unit` has nothing usable bound. The fallback path's descriptor only
 * ever shows unit 0, so only that one needs rewriting. */
static void vk_tex_off(u32 unit)
{
    s_vk.tex_ready[unit] = 0;
    s_vk.tex_sampler[unit] = VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* Same decode as the headless null backend (rsx_null_backend.c,
 * nullsw_bind_texture), so both backends sample identical texels. */
static void vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t)
{
    (void)ud;
    if (unit >= RSX_MAX_TEXTURES || !t || !s_vk.device) return;
    if (!(t->control0 & 0x80000000u) || !t->offset) { vk_tex_off(unit); return; }

    u32 w = (t->image_rect >> 16) & 0xFFFFu;
    u32 h =  t->image_rect        & 0xFFFFu;
    if (!w || !h || w > 4096u || h > 4096u) { vk_tex_off(unit); return; }

    u32 ea = cellGcmResolveLocated((t->format & 3u) == 1u, t->offset);
    if (!vm_base || ea == 0xFFFFFFFFu) { vk_tex_off(unit); return; }
    const u32 fmt = (t->format >> 8) & 0xFFu;

    /* The whole mip chain, as the Metal backend reads it: SET_TEXTURE_FORMAT's
     * level count above bit 15 and SET_TEXTURE_CONTROL3's row pitch, laid out
     * by the shared rsx_texture_mip_chain() (which clamps the count to what
     * the dimensions allow). Cube maps are not handled yet: face 0 only. */
    const u32 levels    = (t->format >> 16) & 0xFFFFu;
    const u32 row_pitch = t->control3 & 0xFFFFFu;
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 nlv = rsx_texture_mip_chain(fmt, w, h, levels, row_pitch, lv);
    if (!nlv || lv[0].tl.compressed) { vk_tex_off(unit); return; }   /* no BC path yet */

    size_t total = 0, off[RSX_MAX_TEXTURE_LEVELS];
    u32 lw[RSX_MAX_TEXTURE_LEVELS], lh[RSX_MAX_TEXTURE_LEVELS];
    for (u32 m = 0; m < nlv; m++) {
        off[m] = total;
        lw[m] = lv[m].w; lh[m] = lv[m].h;
        total += (size_t)lv[m].w * lv[m].h * 4u;
    }
    u8* buf = (u8*)malloc(total);
    if (!buf) { vk_tex_off(unit); return; }
    for (u32 m = 0; m < nlv; m++)
        if (vk_decode_level_rgba8(buf + off[m], vm_base + ea + lv[m].offset,
                                  lv[m].w, lv[m].h, &lv[m].tl)) {
            free(buf); vk_tex_off(unit); return;
        }
    s_vk.tex_ready[unit] = vk_upload_texture_levels(&s_vk.tex[unit], buf, lw, lh, nlv) == 0;
    free(buf);
    s_vk.tex_sampler[unit] = s_vk.tex_ready[unit] ? vk_unit_sampler(t) : VK_NULL_HANDLE;
    if (unit == 0) vk_write_texture_descriptor();
}

/* ---------------------------------------------------------------------------
 * Render pass, framebuffer, pipelines
 * -------------------------------------------------------------------------*/
static int vk_pick_depth_format(void)
{
    /* With stencil first -- titles use it for shadows, reflections and UI
     * masks -- and D32 first within each group: a D32 depth aspect copies out
     * as 32-bit floats, which is what depth snapshots are built from. */
    static const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM
    };
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if ((fp.optimalTilingFeatures & need) == need) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    /* TRANSFER_DST is a 1.1 format feature bit; 1.0 drivers may not report
     * it although clears work. Retry on the attachment bit alone. */
    for (size_t i = 0; i < sizeof candidates / sizeof candidates[0]; i++) {
        VkFormatProperties fp;
        pvkGetPhysicalDeviceFormatProperties(s_vk.phys, candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            s_vk.depth_format = candidates[i]; goto chosen;
        }
    }
    VK_LOG("no usable depth format\n");
    return -1;
chosen:
    s_vk.depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (s_vk.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
        s_vk.depth_format == VK_FORMAT_D24_UNORM_S8_UINT)
        s_vk.depth_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return 0;
}

static int vk_create_render_pass(void)
{
    VkAttachmentDescription att[2] = {
        { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_vk.depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,     /* ignored without stencil */
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    VK_CHECK(pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_vk.render_pass), "vkCreateRenderPass");

    VkImageView views[2] = { s_vk.color.view, s_vk.depth.view };
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = s_vk.render_pass, .attachmentCount = 2,
                                    .pAttachments = views, .width = s_vk.width,
                                    .height = s_vk.height, .layers = 1 };
    VK_CHECK(pvkCreateFramebuffer(s_vk.device, &fci, NULL, &s_vk.framebuffer), "vkCreateFramebuffer");
    return 0;
}

static int vk_create_shader(const uint32_t* code, size_t bytes, VkShaderModule* out)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = bytes, .pCode = code };
    VK_CHECK(pvkCreateShaderModule(s_vk.device, &ci, NULL, out), "vkCreateShaderModule");
    return 0;
}

static int vk_create_pipeline_objects(void)
{
    if (vk_create_shader(rsx_vk_fallback_vert_spv, sizeof rsx_vk_fallback_vert_spv, &s_vk.vs) ||
        vk_create_shader(rsx_vk_fallback_frag_spv, sizeof rsx_vk_fallback_frag_spv, &s_vk.fs))
        return -1;

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &bind };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_vk.set_layout),
             "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_vk.desc_pool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_vk.desc_pool,
                                        .descriptorSetCount = 1, .pSetLayouts = &s_vk.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_vk.desc_set), "vkAllocateDescriptorSets");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int32_t) };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_vk.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_vk.pipe_layout),
             "vkCreatePipelineLayout");

    /* Nearest + repeat: the null backend's point sample with wrapping. */
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                .maxLod = 0.0f };
    VK_CHECK(pvkCreateSampler(s_vk.device, &sci, NULL, &s_vk.sampler), "vkCreateSampler");

    /* A 1x1 magenta dummy keeps the descriptor valid before any bind --
     * the same "not bound" colour the null backend's sampler returns. */
    static const u8 magenta[4] = { 0xFF, 0x00, 0xFF, 0xFF };
    if (vk_upload_texture(&s_vk.dummy, magenta, 1, 1)) return -1;
    vk_write_texture_descriptor();
    return 0;
}

/* NV4097 depth function is the GL enum 0x200 NEVER .. 0x207 ALWAYS, which is
 * VkCompareOp 0..7 in the same order. Anything else passes, as in the null
 * and D3D12 backends (including the 1 rsx_state_init seeds). */
static u32 vk_compare_slot(u32 gl_func)
{
    return (gl_func >= 0x200u && gl_func <= 0x207u) ? gl_func - 0x200u
                                                    : (u32)VK_COMPARE_OP_ALWAYS;
}

static VkPipeline vk_get_pipeline(int ztest, int zwrite, u32 cmp)
{
    u32 slot = ((u32)ztest * 2u + (u32)zwrite) * 8u + cmp;
    if (s_vk.pipelines[slot]) return s_vk.pipelines[slot];

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vk.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_vk.fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, sizeof(vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[3] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, pos) },
        { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vk_vertex, col) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vk_vertex, uv)  },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* No culling: the null backend fills both windings. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_vk.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL, &p);
    vk_pcache_created();
    if (r != VK_SUCCESS) { VK_LOG("vkCreateGraphicsPipelines failed (%d)\n", (int)r); return VK_NULL_HANDLE; }
    s_vk.pipelines[slot] = p;
    return p;
}

/* ---------------------------------------------------------------------------
 * Draw path
 * -------------------------------------------------------------------------*/
static void vk_cb_track_state(void* ud, const rsx_state* state)
{
    (void)ud; s_vk.state = state;
}

static int vk_push_vertex(const float pos[4], const float col[4], const float uv[4])
{
    if (s_vk.cpu_count == s_vk.cpu_cap) {
        size_t cap = s_vk.cpu_cap ? s_vk.cpu_cap * 2 : 1024;
        vk_vertex* n = (vk_vertex*)realloc(s_vk.cpu_verts, cap * sizeof *n);
        if (!n) return -1;
        s_vk.cpu_verts = n; s_vk.cpu_cap = cap;
    }
    vk_vertex* v = &s_vk.cpu_verts[s_vk.cpu_count++];
    memcpy(v->pos, pos, sizeof v->pos);
    memcpy(v->col, col, sizeof v->col);
    v->uv[0] = uv[0]; v->uv[1] = uv[1];
    return 0;
}

/* One triangle, colour from its first vertex (the null backend's flat shade). */
static void vk_emit_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    float p[3][4], uv[3][4], col[4];
    rsx_fetch_attrib(st, 0, i0, p[0]);
    rsx_fetch_attrib(st, 0, i1, p[1]);
    rsx_fetch_attrib(st, 0, i2, p[2]);
    rsx_fetch_attrib(st, 3, i0, col);
    rsx_fetch_attrib(st, 8, i0, uv[0]);
    rsx_fetch_attrib(st, 8, i1, uv[1]);
    rsx_fetch_attrib(st, 8, i2, uv[2]);
    for (int k = 0; k < 3; k++) vk_push_vertex(p[k], col, uv[k]);
}

/* Called once per expanded triangle: the fixed-function path records the
 * null backend's flat-shaded vertex, the guest path all sixteen attributes. */
typedef void (*vk_tri_fn)(const rsx_state* st, u32 i0, u32 i1, u32 i2);

/* Primitive expansion, identical to the null backend's draw_prim(). */
static void vk_expand(const rsx_state* st, u32 prim, u32 first, u32 count, vk_tri_fn emit)
{
    switch (prim) {
    case RSX_PRIMITIVE_TRIANGLES:
        for (u32 i = 0; i + 2 < count; i += 3) emit(st, first + i, first + i + 1, first + i + 2);
        break;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
        for (u32 i = 0; i + 2 < count; i++)
            emit(st, first + i, first + i + 1 + (i & 1), first + i + 2 - (i & 1));
        break;
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_POLYGON:
        for (u32 i = 1; i + 1 < count; i++) emit(st, first, first + i, first + i + 1);
        break;
    case RSX_PRIMITIVE_QUADS:
        for (u32 i = 0; i + 3 < count; i += 4) {
            emit(st, first + i, first + i + 1, first + i + 2);
            emit(st, first + i, first + i + 2, first + i + 3);
        }
        break;
    case RSX_PRIMITIVE_QUAD_STRIP:
        for (u32 i = 0; i + 3 < count; i += 2) {
            emit(st, first + i, first + i + 1, first + i + 3);
            emit(st, first + i, first + i + 3, first + i + 2);
        }
        break;
    default:
        break;   /* points and lines: no coverage, as in the null backend */
    }
}

static int vk_ensure_vertex_buffer(VkDeviceSize bytes)
{
    if (s_vk.vertices.buf && s_vk.vertices.size >= bytes) return 0;
    VkDeviceSize size = s_vk.vertices.size ? s_vk.vertices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_vk.vertices);   /* idle: every submission is waited on */
    if (vk_create_host_buffer(&s_vk.vertices, size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return -1;
    if (!s_vk.vertices.coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guest programs (stage V3)
 *
 * With the HLSL -> SPIR-V translator built and PS3RECOMP_VK_GUEST_PROGRAMS=1,
 * a draw runs the guest's own vertex and fragment programs, as the Metal
 * backend's guest path does and keyed the same way: decompiled to HLSL by the
 * shared decompilers, translated by rsx_shader_spirv.cpp, cached per program.
 * Constants use the Metal/D3D12 layout: VPConst is the 512 transform
 * constants plus the viewport epilogue, PSConstants the fragment program's
 * inline constants plus fp_alpha.
 *
 * Vertices are still fetched and expanded on the CPU through the shared
 * rsx_fetch_attrib(), now all sixteen attributes (a disabled one reads its
 * constant register, as on the hardware). Every RSX vertex format therefore
 * arrives as float4, and the pipeline has one fixed input layout: location n
 * is attribute n, which is what the vertex decompiler declares.
 *
 * A program that needs a cube map or a vertex texture falls back to the
 * fixed-function path, with a message. Not honoured on this path yet: the RSX
 * viewport and scissor (the whole target is used, as on the fallback path),
 * blending, stencil, culling, per-unit sampler state.
 * -------------------------------------------------------------------------*/
#define VK_VP_CACHE      256
#define VK_FP_CACHE      512
#define VK_GPIPE_CACHE   512
#define VK_FP_MAX_BYTES  4096u              /* bound on a fragment program, as Metal/D3D12 */
#define VK_HLSL_BYTES    (256u * 1024u)
#define VK_SPV_WORDS     (256u * 1024u)
#define VK_VPCONST_BYTES ((RSX_MAX_VERTEX_CONSTANTS + 2u) * 16u)
#define VK_GUEST_ATTRS   16u
#define VK_GUEST_FLOATS  (VK_GUEST_ATTRS * 4u)   /* per vertex */

typedef struct { u32 hash; VkShaderModule mod; } vk_vp_entry;
typedef struct { u64 key; VkShaderModule mod; u32 nconst; } vk_fp_entry;
typedef struct { int vs, fs; u32 depth_key; VkPipeline pipe; } vk_gpipe_entry;

static struct {
    int                   on;
    char*                 hlsl;
    u32*                  spv;
    char                  log[4096];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      pipe_layout;
    VkDescriptorPool      pool;
    VkDescriptorSet       set;
    vk_buffer             ubo;              /* VPConst at 0, PSConstants at ps_off */
    VkDeviceSize          ps_off;
    VkDeviceSize          max_ubo_range;
    vk_buffer             vertices;
    float*                cpu;
    size_t                cpu_cap, cpu_count;   /* in vertices */
    vk_vp_entry           vp[VK_VP_CACHE];     u32 vp_count;
    vk_fp_entry           fp[VK_FP_CACHE];     u32 fp_count;
    vk_gpipe_entry        pipes[VK_GPIPE_CACHE]; u32 pipe_count;
    rsx_fp_constant_block consts;
    u32                   draws, last_draws;
    u32                   warned;
} s_g;

int rsx_vulkan_backend_guest_programs(void)
{
    const char* v = getenv("PS3RECOMP_VK_GUEST_PROGRAMS");
    return rsx_hlsl_to_spirv_available() && v && v[0] && v[0] != '0';
}

static u32 vk_fnv1a32(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static u64 vk_fnv1a64(const void* d, size_t n, u64 h)
{
    const u8* p = (const u8*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* Built from the registers, as the Metal and D3D12 backends do. */
static u32 vk_cube_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const rsx_texture_state* t = &st->textures[u];
        const u32 w = (t->image_rect >> 16) & 0xFFFFu, h = t->image_rect & 0xFFFFu;
        if ((t->control0 & 0x80000000u) && (t->format & 4u) && w && w == h) m |= 1u << u;
    }
    return m;
}

static u32 vk_vtex_mask(const rsx_state* st)
{
    u32 m = 0;
    for (u32 u = 0; u < RSX_MAX_VERTEX_TEXTURES; u++)
        if (st->vertex_textures[u].control0 & 0x80000000u) m |= 1u << u;
    return m;
}

static VkShaderModule vk_guest_module(const char* what, int stage)
{
    u32 n = 0;
    if (rsx_hlsl_to_spirv(s_g.hlsl, stage, s_g.spv, VK_SPV_WORDS, &n,
                          s_g.log, sizeof s_g.log) != 0) {
        VK_LOG("%s: %s\n", what, s_g.log);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)n * 4u, .pCode = s_g.spv };
    VkShaderModule m = VK_NULL_HANDLE;
    if (pvkCreateShaderModule(s_vk.device, &ci, NULL, &m) != VK_SUCCESS) {
        VK_LOG("%s: vkCreateShaderModule failed\n", what);
        return VK_NULL_HANDLE;
    }
    return m;
}

/* The vertex program, keyed on its own bytes to its end bit (as Metal). */
static int vk_vp_slot(const rsx_state* st)
{
    if (st->vp_ucode_bytes < 16) return -1;
    u32 vstart = st->transform_program_start * 16u;
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    const u8* uc = st->vp_ucode + vstart;
    const u32 avail = st->vp_ucode_bytes - vstart;
    const u32 instrs = rsx_vp_program_size_instrs(uc, avail);
    const u32 len = instrs ? instrs * 16u : avail;
    const u32 hash = vk_fnv1a32(uc, len);
    for (u32 i = 0; i < s_g.vp_count; i++)
        if (s_g.vp[i].hash == hash) return s_g.vp[i].mod ? (int)i : -1;
    if (s_g.vp_count >= VK_VP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "vertex program %08X", hash);
    VkShaderModule m = VK_NULL_HANDLE;
    const int ni = rsx_vp_decompile_ex(uc, len, 0, s_g.hlsl, VK_HLSL_BYTES);
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_VERTEX);
    if (s_g.vp_count < 32)
        VK_LOG("%s: %d instrs -> %s\n", what, ni, m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.vp_count++;
    s_g.vp[slot].hash = hash;
    s_g.vp[slot].mod  = m;
    return m ? slot : -1;
}

/* The fragment program, resolved and keyed as the Metal backend's
 * fp_slot_for: structure (inline constants live in PSConstants), export
 * width, and the alpha test patched into the source. No cube units here --
 * the caller has already sent those to the fixed-function path. */
static int vk_fp_slot(const rsx_state* st, const u8** uc)
{
    if (!vm_base || st->shader_program == 0) return -1;
    const u32 off = cellGcmResolveLocated((st->shader_program & 3u) == 1u,
                                          st->shader_program & ~3u);
    if (off == 0xFFFFFFFFu) return -1;
    *uc = vm_base + off;
    u64 key = rsx_fp_structural_hash(*uc, VK_FP_MAX_BYTES, 1469598103934665603ull);
    if (!key) return -1;                       /* malformed or unterminated */
    const u32 ctrl     = st->shader_control;
    const u32 ctrl_key = ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
    const u32 alpha_en = (st->alpha_test_enable && st->alpha_func != 0x0207u) ? 1u : 0u;
    const u32 alpha_fn = alpha_en ? st->alpha_func : 0u;
    key = vk_fnv1a64(&ctrl_key, sizeof ctrl_key, key);
    key = vk_fnv1a64(&alpha_en, sizeof alpha_en, key);
    key = vk_fnv1a64(&alpha_fn, sizeof alpha_fn, key);
    for (u32 i = 0; i < s_g.fp_count; i++)
        if (s_g.fp[i].key == key) return s_g.fp[i].mod ? (int)i : -1;
    if (s_g.fp_count >= VK_FP_CACHE) return -1;

    char what[64];
    snprintf(what, sizeof what, "fragment program %016llX", (unsigned long long)key);
    VkShaderModule m = VK_NULL_HANDLE;
    u32 nconst = 0;
    int ni = rsx_fp_decompile_buffered_ex(*uc, VK_FP_MAX_BYTES, ctrl, 0,
                                          s_g.hlsl, VK_HLSL_BYTES, &nconst);
    if (ni > 0 && alpha_en &&
        rsx_fp_apply_alpha_test_buffered(s_g.hlsl, VK_HLSL_BYTES, st->alpha_func) < 0)
        ni = -1;
    if (ni <= 0) VK_LOG("%s: decompile failed (%d)\n", what, ni);
    else         m = vk_guest_module(what, RSX_SHADER_STAGE_FRAGMENT);
    if (s_g.fp_count < 32)
        VK_LOG("%s: %d instrs, %u constants -> %s\n", what, ni, nconst,
               m ? "ok" : "FAILED (fixed-function instead)");
    const int slot = (int)s_g.fp_count++;
    s_g.fp[slot].key    = key;
    s_g.fp[slot].mod    = m;
    s_g.fp[slot].nconst = nconst;
    return m ? slot : -1;
}

static VkPipeline vk_guest_pipeline(int vs, int fs, int ztest, int zwrite, u32 cmp)
{
    const u32 dkey = (u32)ztest | ((u32)zwrite << 1) | (cmp << 2);
    for (u32 i = 0; i < s_g.pipe_count; i++)
        if (s_g.pipes[i].vs == vs && s_g.pipes[i].fs == fs && s_g.pipes[i].depth_key == dkey)
            return s_g.pipes[i].pipe;
    if (s_g.pipe_count >= VK_GPIPE_CACHE) return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_g.vp[vs].mod, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_g.fp[fs].mod, .pName = "main" },
    };
    VkVertexInputBindingDescription vb = { 0, VK_GUEST_FLOATS * 4u, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) {
        va[a].location = a;
        va[a].binding  = 0;
        va[a].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
        va[a].offset   = a * 16u;
    }
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = VK_GUEST_ATTRS, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = ztest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)cmp };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_g.pipe_layout, .renderPass = s_vk.render_pass,
        .subpass = 0 };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL, &p);
    vk_pcache_created();
    if (r != VK_SUCCESS) {
        VK_LOG("guest pipeline: vkCreateGraphicsPipelines failed (%d)\n", (int)r);
        return VK_NULL_HANDLE;
    }
    vk_gpipe_entry* e = &s_g.pipes[s_g.pipe_count++];
    e->vs = vs; e->fs = fs; e->depth_key = dkey; e->pipe = p;
    return p;
}

/* One vertex: all sixteen attributes as float4, attribute n at float 4n. */
static int vk_guest_push(const rsx_state* st, u32 idx)
{
    if (s_g.cpu_count == s_g.cpu_cap) {
        size_t cap = s_g.cpu_cap ? s_g.cpu_cap * 2 : 1024;
        float* n = (float*)realloc(s_g.cpu, cap * VK_GUEST_FLOATS * sizeof(float));
        if (!n) return -1;
        s_g.cpu = n; s_g.cpu_cap = cap;
    }
    float* v = s_g.cpu + s_g.cpu_count * VK_GUEST_FLOATS;
    for (u32 a = 0; a < VK_GUEST_ATTRS; a++) rsx_fetch_attrib(st, (int)a, idx, v + a * 4u);
    s_g.cpu_count++;
    return 0;
}

static void vk_guest_tri(const rsx_state* st, u32 i0, u32 i1, u32 i2)
{
    vk_guest_push(st, i0);
    vk_guest_push(st, i1);
    vk_guest_push(st, i2);
}

static int vk_guest_init(void)
{
    if (!rsx_vulkan_backend_guest_programs()) return 0;
    s_g.hlsl = (char*)malloc(VK_HLSL_BYTES);
    s_g.spv  = (u32*)malloc(VK_SPV_WORDS * sizeof(u32));
    if (!s_g.hlsl || !s_g.spv) { VK_LOG("guest programs: out of memory\n"); return -1; }

    VkPhysicalDeviceProperties p;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_g.max_ubo_range = p.limits.maxUniformBufferRange;
    VkDeviceSize align = p.limits.minUniformBufferOffsetAlignment;
    if (align == 0) align = 256;
    s_g.ps_off = (VK_VPCONST_BYTES + align - 1u) / align * align;
    if (VK_VPCONST_BYTES > s_g.max_ubo_range) {
        VK_LOG("guest programs: maxUniformBufferRange %llu < VPConst\n",
               (unsigned long long)s_g.max_ubo_range);
        return -1;
    }

    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[4] = {
        { RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL },
        { RSX_SPIRV_TEXTURE_BINDING, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES,
          VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    VK_CHECK(pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_g.set_layout),
             "vkCreateDescriptorSetLayout(guest)");
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    VK_CHECK(pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_g.pool),
             "vkCreateDescriptorPool(guest)");
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_g.pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkAllocateDescriptorSets(s_vk.device, &dai, &s_g.set), "vkAllocateDescriptorSets(guest)");
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_g.set_layout };
    VK_CHECK(pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_g.pipe_layout),
             "vkCreatePipelineLayout(guest)");
    if (vk_create_host_buffer(&s_g.ubo,
                              s_g.ps_off + (RSX_FP_MAX_INLINE_CONSTANTS + 1u) * 16u,
                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) || !s_g.ubo.coherent) {
        VK_LOG("guest programs: no host-coherent uniform memory\n");
        return -1;
    }
    s_g.on = 1;
    VK_LOG("guest programs: on (HLSL -> SPIR-V through glslang)\n");
    return 0;
}

static void vk_guest_shutdown(void)
{
    if (s_vk.device) {
        for (u32 i = 0; i < s_g.pipe_count; i++)
            if (s_g.pipes[i].pipe) pvkDestroyPipeline(s_vk.device, s_g.pipes[i].pipe, NULL);
        for (u32 i = 0; i < s_g.vp_count; i++)
            if (s_g.vp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.vp[i].mod, NULL);
        for (u32 i = 0; i < s_g.fp_count; i++)
            if (s_g.fp[i].mod) pvkDestroyShaderModule(s_vk.device, s_g.fp[i].mod, NULL);
        if (s_g.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_g.pipe_layout, NULL);
        if (s_g.pool)        pvkDestroyDescriptorPool(s_vk.device, s_g.pool, NULL);
        if (s_g.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_g.set_layout, NULL);
        vk_destroy_buffer(&s_g.ubo);
        vk_destroy_buffer(&s_g.vertices);
    }
    free(s_g.hlsl);
    free(s_g.spv);
    free(s_g.cpu);
    memset(&s_g, 0, sizeof s_g);
}

static int vk_ensure_buffer(vk_buffer* b, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    if (b->buf && b->size >= bytes) return 0;
    VkDeviceSize size = b->size ? b->size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(b);                 /* idle: every submission is waited on */
    if (vk_create_host_buffer(b, size, usage)) return -1;
    if (!b->coherent) { VK_LOG("vertex memory is not host-coherent\n"); return -1; }
    return 0;
}


/* Returns 1 when the draw went through the guest programs (or had nothing to
 * draw), 0 when the caller must use the fixed-function path instead. */
static int vk_guest_draw(const rsx_state* st, u32 prim, u32 first, u32 count)
{
    if (!s_g.on) return 0;
    if (vk_cube_mask(st) || vk_vtex_mask(st)) {
        if (!(s_g.warned & 1u)) {
            VK_LOG("guest programs: cube maps and vertex textures are not supported "
                   "yet; those draws use the fixed-function path\n");
            s_g.warned |= 1u;
        }
        return 0;
    }
    const int vs = vk_vp_slot(st);
    if (vs < 0) return 0;
    const u8* fp_uc = NULL;
    const int fs = vk_fp_slot(st, &fp_uc);
    if (fs < 0) return 0;
    /* The constant count must be what the program was compiled against. */
    if (rsx_fp_collect_constants(fp_uc, VK_FP_MAX_BYTES, &s_g.consts) < 0 ||
        s_g.consts.count != s_g.fp[fs].nconst)
        return 0;
    const u32 nslots = s_g.consts.count ? s_g.consts.count : 1u;
    const u32 fp_len = (nslots + 1u) * 16u;
    if (fp_len > s_g.max_ubo_range) return 0;

    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_guest_pipeline(vs, fs, ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return 0;

    s_g.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_guest_tri);
    if (!s_g.cpu_count) return 1;
    const VkDeviceSize bytes = (VkDeviceSize)s_g.cpu_count * VK_GUEST_FLOATS * sizeof(float);
    if (vk_ensure_buffer(&s_g.vertices, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return 0;
    memcpy(s_g.vertices.ptr, s_g.cpu, (size_t)bytes);

    /* VPConst: the 512 transform constants, then the viewport epilogue as the
     * Metal and D3D12 backends write it -- x/y identity, and z remapped from
     * GL clip space to [0, 1] when the guest programmed a z scale. */
    u8* vpc = (u8*)s_g.ubo.ptr;
    memcpy(vpc, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16u);
    float* vpx = (float*)(vpc + RSX_MAX_VERTEX_CONSTANTS * 16u);
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (st->viewport_scale[2] != 0.0f) { vpx[2] = st->viewport_scale[2]; vpx[6] = st->viewport_offset[2]; }
    else                                { vpx[2] = 1.0f;                  vpx[6] = 0.0f; }
    /* PSConstants: inline constants as host-order bit patterns, then fp_alpha. */
    u8* fpc = (u8*)s_g.ubo.ptr + s_g.ps_off;
    memset(fpc, 0, fp_len);
    if (s_g.consts.count) memcpy(fpc, s_g.consts.values, s_g.consts.count * 16u);
    float* alpha = (float*)(fpc + nslots * 16u);
    alpha[0] = rsx_fp_alpha_ref(st->alpha_ref, st->surface_format & 0x1Fu);

    /* Descriptors: rewritten per draw, which is safe because every submission
     * is waited on before the next one is recorded. */
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, VK_VPCONST_BYTES },
        { s_g.ubo.buf, s_g.ps_off, fp_len },
    };
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         s_vk.tex_ready[u] ? s_vk.tex[u].view : s_vk.dummy.view,
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ s_vk.tex_sampler[u] ? s_vk.tex_sampler[u] : s_vk.sampler,
                                         VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s_g.set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si },
    };
    pvkUpdateDescriptorSets(s_vk.device, 4, w, 0, NULL);

    if (vk_begin()) return 0;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vpt);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_g.pipe_layout,
                             0, 1, &s_g.set, 0, NULL);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_g.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_g.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    if (vk_submit_and_wait()) return 0;
    s_g.draws++;
    return 1;
}

static void vk_draw(u32 prim, u32 first, u32 count)
{
    const rsx_state* st = s_vk.state;
    if (!st || !s_vk.device || count < 3) return;
    if (vk_guest_draw(st, prim, first, count)) return;

    s_vk.cpu_count = 0;
    vk_expand(st, prim, first, count, vk_emit_tri);
    if (!s_vk.cpu_count) return;

    const VkDeviceSize bytes = (VkDeviceSize)s_vk.cpu_count * sizeof(vk_vertex);
    if (vk_ensure_vertex_buffer(bytes)) return;
    memcpy(s_vk.vertices.ptr, s_vk.cpu_verts, (size_t)bytes);

    /* Depth writes need the test enabled, as on the hardware and in both
     * reference backends. */
    const int ztest  = st->depth_test_enable ? 1 : 0;
    const int zwrite = ztest && st->depth_mask;
    VkPipeline pipe = vk_get_pipeline(ztest, zwrite, vk_compare_slot(st->depth_func));
    if (!pipe) return;
    const int32_t textured = (s_vk.tex_ready[0] && st->vertex_attribs[8].enabled) ? 1 : 0;

    if (vk_begin()) return;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = s_vk.render_pass, .framebuffer = s_vk.framebuffer,
                                  .renderArea = { { 0, 0 }, { s_vk.width, s_vk.height } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    /* Whole target, as the null backend's ndc_to_px maps it. */
    VkViewport vp = { 0.0f, 0.0f, (float)s_vk.width, (float)s_vk.height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { s_vk.width, s_vk.height } };
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vk.pipe_layout,
                             0, 1, &s_vk.desc_set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_vk.pipe_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof textured, &textured);
    VkDeviceSize off = 0;
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &s_vk.vertices.buf, &off);
    pvkCmdDraw(s_vk.cmd, (u32)s_vk.cpu_count, 1, 0, 0);
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

static void vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    (void)ud;
    vk_draw(primitive, first, count);
}

static void vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count)
{
    /* Not read yet, as in the null backend: the harness geometry is
     * non-indexed. Real titles need this; it lands with the index fetch. */
    (void)ud; (void)primitive; (void)offset; (void)count;
}

/* ---------------------------------------------------------------------------
 * Clear and present
 * -------------------------------------------------------------------------*/
static void vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud; (void)stencil;                       /* no stencil buffer yet */
    s_vk.clear_argb = color;
    /* CLEAR_SURFACE bits: 0x01 depth, 0x02 stencil, 0xF0 the colour channels
     * (all or nothing, as in the null backend). */
    const int do_color = (flags & 0xF0u) != 0;
    const int do_depth = (flags & 0x01u) != 0;
    if (!s_vk.device || (!do_color && !do_depth)) return;

    if (vk_begin()) return;
    if (do_color) {
        VkClearColorValue v;
        v.float32[0] = (float)((color >> 16) & 0xFFu) / 255.0f;   /* R */
        v.float32[1] = (float)((color >>  8) & 0xFFu) / 255.0f;   /* G */
        v.float32[2] = (float)( color        & 0xFFu) / 255.0f;   /* B */
        v.float32[3] = (float)((color >> 24) & 0xFFu) / 255.0f;   /* A */
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    }
    if (do_depth) {
        VkClearDepthStencilValue d = { depth, 0 };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    }
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Optional window: PS3RECOMP_VK_WINDOW=1
 *
 * An SDL2 window with a Vulkan swapchain. Rendering still targets the
 * offscreen image, so readback and every test are unchanged; a present then
 * also blits that image into the next swapchain image. Any failure on this
 * path drops back to headless with a message instead of failing the run.
 * -------------------------------------------------------------------------*/
static int vk_env_on(const char* name)
{
    const char* v = getenv(name);
    return v && v[0] && v[0] != '0';
}

static void vk_window_drop(const char* why)
{
    if (why) VK_LOG("window disabled, continuing headless: %s\n", why);
    if (s_vk.device && pvkDeviceWaitIdle) {
        vk_device_idle();
        for (u32 i = 0; i < 8; i++)
            if (s_vk.sc_done[i]) {
                pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
                s_vk.sc_done[i] = VK_NULL_HANDLE;
            }
        if (s_vk.sem_acquire) pvkDestroySemaphore(s_vk.device, s_vk.sem_acquire, NULL);
        if (s_vk.swapchain && pvkDestroySwapchainKHR)
            pvkDestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
    }
    if (s_vk.surface && s_vk.instance && pvkDestroySurfaceKHR)
        pvkDestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
    if (s_vk.window) {
#ifdef _WIN32
#ifdef OSK_NATIVE_HAS_WIN32_HOST
        osk_native_win32_set_window(NULL);
#endif
        DestroyWindow(s_vk.window);
#else
#ifdef OSK_NATIVE_HAS_SDL_HOST
        osk_native_sdl_set_window(NULL);   /* retire a dialog before SDL video goes */
#endif
#ifdef PAD_HAS_SDL_KEYBOARD
        ps3recomp_pad_sdl_keyboard_sample(NULL, 1);   /* release every key */
#endif
        SDL_DestroyWindow(s_vk.window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
#endif
    }
    s_vk.window = NULL;
    s_vk.surface = VK_NULL_HANDLE;
    s_vk.swapchain = VK_NULL_HANDLE;
    s_vk.sem_acquire = VK_NULL_HANDLE;
    s_vk.sc_count = 0;
    s_vk.windowed = 0;
}

#ifdef _WIN32
static LRESULT CALLBACK vk_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE:
        s_vk.window_closed = 1;
        return 0;   /* vk_window_drop destroys it on the backend's own terms */
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { s_vk.window_closed = 1; return 0; }
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = vk_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_vulkan";
    RegisterClassExA(&wc);   /* a second init finds the class already registered */
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    RECT wr = { 0, 0, (LONG)s_vk.width, (LONG)s_vk.height };
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) {
        style = WS_POPUP | WS_VISIBLE;
        x = y = 0;
        wr.right = GetSystemMetrics(SM_CXSCREEN);
        wr.bottom = GetSystemMetrics(SM_CYSCREEN);
    } else {
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    }
    /* Keep the display awake, as the D3D12 and null backends do. */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    s_vk.window_closed = 0;
    s_vk.window = CreateWindowExA(0, "ps3recomp_vulkan",
                                  title && title[0] ? title : "ps3recomp",
                                  style, x, y, wr.right - wr.left, wr.bottom - wr.top,
                                  NULL, NULL, GetModuleHandleA(NULL), NULL);
    s_vk.windowed = 1;
    if (!s_vk.window) {
        char why[128];
        snprintf(why, sizeof why, "CreateWindowEx failed (error %lu)", GetLastError());
        vk_window_drop(why);
        return;
    }
#ifdef OSK_NATIVE_HAS_WIN32_HOST
    osk_native_win32_set_window(s_vk.window);
#endif
    s_vk.inst_exts[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    s_vk.inst_exts[1] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    s_vk.inst_ext_count = 2;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: Win32, %u surface extension(s)\n", s_vk.inst_ext_count);
}

static int vk_create_window_surface(void)
{
    PFN_vkCreateWin32SurfaceKHR create = (PFN_vkCreateWin32SurfaceKHR)
        pvkGetInstanceProcAddr(s_vk.instance, "vkCreateWin32SurfaceKHR");
    if (!create) return -1;
    VkWin32SurfaceCreateInfoKHR info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                         .hinstance = GetModuleHandleA(NULL), .hwnd = s_vk.window };
    return create(s_vk.instance, &info, NULL, &s_vk.surface) == VK_SUCCESS ? 0 : -1;
}

static void vk_window_drawable_size(int* w, int* h)
{
    RECT r = { 0, 0, 0, 0 };
    GetClientRect(s_vk.window, &r);
    *w = (int)(r.right - r.left); *h = (int)(r.bottom - r.top);
}
#else
static void vk_window_open(const char* title)
{
    if (!vk_env_on("PS3RECOMP_VK_WINDOW")) return;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        VK_LOG("window disabled, continuing headless: SDL video init failed: %s\n", SDL_GetError());
        return;
    }
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (vk_env_on("PS3RECOMP_VK_FULLSCREEN")) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    s_vk.window = SDL_CreateWindow(title && title[0] ? title : "ps3recomp",
                                   SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   (int)s_vk.width, (int)s_vk.height, flags);
    s_vk.windowed = 1;          /* so vk_window_drop also shuts SDL video down */
    if (!s_vk.window) {
        char why[256];
        snprintf(why, sizeof why, "SDL_CreateWindow failed: %s", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    unsigned n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, NULL) || n == 0 || n > 16 ||
        !SDL_Vulkan_GetInstanceExtensions(s_vk.window, &n, s_vk.inst_exts)) {
        char why[256];
        snprintf(why, sizeof why, "no Vulkan surface extensions (%s)", SDL_GetError());
        vk_window_drop(why);
        return;
    }
    s_vk.inst_ext_count = n;
    const char* hold = getenv("PS3RECOMP_VK_HOLD");
    s_vk.hold_seconds = hold ? atof(hold) : 0.0;
    VK_LOG("window: SDL video driver '%s', %u surface extension(s)\n",
           SDL_GetCurrentVideoDriver(), n);
#ifdef OSK_NATIVE_HAS_SDL_HOST
    /* This thread owns SDL video and pumps it, so the OSK dialog lives here. */
    osk_native_sdl_set_window(s_vk.window);
#endif
}

static int vk_create_window_surface(void)
{
    return SDL_Vulkan_CreateSurface(s_vk.window, s_vk.instance, &s_vk.surface) ? 0 : -1;
}

static void vk_window_drawable_size(int* w, int* h)
{
    SDL_Vulkan_GetDrawableSize(s_vk.window, w, h);
}
#endif

static int vk_create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_vk.phys, s_vk.surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        VK_LOG("swapchain images cannot be blit destinations\n"); return -1;
    }

    u32 nf = 0;
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, NULL),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (nf == 0) { VK_LOG("surface reports no formats\n"); return -1; }
    if (nf > 64) nf = 64;
    VkSurfaceFormatKHR fmts[64];
    VK_CHECK(pvkGetPhysicalDeviceSurfaceFormatsKHR(s_vk.phys, s_vk.surface, &nf, fmts),
             "vkGetPhysicalDeviceSurfaceFormatsKHR");
    /* UNORM, not SRGB: the offscreen target holds the guest's values as-is. */
    VkSurfaceFormatKHR fmt = fmts[0];
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        static const VkFormat pref[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
        int found = 0;
        for (size_t k = 0; k < 2 && !found; k++)
            for (u32 i = 0; i < nf; i++)
                if (fmts[i].format == pref[k]) { fmt = fmts[i]; found = 1; break; }
    }
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        VK_LOG("swapchain format %d cannot be a blit destination\n", (int)fmt.format); return -1;
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        int w = 0, hh = 0;
        vk_window_drawable_size(&w, &hh);
        ext.width = (u32)w; ext.height = (u32)hh;
        if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    if (!ext.width || !ext.height) { VK_LOG("window has no drawable area\n"); return -1; }

    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;

    static const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
    };
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (size_t k = 0; k < 4; k++)
        if (caps.supportedCompositeAlpha & alphas[k]) { alpha = alphas[k]; break; }

    VkSwapchainKHR old = s_vk.swapchain;
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s_vk.surface, .minImageCount = count,
        .imageFormat = fmt.format, .imageColorSpace = fmt.colorSpace,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,   /* the one mode every driver has */
        .clipped = VK_TRUE, .oldSwapchain = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = pvkCreateSwapchainKHR(s_vk.device, &sci, NULL, &sc);
    if (old) pvkDestroySwapchainKHR(s_vk.device, old, NULL);
    s_vk.swapchain = VK_NULL_HANDLE;
    if (r != VK_SUCCESS) { VK_LOG("vkCreateSwapchainKHR failed (VkResult %d)\n", (int)r); return -1; }
    s_vk.swapchain = sc;

    u32 n = 0;
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, NULL), "vkGetSwapchainImagesKHR");
    if (n == 0 || n > 8) { VK_LOG("unsupported swapchain image count %u\n", n); return -1; }
    VK_CHECK(pvkGetSwapchainImagesKHR(s_vk.device, sc, &n, s_vk.sc_images), "vkGetSwapchainImagesKHR");

    for (u32 i = 0; i < 8; i++)
        if (s_vk.sc_done[i]) {
            pvkDestroySemaphore(s_vk.device, s_vk.sc_done[i], NULL);
            s_vk.sc_done[i] = VK_NULL_HANDLE;
        }
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (u32 i = 0; i < n; i++)
        VK_CHECK(pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sc_done[i]), "vkCreateSemaphore");

    s_vk.sc_extent = ext;
    s_vk.sc_count = n;
    VK_LOG("window swapchain %ux%u, %u images, format %d\n", ext.width, ext.height, n, (int)fmt.format);
    return 0;
}

static void vk_swapchain_lost(void)
{
    vk_device_idle();
    if (vk_create_swapchain()) vk_window_drop("the swapchain could not be recreated");
}

/* Blit the offscreen target into the next swapchain image and present it. */
static void vk_record_window_blit(VkCommandBuffer cb, u32 idx);

static void vk_present_window(void)
{
    if (!s_vk.windowed || !s_vk.swapchain) return;

    u32 idx = 0;
    VkResult r = vk_busy_acquire(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_vk.sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); return;
    }

    /* The present's own command buffer: free, since every caller has waited
     * for its read-back copy (present_fence) first. */
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) { VK_LOG("vkResetCommandBuffer (window) failed\n"); return; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { VK_LOG("vkBeginCommandBuffer (window) failed\n"); return; }
    vk_record_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer failed\n"); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sem_acquire,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx],
    };
    pvkResetFences(s_vk.device, 1, &s_vk.window_fence);
    if (vk_queue_submit(&si, s_vk.window_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (window) failed\n"); return;
    }
    vk_fence_wait(s_vk.window_fence);

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present(&pi);
    if (s_vk.fps_on && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
        metal_frame_rate_presented(&s_vk.fps, vk_now_seconds());
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
}

static double vk_now_seconds(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* present_mu held. Same line shape as the Metal backend's, plus the clock. */
/* display/guest count host presents (every vkQueuePresentKHR, so a frame shown
 * twice counts twice); guest_flips is the guest's own flip requests per second
 * over the same interval (cellGcm_flip_request_count): the frame rate the title
 * actually produced. */
u32 cellGcm_flip_request_count(void);
static void vk_fps_report(void)
{
    static u32 last_requests;
    static double last_requests_at;
    const double now = vk_now_seconds();
    if (now - s_vk.fps.epoch < 1.0) return;
    MetalFrameRateSample r = metal_frame_rate_sample(&s_vk.fps, now);
    const u32 requests = cellGcm_flip_request_count();
    const double flips = last_requests_at > 0.0 && now > last_requests_at
        ? (double)(u32)(requests - last_requests) / (now - last_requests_at) : 0.0;
    last_requests = requests; last_requests_at = now;
    fprintf(stderr, "[RSX FPS] display=%.2f guest=%.2f avg_ms=%.2f p95_ms=%.2f max_ms=%.2f age_s=%.2f clock=host_present_call window=%d guest_flips=%.2f\n",
            r.display_fps, r.guest_fps, r.average_ms, r.p95_ms, r.worst_ms, r.age_seconds, s_vk.windowed && s_vk.swapchain ? 1 : 0, flips);
}

static void vk_dump_ppm(const unsigned char* rgba)
{
    FILE* f = fopen(s_vk.dump_path, "wb");
    if (!f) { VK_LOG("cannot open %s for the frame dump\n", s_vk.dump_path); return; }
    fprintf(f, "P6\n%u %u\n255\n", s_vk.width, s_vk.height);
    for (size_t i = 0; i < (size_t)s_vk.width * s_vk.height; i++)
        fwrite(rgba + i * 4, 1, 3, f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * Present
 *
 * Direct by default on the engine path (see vk_present_readback_wanted):
 * one submission blits the flipped surface into the offscreen target and the
 * target into the window, and nothing waits. A read-back present comes in
 * two halves. The first records, on the present's own command buffer,
 * whatever puts the frame into the offscreen target (the engine's blit of
 * the flipped surface) and the copy of that target into `readback`, and
 * submits it without waiting. The second waits for it, reads the centre
 * pixel and the frame dump back, and blits the target into the window.
 *
 * The first half reads the frame, so it must not run while the walker is
 * recording; the second touches only the target, `readback` and the
 * present's own objects, none of which the engine path draws into, so a host
 * can run it on a thread of its own beside the walker
 * (rsx_vulkan_backend_present_begin / _finish). On the fixed-function path
 * the walker draws into the target itself, so there the second half always
 * follows the first at once.
 * -------------------------------------------------------------------------*/

/* Read-back presents (the original behaviour): every present copies the
 * whole frame into `readback`, waits for it, keeps its centre pixel, writes
 * PS3RECOMP_VK_DUMP and only then blits into the window and waits again --
 * the CPU waits for all GPU work, native included, once per frame. They
 * remain for the fixed-function path (its tests read the centre pixel back),
 * for the presents PS3RECOMP_VK_DUMP writes (every PS3RECOMP_VK_DUMP_EVERY'th)
 * and on request (PS3RECOMP_VK_PRESENT_READBACK=1).
 *
 * Otherwise a present is direct, as on Metal: the flipped surface's blit and
 * the window blit go into one command buffer that waits for the swapchain
 * image on the GPU and signals its present, and nothing waits on the CPU. A
 * ring of VK_PRESENT_SLOTS command buffers and fences keeps frames in flight;
 * a slot is reused only once its fence signalled. */
/* PS3RECOMP_VK_DEFER_HELD_PRESENT=1 (opt-in): a direct present made while a
 * reserved queue order holds original submissions does not wait for the
 * reservation to end on the presenting thread -- the thread that also drains
 * the FIFO. It is split in two:
 *  - the capture (the flipped surface blitted into the offscreen target) is
 *    recorded and submitted at once; vk_queue_submit queues it behind the held
 *    work, so it still shows exactly the frame that was flipped;
 *  - the show (acquire, blit into the swapchain image, vkQueuePresentKHR), which
 *    cannot go to the queue before the capture does, is left pending and made
 *    by vk_show_pump() from the host's message pump, on the same thread, as
 *    soon as the capture has left the hold. The end of a hold wakes the host
 *    thread (cellGcm_fifo_kick_notify) when a show is pending. The show reads
 *    only the offscreen target and writes only a swapchain image -- nothing a
 *    native batch or held original work touches -- so it is submitted and
 *    presented directly even if a newer reservation holds other work by then;
 *    it orders after its capture by queue order.
 * One show is pending at most: a later capture supersedes it (the offscreen
 * target then holds the newer frame), and so does an ordinary present. The
 * show uses its own ring of command buffers, fences and acquire semaphores.
 * A read-back present (a frame dump) is split the same way where the host
 * splits it: its completion -- the dump and the window present -- is left to
 * the pump instead of present_finish while the reservation is held.
 * Off, nothing here runs and presents are unchanged. */
#define VK_SHOW_SLOTS 3
static struct {
    int pending;                          /* present_mu */
    int readback_pending;                 /* a read-back present left to complete */
    VkFence capture;                      /* the pending show's capture submission */
    u32 slot;
    VkCommandBuffer cmds[VK_SHOW_SLOTS];
    VkFence fences[VK_SHOW_SLOTS];
    VkSemaphore acquire[VK_SHOW_SLOTS];
    int busy[VK_SHOW_SLOTS];
    unsigned long long deferred, served, superseded, failed;
} s_show;
static int vk_defer_held_present(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_DEFER_HELD_PRESENT"); on = e && !strcmp(e, "1"); }
    return on;
}
static int vk_hold_active(void)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const int active = s_hold.active;
    pthread_mutex_unlock(&s_vk.queue_mu);
    return active;
}
static void vk_show_kick(void)
{
    extern void cellGcm_fifo_kick_notify(void);
    if (__atomic_load_n(&s_show.pending, __ATOMIC_ACQUIRE)) cellGcm_fifo_kick_notify();
}
/* Whether the pending show's capture has reached the queue. */
static int vk_show_capture_queued(void)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const int held = vk_hold_has_locked(s_show.capture);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return !held;
}
/* A submission that orders only after work already on the queue: never held.
 * It is an original submission for the owner's accounting; while a
 * reservation holds other work it does not become the native boundary. */
static VkResult vk_queue_submit_unheld(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    vulkan_owner_submitted(r);
    if(vulkan_owner_enabled()&&r==VK_SUCCESS&&!s_hold.active&&s_vulkan_owner_thread.original_depth&&!s_vulkan_owner.failed){
        s_vulkan_boundary_fence=fence;
        s_vulkan_boundary_commit=s_vulkan_commits;
        s_vulkan_boundary_generation=s_vulkan_owner.generation;
    }
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}
/* Its semaphore's signal is already on the queue: no hold wait. */
static VkResult vk_queue_present_unheld(const VkPresentInfoKHR* pi)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueuePresentKHR(s_vk.queue, pi);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}
static void vk_show_report(void)
{
    const unsigned long long n = s_show.deferred;
    if (n <= 3 || n % 600 == 0)
        VK_LOG("deferred held present: deferred=%llu served=%llu superseded=%llu failed=%llu\n",
               n, s_show.served, s_show.superseded, s_show.failed);
}
/* present_mu held: a pending show will not be made -- a newer frame takes it. */
static void vk_show_supersede_locked(void)
{
    if (!s_show.pending) return;
    s_show.pending = 0;
    s_show.superseded++;
}

/* A fence of the direct-present ring or of the deferred show ring (see
 * rsx_vulkan_backend_native_boundary). */
static int vk_present_is_fence(VkFence f)
{
    if (!f) return 0;
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++) if (s_vk.present_fences[i] == f) return 1;
    for (u32 i = 0; i < VK_SHOW_SLOTS; i++) if (s_show.fences[i] == f) return 1;
    return 0;
}
static int vk_present_readback_wanted(void)
{
    static int forced = -1;
    if (forced < 0) { const char* e = getenv("PS3RECOMP_VK_PRESENT_READBACK"); forced = e && !strcmp(e, "1"); }
    /* A dump reads back only the presents it writes. */
    const int dump = s_vk.dump_path && ((s_vk.presented + 1u) % s_vk.dump_every) == 0;
    return forced || !s_vk.eng_active || dump;
}

/* Wait until the GPU is done with every present in flight (an image is about
 * to be destroyed). */
static void vk_present_wait_gpu(void)
{
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        if (__atomic_load_n(&s_vk.present_busy[i], __ATOMIC_ACQUIRE) && s_vk.present_fences[i])
            vk_fence_wait(s_vk.present_fences[i]);
    for (u32 i = 0; i < VK_SHOW_SLOTS; i++)
        if (__atomic_load_n(&s_show.busy[i], __ATOMIC_ACQUIRE) && s_show.fences[i])
            vk_fence_wait(s_show.fences[i]);
}

/* Wait only for the presents that may still read `img`: a ring slot whose
 * present blits that engine image, and every present and window show when it
 * is the backend's own colour image (all of them read it). Destroying any
 * other image does not wait on presentation: before, each one waited for
 * every present in flight, a CPU stall per released texture or target. */
static void vk_present_wait_image(VkImage img)
{
    if (img == s_vk.color.img) { vk_present_wait_gpu(); return; }
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        if (__atomic_load_n(&s_vk.present_busy[i], __ATOMIC_ACQUIRE) && s_vk.present_fences[i] &&
            s_vk.present_src[i] == img)
            vk_fence_wait(s_vk.present_fences[i]);
}

/* present_mu held: finish a read-back present -- wait for its copy, keep the
 * centre pixel and the dump, then show it in the window. */
static void vk_present_complete_locked(void)
{
    if (!s_vk.present_inflight) return;
    vk_fence_wait(s_vk.present_fence);
    __atomic_store_n(&s_vk.present_busy[s_vk.present_slot], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vk.present_inflight, 0, __ATOMIC_RELEASE);

    vk_invalidate(&s_vk.readback);
    const unsigned char* px = (const unsigned char*)s_vk.readback.ptr;
    size_t c = ((size_t)(s_vk.height / 2) * s_vk.width + s_vk.width / 2) * 4;
    s_vk.last_center = ((u32)px[c] << 16) | ((u32)px[c + 1] << 8) | (u32)px[c + 2];
    s_vk.presented++;
    s_vk.present_readback_count++;
    if (s_vk.dump_path && (s_vk.presented % s_vk.dump_every) == 0) vk_dump_ppm(px);
    if (s_vk.fps_on) metal_frame_rate_guest(&s_vk.fps);
    vk_present_window();
    if (s_vk.fps_on) vk_fps_report();
}

/* present_mu held: a previous read-back present finished, the next ring
 * slot's earlier present complete, and its command buffer begun after
 * everything submitted so far. */
static VkCommandBuffer vk_present_record_begin(void)
{
    uint64_t tp = s_vkpt_on > 0 ? vkpt_now() : 0;
    vk_present_complete_locked();
    vkpt_add(VKPT_COMPLETE, tp);
    const u32 slot = (s_vk.present_slot + 1u) % VK_PRESENT_SLOTS;
    tp = s_vkpt_on > 0 ? vkpt_now() : 0;
    if (s_vk.present_busy[slot]) {
        vk_fence_wait(s_vk.present_fences[slot]);
        __atomic_store_n(&s_vk.present_busy[slot], 0, __ATOMIC_RELEASE);
    }
    vkpt_add(VKPT_SLOT, tp);
    s_vk.present_slot = slot;
    s_vk.present_src[slot] = VK_NULL_HANDLE;   /* set by a present that blits an engine image */
    s_vk.present_cmd = s_vk.present_cmds[slot];
    s_vk.present_fence = s_vk.present_fences[slot];
    VkCommandBuffer cb = s_vk.present_cmd;
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_cmd_after_everything(cb);
    return cb;
}

/* Record the blit of the offscreen target into swapchain image `idx`, from
 * GENERAL to PRESENT_SRC. */
static void vk_record_window_blit(VkCommandBuffer cb, u32 idx)
{
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s_vk.sc_images[idx], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 },
                        { (int32_t)s_vk.sc_extent.width, (int32_t)s_vk.sc_extent.height, 1 } },
    };
    pvkCmdBlitImage(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

/* present_mu held: end the present's recording and submit it -- read-back
 * (copy out, finished at once unless a host asked for the halves separately
 * on the engine path) or direct (window blit included, no CPU wait). */
static void vk_present_record_end(VkCommandBuffer cb)
{
    const u32 slot = s_vk.present_slot;
    /* Deferred: capture now behind the held work, show from the pump. */
    const int defer = vk_defer_held_present() && !vk_present_readback_wanted() && vk_hold_active();
    if (vk_defer_held_present() && !defer) vk_show_supersede_locked();
    if (!vk_present_readback_wanted()) {
        u32 idx = 0;
        int window = s_vk.windowed && s_vk.swapchain && !defer;
        VkSemaphore acquire = VK_NULL_HANDLE;
        if (window) {
            if (!s_vk.present_acquire[slot]) {
                VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.present_acquire[slot]) != VK_SUCCESS)
                    s_vk.present_acquire[slot] = VK_NULL_HANDLE;
            }
            acquire = s_vk.present_acquire[slot];
            const uint64_t ta = s_vkpt_on > 0 ? vkpt_now() : 0;
            VkResult r = acquire ? vk_busy_acquire(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                                          acquire, VK_NULL_HANDLE, &idx)
                                 : VK_ERROR_INITIALIZATION_FAILED;
            vkpt_add(VKPT_ACQUIRE, ta);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); window = 0; }
            else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                VK_LOG("vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r); window = 0;
            }
            if (window) vk_record_window_blit(cb, idx);
        }
        if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .waitSemaphoreCount = window ? 1u : 0u, .pWaitSemaphores = &acquire,
                            .pWaitDstStageMask = &wait_stage,
                            .commandBufferCount = 1, .pCommandBuffers = &cb,
                            .signalSemaphoreCount = window ? 1u : 0u,
                            .pSignalSemaphores = window ? &s_vk.sc_done[idx] : NULL };
        pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
        const uint64_t tsub = s_vkpt_on > 0 ? vkpt_now() : 0;
        if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
            VK_LOG("vkQueueSubmit (present) failed\n"); return;
        }
        vkpt_add(VKPT_SUBMIT, tsub);
        __atomic_store_n(&s_vk.present_busy[slot], 1, __ATOMIC_RELEASE);
        s_vk.present_order = 1;
        s_vk.presented++;
        s_vk.present_direct++;
        if (s_vk.fps_on) metal_frame_rate_guest(&s_vk.fps);
        if (defer) {
            if (s_show.pending) s_show.superseded++;
            s_show.capture = s_vk.present_fence;
            __atomic_store_n(&s_show.pending, 1, __ATOMIC_RELEASE);
            s_show.deferred++;
            vk_show_report();
        }
        if (window) {
            VkPresentInfoKHR pi = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
                .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
            };
            const uint64_t tqp = s_vkpt_on > 0 ? vkpt_now() : 0;
            VkResult r = vk_queue_present(&pi);
            vkpt_add(VKPT_QPRESENT, tqp);
            if (s_vk.fps_on && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
                metal_frame_rate_presented(&s_vk.fps, vk_now_seconds());
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
            else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
        }
        if (s_vk.fps_on) vk_fps_report();
        return;
    }
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s_vk.width, s_vk.height, 1 },
    };
    pvkCmdCopyImageToBuffer(cb, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.readback.buf, 1, &region);
    VkBufferMemoryBarrier host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vk.readback.buf, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &host, 0, NULL);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (present) failed\n"); return; }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vk.present_fence);
    if (vk_queue_submit(&si, s_vk.present_fence) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (present) failed\n"); return;
    }
    __atomic_store_n(&s_vk.present_busy[slot], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_vk.present_inflight, 1, __ATOMIC_RELEASE);
    s_vk.present_order = 1;
    if (!(s_vk.present_split && s_vk.eng_active)) vk_present_complete_locked();
}

/* present_mu held: make the pending show once no reservation holds original
 * submissions. Returns 1 when the pending show was made (or had no window to
 * reach), 0 when it is still held or nothing was pending. */
static int vk_show_service_locked(void)
{
    if (s_show.readback_pending && !vk_hold_active()) {
        s_show.readback_pending = 0;
        if (!s_vk.present_inflight) return 0;    /* the next present completed it already */
        vk_present_complete_locked();            /* the dump and the window present */
        s_show.served++;
        return 1;
    }
    if (!s_show.pending || !vk_show_capture_queued()) return 0;
    s_show.pending = 0;
    if (!(s_vk.windowed && s_vk.swapchain)) { s_show.served++; return 1; }
    const u32 slot = (s_show.slot + 1u) % VK_SHOW_SLOTS;
    if (!s_show.cmds[slot]) {
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = s_vk.present_pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkAllocateCommandBuffers(s_vk.device, &cai, &s_show.cmds[slot]) != VK_SUCCESS ||
            pvkCreateFence(s_vk.device, &fci, NULL, &s_show.fences[slot]) != VK_SUCCESS ||
            pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_show.acquire[slot]) != VK_SUCCESS) {
            VK_LOG("deferred held present: show slot %u could not be created\n", slot);
            s_show.failed++; return 0;
        }
    }
    if (s_show.busy[slot]) {
        vk_fence_wait(s_show.fences[slot]);
        __atomic_store_n(&s_show.busy[slot], 0, __ATOMIC_RELEASE);
    }
    s_show.slot = slot;
    u32 idx = 0;
    VkResult r = vk_busy_acquire(s_vk.device, s_vk.swapchain, UINT64_MAX,
                                        s_show.acquire[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); s_show.failed++; return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_LOG("vkAcquireNextImageKHR (deferred show) failed (VkResult %d)\n", (int)r); s_show.failed++; return 0;
    }
    VkCommandBuffer cb = s_show.cmds[slot];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOG("deferred held present: show recording failed\n"); s_show.failed++; return 0;
    }
    vk_cmd_after_everything(cb);                 /* after the capture it shows */
    vk_record_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("vkEndCommandBuffer (deferred show) failed\n"); s_show.failed++; return 0; }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_show.acquire[slot],
                        .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = 1, .pSignalSemaphores = &s_vk.sc_done[idx] };
    pvkResetFences(s_vk.device, 1, &s_show.fences[slot]);
    if (vk_queue_submit_unheld(&si, s_show.fences[slot]) != VK_SUCCESS) {
        VK_LOG("vkQueueSubmit (deferred show) failed\n"); s_show.failed++; return 0;
    }
    __atomic_store_n(&s_show.busy[slot], 1, __ATOMIC_RELEASE);
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
        .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
    };
    r = vk_queue_present_unheld(&pi);
    if (s_vk.fps_on && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR))
        metal_frame_rate_presented(&s_vk.fps, vk_now_seconds());
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
    else if (r != VK_SUCCESS) VK_LOG("vkQueuePresentKHR (deferred show) failed (VkResult %d)\n", (int)r);
    s_show.served++;
    return 1;
}

/* From the host's message pump: make a pending deferred show. Off, or with
 * nothing pending, it returns at once. */
static void vk_show_pump(void)
{
    if (!vk_defer_held_present() || !s_vk.device) return;
    if (!__atomic_load_n(&s_show.pending, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s_show.readback_pending, __ATOMIC_ACQUIRE)) return;
    if (!vulkan_owner_begin(1)) return;
    const uint64_t before = s_vulkan_commits;
    pthread_mutex_lock(&s_vk.present_mu);
    (void)vk_show_service_locked();
    pthread_mutex_unlock(&s_vk.present_mu);
    vulkan_owner_finish(vulkan_owner_boundary(before));
}

static void vk_cb_present(void* ud, u32 buffer_id)
{
    (void)ud; (void)buffer_id;
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) vk_present_record_end(cb);
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* ---------------------------------------------------------------------------
 * Register-file draw engine backend (rsx_draw_engine.h)
 *
 * The path the Metal backend takes: the shared engine walks the register
 * file, owns surfaces, texture caching, vertex compaction and pipeline keys,
 * and drives this backend through rsx_draw_backend. Selected with
 * PS3RECOMP_RSX_ENGINE=dispatch; only one of the two paths is registered at a
 * time, since the FIFO walker feeds both and would record every draw twice.
 *
 * Every operation is still one synchronous submission, so submit_and_wait has
 * nothing left to do by the time the engine calls it -- which is exactly the
 * contract it asks for (staging is free to reuse on return).
 *
 * Stage E1: colour and depth targets, clears, present and readback.
 * Pipelines, draws and textures are the next stages; until then
 * pipeline_create reports 0, which the engine caches as "cannot build".
 * -------------------------------------------------------------------------*/
#define VK_ENG_MAX_OBJ 2048          /* the engine's texture cache is 1024 */

enum { VK_ENG_FREE = 0, VK_ENG_COLOR, VK_ENG_DEPTH, VK_ENG_TEXTURE, VK_ENG_VIEW };

typedef struct vk_eng_obj {
    u8       kind;
    VkFormat fmt;
    u32      bpp;                    /* bytes per texel, 0 for depth     */
    u32      w, h;
    vk_image im;                     /* a VIEW owns only im.view            */
    u32      block;                  /* bytes per 4x4 block, 0 = not BC     */
    u32      surface, remap, rsx_fmt;/* VIEW: what it is a view of          */
    u8       cube;                   /* TEXTURE: six layers, a CUBE view    */
    u8       snapshot;               /* COLOR: a feedback copy, never a target */
} vk_eng_obj;

static vk_eng_obj s_eobj[VK_ENG_MAX_OBJ];
static uint64_t s_eobj_generation[VK_ENG_MAX_OBJ],s_eobj_next_generation;
static uint32_t s_eobj_native_pins[VK_ENG_MAX_OBJ];
static uint8_t s_eobj_retired[VK_ENG_MAX_OBJ];
/* PS3RECOMP_VK_PIN_RELEASE_NOWAIT=1 (opt-in): the original release of an
 * image the native side still pins neither flushes-and-waits nor waits for
 * presentation. It submits the open batch without waiting, ending in a full
 * dependency on everything submitted before it, and keeps that batch's ticket
 * with the image. When the last native pin goes (after the native side's own
 * fences), the image is destroyed at once if that ticket has completed, or
 * parked on a bounded list that the completion reap destroys later. With no
 * room left on the list, the release waits as before. */
#define VK_PIN_RETIRE 64u
typedef struct { vk_image im; u64 ticket; } vk_pin_retired;
static vk_pin_retired s_pin_retire[VK_PIN_RETIRE];
static u32 s_pin_retire_n, s_pin_retire_reserved;
static pthread_mutex_t s_pin_retire_mu = PTHREAD_MUTEX_INITIALIZER;
static u64 s_eobj_pin_ticket[VK_ENG_MAX_OBJ];
static unsigned long long s_pin_nowait, s_pin_fallback, s_pin_immediate, s_pin_deferred, s_pin_destroyed;
static int vk_pin_release_nowait(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_PIN_RELEASE_NOWAIT"); on = e && !strcmp(e, "1"); }
    return on;
}
/* Destroy without the presentation wait: the caller has established that no
 * GPU work may still use the image. */
static void vk_image_destroy_unused(vk_image* im)
{
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
}
static void vk_pin_retire_reap(u64 completed, int all)
{
    pthread_mutex_lock(&s_pin_retire_mu);
    for (u32 i = 0; i < s_pin_retire_n; ) {
        if (all || s_pin_retire[i].ticket <= completed) {
            vk_image_destroy_unused(&s_pin_retire[i].im);
            s_pin_retire[i] = s_pin_retire[--s_pin_retire_n];
            s_pin_destroyed++;
        } else i++;
    }
    pthread_mutex_unlock(&s_pin_retire_mu);
}
static rsx_native_vulkan_image s_eobj_native_image[VK_ENG_MAX_OBJ];
/* The last asynchronous ticket that reads or writes each engine image (E5);
 * 0 when none can still be pending. */
static u64 s_e5_obj_ticket[VK_ENG_MAX_OBJ];
static int vk_e5_defer_destroy(u32 handle);
static void vk_e5_shutdown(void);

/* Defined with the E2 pipeline state further down. */
static void vk_eng_drop_framebuffers(void);
static void vk_eng_drop_framebuffers_for(u32 handle);
static void vk_eng2_shutdown(void* user);

static struct {
    u32 rt[RSX_BE_MAX_COLOR_TARGETS], nrt, depth;
} s_eng_bound;

static VkFormat vk_eng_format(rsx_be_format f, u32* bpp)
{
    switch (f) {
    case RSX_BE_FMT_R8:            *bpp = 1;  return VK_FORMAT_R8_UNORM;
    case RSX_BE_FMT_R8G8:          *bpp = 2;  return VK_FORMAT_R8G8_UNORM;
    case RSX_BE_FMT_R8G8B8A8:      *bpp = 4;  return VK_FORMAT_R8G8B8A8_UNORM;
    case RSX_BE_FMT_R16:           *bpp = 2;  return VK_FORMAT_R16_UNORM;
    case RSX_BE_FMT_R16G16:        *bpp = 4;  return VK_FORMAT_R16G16_UNORM;
    case RSX_BE_FMT_R16G16F:       *bpp = 4;  return VK_FORMAT_R16G16_SFLOAT;
    case RSX_BE_FMT_R16G16B16A16F: *bpp = 8;  return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RSX_BE_FMT_R32F:          *bpp = 4;  return VK_FORMAT_R32_SFLOAT;
    case RSX_BE_FMT_R32G32B32A32F: *bpp = 16; return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RSX_BE_FMT_BC1:           *bpp = 8;  return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  /* per block */
    case RSX_BE_FMT_BC2:           *bpp = 16; return VK_FORMAT_BC2_UNORM_BLOCK;
    case RSX_BE_FMT_BC3:           *bpp = 16; return VK_FORMAT_BC3_UNORM_BLOCK;
    default:                       *bpp = 0;  return VK_FORMAT_UNDEFINED;
    }
}

/* Colour formats the E2 passes render into: RGBA8 for ordinary and display
 * targets, RGBA16F for SET_SURFACE_FORMAT 0xB HDR targets, and R32F / RGBA32F
 * for the float targets a title renders small reductions into. All four are
 * required colour attachment formats in Vulkan 1.0; blending is guaranteed only
 * for the first two (see vk_eng_rt_blendable). */
#define VK_ENG_RT_FORMATS 4
static int vk_eng_rt_index(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:      return 0;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 1;
    case VK_FORMAT_R32_SFLOAT:          return 2;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 3;
    default:                            return -1;
    }
}

/* Whether the device blends into colour format `f` (guaranteed for RGBA8 and
 * RGBA16F, optional for 32-bit float formats). */
static int vk_eng_rt_blendable(VkFormat f)
{
    if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R16G16B16A16_SFLOAT) return 1;
    VkFormatProperties fp;
    pvkGetPhysicalDeviceFormatProperties(s_vk.phys, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
}

/* The largest image side the engine creates: the device's own limit once a
 * device is picked, the 4096 Vulkan guarantees before. */
static u32 vk_max_dim(void) { return s_vk.max_dim ? s_vk.max_dim : 4096u; }

static u32 vk_eng_alloc(void)
{
    /* Hand out slots round-robin rather than lowest-first, so a handle the
     * engine releases is not immediately reused: a late or duplicated release
     * of an old handle then finds a free slot instead of a live object. */
    static u32 cursor;
    for (u32 step = 0; step < VK_ENG_MAX_OBJ - 1; step++) {
        const u32 i = 1 + (cursor + step) % (VK_ENG_MAX_OBJ - 1);
        if (s_eobj[i].kind == VK_ENG_FREE && !s_eobj_native_pins[i]) {
            cursor = i % (VK_ENG_MAX_OBJ - 1);
            if(vulkan_owner_enabled()) {
                if(s_eobj_next_generation==UINT64_MAX){vulkan_owner_fault();return 0;}
                s_eobj_generation[i]=++s_eobj_next_generation;
            }
            memset(&s_eobj_native_image[i],0,sizeof s_eobj_native_image[i]);
            s_eobj_retired[i]=0;
            s_e5_obj_ticket[i]=0;
            return i;
        }
    }
    VK_LOG("engine: object table full (%d)\n", VK_ENG_MAX_OBJ);
    return 0;
}

static vk_eng_obj* vk_eng_get(u32 handle, int kind)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj_retired[handle]) return NULL;
    vk_eng_obj* o = &s_eobj[handle];
    return (o->kind == kind) ? o : NULL;
}

/* Copy `rows` rows of `w` texels from host memory into a w x h rectangle at
 * (x, y) of one level / layer of an image that is in GENERAL. */
static int vk_eng_upload_region(VkImage img, u32 layer, u32 mip, u32 x, u32 y,
                                u32 w, u32 h, size_t tight, u32 rows,
                                const void* src, u32 row_bytes)
{
    if (tight > row_bytes) tight = row_bytes;
    vk_buffer st = {0};
    if (vk_stage_acquire(&st, (VkDeviceSize)tight * rows)) return -1;
    for (u32 y = 0; y < rows; y++)
        memcpy((u8*)st.ptr + (size_t)y * tight, (const u8*)src + (size_t)y * row_bytes, tight);
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 },
            .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        rc = vk_submit_and_wait();
    }
    vk_stage_release(&st);
    return rc;
}

/* The same into the whole of a level, from its origin. */
static int vk_eng_upload_rows(VkImage img, u32 layer, u32 mip, u32 w, u32 h,
                              size_t tight, u32 rows, const void* src, u32 row_bytes)
{
    return vk_eng_upload_region(img, layer, mip, 0, 0, w, h, tight, rows, src, row_bytes);
}

static int vk_eng_upload(VkImage img, u32 layer, u32 mip, u32 w, u32 h, u32 bpp,
                         const void* src, u32 row_bytes)
{
    return vk_eng_upload_rows(img, layer, mip, w, h, (size_t)w * bpp, h, src, row_bytes);
}

/* NV4097 TEXTURE_CONTROL1 crossbar as a Vulkan component mapping: the shared
 * rsx_texture_component_remap() gives, in A,R,G,B order, which uploaded
 * channel (0..3 = R,G,B,A) or constant each output takes -- the table the
 * Metal backend's swizzle_sel reads. */
static VkComponentSwizzle vk_remap_sel(u8 sel)
{
    switch (sel) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case RSX_REMAP_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_ZERO;
    }
}
static VkComponentMapping vk_remap(u32 remap, u32 rsx_fmt)
{
    u8 sel[4];
    rsx_texture_component_remap(remap, rsx_fmt & 0x9Fu, sel);
    VkComponentMapping m = { vk_remap_sel(sel[1]), vk_remap_sel(sel[2]),
                             vk_remap_sel(sel[3]), vk_remap_sel(sel[0]) };
    return m;
}
static int vk_eng_make_view(VkImage img, VkFormat fmt, u32 levels, VkComponentMapping map,
                            int cube, VkImageView* out)
{
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt, .components = map,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels ? levels : 1, 0, cube ? 6u : 1u },
    };
    return pvkCreateImageView(s_vk.device, &vci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

static void vk_pt_start(void);
static void vk_pt_stop(void);
static int vk_eng_init(void* user, u32 width, u32 height)
{
    (void)user; (void)width; (void)height;
    if(vulkan_owner_enabled()&&s_vulkan_owner.retained)return -1;
    memset(s_eobj, 0, sizeof s_eobj);
    memset(s_eobj_generation,0,sizeof s_eobj_generation);
    memset(s_eobj_native_pins,0,sizeof s_eobj_native_pins);
    memset(s_eobj_retired,0,sizeof s_eobj_retired);
    memset(s_eobj_native_image,0,sizeof s_eobj_native_image);
    memset(&s_eng_bound, 0, sizeof s_eng_bound);
    memset(s_e5_obj_ticket, 0, sizeof s_e5_obj_ticket);
    atomic_store(&s_vk_eng_failed, 0);
    if (!s_vk.device) return -1;
    if (!s_bat_on && vk_bat_init()) {
        VK_LOG("engine: batched recording could not start; every operation stays one waited submission\n");
        vk_bat_shutdown();
        s_bat_on = 0;
    }
    if (rsx_present_thread_requested()) vk_pt_start();
    return 0;
}

static void vk_eng_snapshot_recycle(vk_eng_obj* o);

static void vk_eng_release(void* user, u32 handle)
{
    (void)user;
    if (!handle || handle >= VK_ENG_MAX_OBJ || s_eobj[handle].kind == VK_ENG_FREE || s_eobj_retired[handle]) return;
    if (s_eobj[handle].kind == VK_ENG_COLOR)
        for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
            if (s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == handle) {
                vk_destroy_image(&s_eobj[i].im);
                memset(&s_eobj[i], 0, sizeof s_eobj[i]);
            }
    if (s_eobj[handle].kind == VK_ENG_DEPTH && s_eobj[handle].surface) {
        const u32 snap = s_eobj[handle].surface;
        s_eobj[handle].surface = 0;
        if (vk_eng_get(snap, VK_ENG_TEXTURE)) vk_eng_release(user, snap);
    }
    if ((s_eobj[handle].kind == VK_ENG_COLOR && !s_eobj[handle].snapshot) || s_eobj[handle].kind == VK_ENG_DEPTH)
        vk_eng_drop_framebuffers_for(handle);
    if(s_eobj_native_pins[handle]) {
        /* PS3RECOMP_RSX_TARGET_CHURN=1: count these waited releases and their
         * wall time (diagnostic). */
        static int churn = -1;
        if (churn < 0) { const char* e = getenv("PS3RECOMP_RSX_TARGET_CHURN"); churn = e && !strcmp(e, "1"); }
        const double t0 = churn ? vk_now_seconds() : 0.0;
        if (vk_pin_release_nowait() && s_bat_on) {
            pthread_mutex_lock(&s_pin_retire_mu);
            const int room = s_pin_retire_n + s_pin_retire_reserved < VK_PIN_RETIRE;
            if (room) s_pin_retire_reserved++;
            pthread_mutex_unlock(&s_pin_retire_mu);
            /* An empty batch still ends in the full dependency, so its ticket
             * covers every earlier submission, presents included. */
            if (room && !vk_bat_open() && !vk_bat_flush(0) && s_bat_last_ticket) {
                s_eobj_pin_ticket[handle] = s_bat_last_ticket;
                s_eobj_retired[handle] = 1;
                s_pin_nowait++;
                if (churn) {
                    static double last; const double t1 = vk_now_seconds();
                    if (!last) last = t1;
                    if (t1 - last >= 2.0) {
                        fprintf(stderr, "[target churn] pinned releases without waiting: %llu, waited fallbacks %llu, "
                                "destroyed at last pin %llu, parked %llu, destroyed by reap %llu, parked now %u\n",
                                s_pin_nowait, s_pin_fallback, s_pin_immediate, s_pin_deferred, s_pin_destroyed, s_pin_retire_n);
                        last = t1;
                    }
                }
                return;
            }
            if (room) { pthread_mutex_lock(&s_pin_retire_mu); s_pin_retire_reserved--; pthread_mutex_unlock(&s_pin_retire_mu); }
            s_pin_fallback++;
        }
        /* Batched: the native side destroys it after its own fences, which
         * do not cover original work recorded after its boundary; finish
         * that first. */
        if (s_bat_on) (void)vk_bat_flush(1);
        /* Match the original release's presentation drain now. Native pins
         * outlive genuine native fences; their final callback must not wait. */
        vk_present_wait_gpu();
        s_eobj_retired[handle]=1;
        if (churn) {
            static unsigned long long n; static double ms, last;
            const double t1 = vk_now_seconds(); n++; ms += (t1 - t0) * 1e3;
            if (!last) last = t1;
            if (t1 - last >= 2.0) {
                fprintf(stderr, "[target churn] vulkan pinned releases: %llu in %.1f s, waited %.1f ms total (%.2f ms each)\n",
                        n, t1 - last, ms, n ? ms / (double)n : 0.0);
                n = 0; ms = 0; last = t1;
            }
        }
        return;
    }
    if (s_eobj[handle].kind == VK_ENG_COLOR && s_eobj[handle].snapshot) {
        /* Never an attachment, so no framebuffer names it; the image goes
         * back to the pool for the next feedback draw. */
        vk_eng_snapshot_recycle(&s_eobj[handle]);
        memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
        return;
    }
    /* Idle once every submission that used it is waited on; an image an
     * asynchronous ticket still reads or writes is destroyed when it retires. */
    if (!vk_e5_defer_destroy(handle)) vk_destroy_image(&s_eobj[handle].im);
    memset(&s_eobj[handle], 0, sizeof s_eobj[handle]);
}

static void vk_eng_snapshot_pool_drain(void);

static void vk_eng_shutdown(void* user)
{
    vk_pt_stop();   /* joins the show thread before anything it uses goes */
    if (s_bat_on) (void)vk_bat_flush(1);
    vk_eng2_shutdown(user);
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++) vk_eng_release(user, i);
    vk_eng_snapshot_pool_drain();
    vk_bat_shutdown();
    vk_e5_shutdown();
}

static void vk_eng_submit_and_wait(void* user, u32 reason)
{
    (void)user; (void)reason;      /* default path submissions already completed */
    /* Batched: submit (an empty batch if nothing is open, so the owner sees
     * a real submitted boundary, as the per-operation path makes one) and wait;
     * the engine reuses its staging the moment this returns. */
    if (s_bat_on) { if (vk_bat_begin_op() || vk_bat_flush(1)) vulkan_owner_fault(); return; }
    if(vulkan_owner_enabled() && (vk_begin() || vk_submit_and_wait()))vulkan_owner_fault();
}

static u32 vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    /* Only formats the E2 passes are built for (vk_eng_rt_index). */
    if (vk_eng_rt_index(vf) < 0 || !w || !h || w > vk_max_dim() || h > vk_max_dim()) {
        VK_LOG("engine: colour target format %d %ux%u not supported yet\n", (int)fmt, w, h);
        return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image_pooled(&o->im, vf, w, h,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_COLOR; o->fmt = vf; o->bpp = bpp; o->w = w; o->h = h;
    /* UNDEFINED -> GENERAL, then the guest's own bytes when the engine could
     * resolve them, else transparent black so the contents are defined. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue zero = { .float32 = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    if (seed && seed_row_bytes)
        vk_eng_upload(o->im.img, 0, 0, w, h, bpp, seed, seed_row_bytes);
    return hd;
}

static u32 vk_eng_depth_target_create(void* user, u32 w, u32 h)
{
    (void)user;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim()) return 0;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageUsageFlags usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if(vulkan_owner_enabled()) {
        VkFormatProperties properties;pvkGetPhysicalDeviceFormatProperties(s_vk.phys,s_vk.depth_format,&properties);
        if(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)usage|=VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (vk_create_image(&o->im, s_vk.depth_format, w, h,usage,s_vk.depth_aspect)) {
        vk_destroy_image(&o->im); return 0;
    }
    o->kind = VK_ENG_DEPTH; o->fmt = s_vk.depth_format; o->w = w; o->h = h;
    /* Far plane, for the reason the display depth starts there. */
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static void vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth)
{
    (void)user;
    if (count > RSX_BE_MAX_COLOR_TARGETS) count = RSX_BE_MAX_COLOR_TARGETS;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        s_eng_bound.rt[i] = (i < count && surfaces) ? surfaces[i] : 0;
    s_eng_bound.nrt = count;
    s_eng_bound.depth = depth;
}

static void vk_eng_clear_color(void* user, u32 surface, const float rgba[4])
{
    (void)user;
    /* The debug hook reports the last colour asked for, as Metal's does. */
    s_vk.clear_argb = ((u32)(rgba[3] * 255.0f + 0.5f) << 24) |
                      ((u32)(rgba[0] * 255.0f + 0.5f) << 16) |
                      ((u32)(rgba[1] * 255.0f + 0.5f) <<  8) |
                       (u32)(rgba[2] * 255.0f + 0.5f);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || vk_begin()) return;
    VkClearColorValue v;
    memcpy(v.float32, rgba, sizeof v.float32);
    VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearColorImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    vk_submit_and_wait();
}

static void vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(depth, VK_ENG_DEPTH);
    VkImageAspectFlags aspect = 0;
    if (flags & RSX_BE_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & RSX_BE_CLEAR_STENCIL) aspect |= s_vk.depth_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!o || !aspect || vk_begin()) return;
    VkClearDepthStencilValue d = { depth_value, stencil };
    VkImageSubresourceRange r = { aspect, 0, 1, 0, 1 };
    vk_barrier_image(o->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
    pvkCmdClearDepthStencilImage(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, &d, 1, &r);
    vk_submit_and_wait();
}

/* ---------------------------------------------------------------------------
 * Opt-in present thread (PS3RECOMP_RSX_PRESENT_THREAD=1)
 *
 * rsx_present_thread.h owns the ring, the thread and the counters; these are
 * the Vulkan halves. Direct presents only: a read-back present (a frame dump,
 * PS3RECOMP_VK_PRESENT_READBACK=1) or the fixed-function path keeps the
 * ordinary present, with a one-time message.
 *
 * Stage (presenting thread, inside the owner gate like vk_eng_present): the
 * flipped surface blitted into this slot's staging image, on a command buffer
 * of the presenting thread's own pool, after a full dependency on everything
 * submitted before, through vk_queue_submit (so it is held behind a
 * reservation exactly as the present's copy was). present_order then makes the
 * walker's next command buffer wait for it, so no later write reaches the
 * surface before the copy has read it. Nothing waits for a swapchain image.
 *
 * Show (the present thread): the swapchain image (finite timeouts, so a
 * shutdown is never stuck behind it), then on its own pool the staging image
 * into the offscreen target and the offscreen target into the swapchain image
 * (vk_record_window_blit), then vkQueuePresentKHR. It submits only after the
 * stage it shows has left any hold (queue order), through queue_mu alone: no
 * present_mu, no owner gate or consumer lease, and not as the original owner's
 * submission. In this mode only the show thread uses the offscreen target, the
 * swapchain and its semaphores; the walker's engine path never draws into
 * them. The frame-rate counters, which both halves update, take a mutex of
 * their own.
 *
 * Off (the default), none of this is created and vk_eng_present is unchanged. */
#define VK_PT_STAGE_CMDS 6
#define VK_PT_SHOW_SLOTS 3
static struct {
    rsx_present_thread* pt;
    int stopping;                                   /* atomic */
    VkCommandPool stage_pool;                       /* presenting thread */
    VkCommandBuffer stage_cmds[VK_PT_STAGE_CMDS];
    VkFence stage_fences[VK_PT_STAGE_CMDS];
    int stage_busy[VK_PT_STAGE_CMDS];
    u32 stage_next;
    vk_image img[RSX_PRESENT_THREAD_MAX_DEPTH];     /* the staging images */
    u32 img_w[RSX_PRESENT_THREAD_MAX_DEPTH], img_h[RSX_PRESENT_THREAD_MAX_DEPTH];
    int img_fresh[RSX_PRESENT_THREAD_MAX_DEPTH];    /* still UNDEFINED */
    VkFence slot_fence[RSX_PRESENT_THREAD_MAX_DEPTH];
    VkCommandPool show_pool;                        /* present thread */
    VkCommandBuffer show_cmds[VK_PT_SHOW_SLOTS];
    VkFence show_fences[VK_PT_SHOW_SLOTS];
    VkSemaphore show_acquire[VK_PT_SHOW_SLOTS];
    int show_busy[VK_PT_SHOW_SLOTS];
    u32 show_next;
    pthread_mutex_t fps_mu;
} s_vpt;

static int vk_pt_is_stage_fence(VkFence f)
{
    if (!s_vpt.pt || !f) return 0;
    for (u32 i = 0; i < VK_PT_STAGE_CMDS; i++) if (s_vpt.stage_fences[i] == f) return 1;
    return 0;
}

static void vk_pt_destroy_image(u32 slot)
{
    vk_image* im = &s_vpt.img[slot];
    if (im->view) pvkDestroyImageView(s_vk.device, im->view, NULL);
    if (im->img)  pvkDestroyImage(s_vk.device, im->img, NULL);
    if (im->mem)  pvkFreeMemory(s_vk.device, im->mem, NULL);
    memset(im, 0, sizeof *im);
    s_vpt.img_w[slot] = s_vpt.img_h[slot] = 0;
}

static int vk_pt_stage(void* user, uint32_t slot, uint32_t surface)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || slot >= RSX_PRESENT_THREAD_MAX_DEPTH) return 0;
    /* The offscreen target's size, which the window blit reads. A slot whose
     * image no longer matches gets a new one; the queue goes idle first, since
     * a show submitted earlier may still read the old one. */
    if (s_vpt.img[slot].img && (s_vpt.img_w[slot] != s_vk.width || s_vpt.img_h[slot] != s_vk.height)) {
        vk_device_idle();
        vk_pt_destroy_image(slot);
    }
    if (!s_vpt.img[slot].img) {
        /* Only ever a blit source and destination, but vk_create_image also
         * makes a view, and a view needs a usage it can serve (VUID-
         * VkImageViewCreateInfo-image-04441): SAMPLED, as the offscreen
         * target's own images have, costs nothing here. */
        if (vk_create_image(&s_vpt.img[slot], s_vk.color.format, s_vk.width, s_vk.height,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_pt_destroy_image(slot);
            return 0;
        }
        s_vpt.img_w[slot] = s_vk.width; s_vpt.img_h[slot] = s_vk.height;
        s_vpt.img_fresh[slot] = 1;
    }
    const u32 c = s_vpt.stage_next;
    s_vpt.stage_next = (c + 1u) % VK_PT_STAGE_CMDS;
    if (s_vpt.stage_busy[c]) {           /* VK_PT_STAGE_CMDS stages back: GPU lag only */
        vk_fence_wait(s_vpt.stage_fences[c]);
        s_vpt.stage_busy[c] = 0;
    }
    pthread_mutex_lock(&s_vk.present_mu);   /* a present's exclusion, as vk_eng_present */
    VkCommandBuffer cb = s_vpt.stage_cmds[c];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        pthread_mutex_unlock(&s_vk.present_mu);
        VK_LOG("present thread: stage recording failed\n");
        return 0;
    }
    vk_cmd_after_everything(cb);
    vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_vpt.img[slot].img, VK_IMAGE_ASPECT_COLOR_BIT,
                        s_vpt.img_fresh[slot] ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
    };
    pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vpt.img[slot].img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                    (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) {
        pthread_mutex_unlock(&s_vk.present_mu);
        VK_LOG("present thread: vkEndCommandBuffer (stage) failed\n");
        return 0;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cb };
    pvkResetFences(s_vk.device, 1, &s_vpt.stage_fences[c]);
    if (vk_queue_submit(&si, s_vpt.stage_fences[c]) != VK_SUCCESS) {
        pthread_mutex_unlock(&s_vk.present_mu);
        VK_LOG("present thread: vkQueueSubmit (stage) failed\n");
        return 0;
    }
    s_vpt.stage_busy[c] = 1;
    s_vpt.slot_fence[slot] = s_vpt.stage_fences[c];
    s_vpt.img_fresh[slot] = 0;
    s_vk.present_order = 1;                 /* later writes wait for this copy */
    if (s_vk.fps_on) {
        pthread_mutex_lock(&s_vpt.fps_mu);
        metal_frame_rate_guest(&s_vk.fps);
        pthread_mutex_unlock(&s_vpt.fps_mu);
    }
    pthread_mutex_unlock(&s_vk.present_mu);
    return 1;
}

/* The show thread's submission: on the queue at once, and not the original
 * owner's (it reads only a staging image and writes only the offscreen target
 * and a swapchain image). */
static VkResult vk_pt_queue_submit(const VkSubmitInfo* si, VkFence fence)
{
    pthread_mutex_lock(&s_vk.queue_mu);
    const VkResult r = pvkQueueSubmit(s_vk.queue, 1, si, fence);
    pthread_mutex_unlock(&s_vk.queue_mu);
    return r;
}

static int vk_pt_show(void* user, uint32_t slot, rsx_present_show_timing* timing)
{
    (void)user;
    if (slot >= RSX_PRESENT_THREAD_MAX_DEPTH || !s_vpt.img[slot].img) return 0;
    const uint64_t t0 = vkpt_now();
    const u32 k = s_vpt.show_next;
    s_vpt.show_next = (k + 1u) % VK_PT_SHOW_SLOTS;
    if (s_vpt.show_busy[k]) {
        vk_fence_wait(s_vpt.show_fences[k]);
        s_vpt.show_busy[k] = 0;
    }
    /* The stage must reach the queue first: wait out a hold it is behind. */
    pthread_mutex_lock(&s_vk.queue_mu);
    while (vk_hold_has_locked(s_vpt.slot_fence[slot])) pthread_cond_wait(&s_hold_cv, &s_vk.queue_mu);
    pthread_mutex_unlock(&s_vk.queue_mu);
    u32 idx = 0;
    int window = s_vk.windowed && s_vk.swapchain;
    if (window) {
        VkResult r;
        for (;;) {
            r = pvkAcquireNextImageKHR(s_vk.device, s_vk.swapchain, 100000000ull /* 100 ms */,
                                       s_vpt.show_acquire[k], VK_NULL_HANDLE, &idx);
            if ((r == VK_TIMEOUT || r == VK_NOT_READY) && !__atomic_load_n(&s_vpt.stopping, __ATOMIC_ACQUIRE)) continue;
            break;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { vk_swapchain_lost(); window = 0; }
        else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            if (r != VK_TIMEOUT && r != VK_NOT_READY) VK_LOG("present thread: vkAcquireNextImageKHR failed (VkResult %d)\n", (int)r);
            window = 0;
        }
        if (!window) { timing->acquire_ns = vkpt_now() - t0; return 0; }
    }
    const uint64_t t1 = vkpt_now();
    timing->acquire_ns = t1 - t0;
    VkCommandBuffer cb = s_vpt.show_cmds[k];
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkResetCommandBuffer(cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
        VK_LOG("present thread: show recording failed\n");
        return 0;   /* an acquired image is left unpresented; the next acquire recycles it */
    }
    vk_barrier_image_on(cb, s_vpt.img[slot].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)s_vpt.img_w[slot], (int32_t)s_vpt.img_h[slot], 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
    };
    pvkCmdBlitImage(cb, s_vpt.img[slot].img, VK_IMAGE_LAYOUT_GENERAL,
                    s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                    (s_vpt.img_w[slot] == s_vk.width && s_vpt.img_h[slot] == s_vk.height)
                        ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    if (window) vk_record_window_blit(cb, idx);
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { VK_LOG("present thread: vkEndCommandBuffer (show) failed\n"); return 0; }
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = window ? 1u : 0u, .pWaitSemaphores = &s_vpt.show_acquire[k],
                        .pWaitDstStageMask = &wait_stage,
                        .commandBufferCount = 1, .pCommandBuffers = &cb,
                        .signalSemaphoreCount = window ? 1u : 0u,
                        .pSignalSemaphores = window ? &s_vk.sc_done[idx] : NULL };
    pvkResetFences(s_vk.device, 1, &s_vpt.show_fences[k]);
    if (vk_pt_queue_submit(&si, s_vpt.show_fences[k]) != VK_SUCCESS) {
        VK_LOG("present thread: vkQueueSubmit (show) failed\n");
        atomic_store(&s_vk_eng_failed, 1);
        return 0;
    }
    s_vpt.show_busy[k] = 1;
    s_vk.presented++;
    if (window) {
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &s_vk.sc_done[idx],
            .swapchainCount = 1, .pSwapchains = &s_vk.swapchain, .pImageIndices = &idx,
        };
        const VkResult r = vk_queue_present_unheld(&pi);
        if (s_vk.fps_on && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) {
            pthread_mutex_lock(&s_vpt.fps_mu);
            metal_frame_rate_presented(&s_vk.fps, vk_now_seconds());
            vk_fps_report();
            pthread_mutex_unlock(&s_vpt.fps_mu);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) vk_swapchain_lost();
        else if (r != VK_SUCCESS) VK_LOG("present thread: vkQueuePresentKHR failed (VkResult %d)\n", (int)r);
    }
    timing->blit_ns = vkpt_now() - t1;
    return 1;
}

static void vk_pt_discard(void* user, uint32_t slot)
{
    (void)user; (void)slot;   /* the staging image stays for the slot's next frame */
}

static void vk_pt_stop(void);
static void vk_pt_start(void)
{
    if (s_vpt.pt || !s_vk.device) return;
    const char* rb = getenv("PS3RECOMP_VK_PRESENT_READBACK");
    if ((rb && !strcmp(rb, "1")) || s_vk.dump_path) {
        rsx_present_thread_unsupported("vulkan", "read-back presents (PS3RECOMP_VK_PRESENT_READBACK / PS3RECOMP_VK_DUMP)");
        return;
    }
    memset(&s_vpt, 0, sizeof s_vpt);
    if (pthread_mutex_init(&s_vpt.fps_mu, NULL)) return;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    int ok = pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vpt.stage_pool) == VK_SUCCESS &&
             pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vpt.show_pool) == VK_SUCCESS;
    if (ok) {
        cai.commandPool = s_vpt.stage_pool; cai.commandBufferCount = VK_PT_STAGE_CMDS;
        ok = pvkAllocateCommandBuffers(s_vk.device, &cai, s_vpt.stage_cmds) == VK_SUCCESS;
    }
    if (ok) {
        cai.commandPool = s_vpt.show_pool; cai.commandBufferCount = VK_PT_SHOW_SLOTS;
        ok = pvkAllocateCommandBuffers(s_vk.device, &cai, s_vpt.show_cmds) == VK_SUCCESS;
    }
    for (u32 i = 0; ok && i < VK_PT_STAGE_CMDS; i++)
        ok = pvkCreateFence(s_vk.device, &fci, NULL, &s_vpt.stage_fences[i]) == VK_SUCCESS;
    for (u32 i = 0; ok && i < VK_PT_SHOW_SLOTS; i++)
        ok = pvkCreateFence(s_vk.device, &fci, NULL, &s_vpt.show_fences[i]) == VK_SUCCESS &&
             pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vpt.show_acquire[i]) == VK_SUCCESS;
    if (ok) {
        const rsx_present_hooks hooks = { NULL, vk_pt_stage, vk_pt_show, vk_pt_discard };
        s_vpt.pt = rsx_present_thread_create(&hooks, rsx_present_thread_depth_from(getenv("PS3RECOMP_RSX_PRESENT_THREAD_DEPTH")));
    }
    if (!s_vpt.pt) {
        VK_LOG("present thread could not start; presents stay on the presenting thread\n");
        vk_pt_stop();
    }
}

static void vk_pt_stop(void)
{
    __atomic_store_n(&s_vpt.stopping, 1, __ATOMIC_RELEASE);
    rsx_present_thread_destroy(s_vpt.pt);   /* joins; discards what is still staged */
    s_vpt.pt = NULL;
    if (!s_vk.device || (!s_vpt.stage_pool && !s_vpt.show_pool)) return;
    vk_device_idle();
    for (u32 i = 0; i < RSX_PRESENT_THREAD_MAX_DEPTH; i++) vk_pt_destroy_image(i);
    for (u32 i = 0; i < VK_PT_STAGE_CMDS; i++)
        if (s_vpt.stage_fences[i]) pvkDestroyFence(s_vk.device, s_vpt.stage_fences[i], NULL);
    for (u32 i = 0; i < VK_PT_SHOW_SLOTS; i++) {
        if (s_vpt.show_fences[i]) pvkDestroyFence(s_vk.device, s_vpt.show_fences[i], NULL);
        if (s_vpt.show_acquire[i]) pvkDestroySemaphore(s_vk.device, s_vpt.show_acquire[i], NULL);
    }
    if (s_vpt.stage_pool) pvkDestroyCommandPool(s_vk.device, s_vpt.stage_pool, NULL);
    if (s_vpt.show_pool) pvkDestroyCommandPool(s_vk.device, s_vpt.show_pool, NULL);
    pthread_mutex_destroy(&s_vpt.fps_mu);
    memset(&s_vpt, 0, sizeof s_vpt);
}

/* The named surface becomes the frame: blit it onto the display image, then
 * the existing present does the readback, the PPM dump and the window. */
static void vk_eng_present(void* user, u32 surface)
{
    (void)user;
    if (!s_vk.device) return;
    if (s_vpt.pt) {
        /* Present thread: stage the surface and return (see above). */
        if (s_bat_on) (void)vk_bat_flush(0);
        (void)rsx_present_thread_submit(s_vpt.pt, surface);
        s_g.last_draws = s_g.draws;
        s_g.draws = 0;
        return;
    }
    const uint64_t t0 = vkpt_enabled() ? vkpt_now() : 0;
    /* Batched: what the frame recorded goes on the queue ahead of the blit. */
    if (s_bat_on) (void)vk_bat_flush(0);
    vkpt_add(VKPT_FLUSH, t0);
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    const uint64_t tl = s_vkpt_on > 0 ? vkpt_now() : 0;
    pthread_mutex_lock(&s_vk.present_mu);
    vkpt_add(VKPT_LOCK, tl);
    VkCommandBuffer cb = vk_present_record_begin();
    if (cb) {
        if (o) {
            s_vk.present_src[s_vk.present_slot] = o->im.img;
            vk_barrier_image_on(cb, o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            vk_barrier_image_on(cb, s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
            VkImageBlit blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { { 0, 0, 0 }, { (int32_t)o->w, (int32_t)o->h, 1 } },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { { 0, 0, 0 }, { (int32_t)s_vk.width, (int32_t)s_vk.height, 1 } },
            };
            pvkCmdBlitImage(cb, o->im.img, VK_IMAGE_LAYOUT_GENERAL,
                            s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                            (o->w == s_vk.width && o->h == s_vk.height) ? VK_FILTER_NEAREST
                                                                        : VK_FILTER_LINEAR);
        }
        vk_present_record_end(cb);
    }
    if (s_vkpt_on > 0) { vkpt_add(VKPT_TOTAL, t0); vkpt_commit(s_vk.present_split != 0); }
    pthread_mutex_unlock(&s_vk.present_mu);
    s_g.last_draws = s_g.draws;
    s_g.draws = 0;
}

/* Pixels a 2D transfer put into guest memory, pushed into the colour target
 * that rectangle belongs to. */
static void vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || o->fmt != VK_FORMAT_R8G8B8A8_UNORM) return;   /* rows are R,G,B,A */
    if (!rows || !w || !h || x + w > o->w || y + h > o->h) return;
    vk_eng_upload_region(o->im.img, 0, 0, x, y, w, h, (size_t)w * o->bpp, h, rows, row_bytes);
}

static void vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(surface, VK_ENG_COLOR);
    if (!o || !out || !out_pitch || !w || !h || x + w > o->w || y + h > o->h) return;
    const size_t tight = (size_t)w * o->bpp;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)tight * h, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return;
    }
    if (!vk_begin()) {
        vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, o->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        if (!vk_sync()) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch, (const u8*)st.ptr + (size_t)row * tight, tight);
        }
    }
    vk_destroy_buffer(&st);
}

/* ---- E3: textures ---------------------------------------------------------
 * The engine decodes every level to host rows (rsx_texture_decode) and hands
 * them over one upload each; the crossbar is a property of the view, so it
 * becomes the image view's component mapping. A cube map (faces == 6) is a
 * six-layer cube-compatible image, each face uploaded into its layer, sampled
 * through a CUBE view; the pipelines that sample one use the split set layout
 * (vk_eng_split_layout). */
static u32 vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)user;
    u32 bpp;
    const VkFormat vf = vk_eng_format(fmt, &bpp);
    const int bc = (fmt == RSX_BE_FMT_BC1 || fmt == RSX_BE_FMT_BC2 || fmt == RSX_BE_FMT_BC3);
    if (vf == VK_FORMAT_UNDEFINED || !w || !h || w > vk_max_dim() || h > vk_max_dim() ||
        (bc && !s_vk.bc_ok))
        return 0;
    const int cube = (faces == 6);
    if ((faces != 1 && !cube) || (cube && w != h)) return 0;   /* cube faces are square */
    if (!mips) mips = 1;
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, .imageType = VK_IMAGE_TYPE_2D,
        .format = vf, .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = cube ? 6u : 1u,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    u32 type;
    if (pvkCreateImage(s_vk.device, &ici, NULL, &o->im.img) != VK_SUCCESS) goto fail;
    pvkGetImageMemoryRequirements(s_vk.device, o->im.img, &req);
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) goto fail;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &o->im.mem) != VK_SUCCESS ||
        pvkBindImageMemory(s_vk.device, o->im.img, o->im.mem, 0) != VK_SUCCESS ||
        vk_eng_make_view(o->im.img, vf, mips, vk_remap(remap, rsx_fmt), cube, &o->im.view))
        goto fail;
    o->kind = VK_ENG_TEXTURE; o->fmt = vf; o->w = w; o->h = h; o->cube = (u8)cube;
    o->bpp = bc ? 0 : bpp; o->block = bc ? bpp : 0;
    /* UNDEFINED -> GENERAL for every level and face before the uploads arrive. */
    if (vk_begin()) goto fail;
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    if (vk_submit_and_wait()) goto fail;
    return hd;
fail:
    vk_destroy_image(&o->im);
    memset(o, 0, sizeof *o);
    return 0;
}

static void vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows)
{
    (void)user;
    vk_eng_obj* o = vk_eng_get(texture, VK_ENG_TEXTURE);
    if (!o || !src || !row_bytes || !rows || face >= (o->cube ? 6u : 1u)) return;
    /* A BC row is a row of 4x4 blocks; the engine's `rows` already counts them. */
    const size_t tight = o->block ? (size_t)((w + 3u) / 4u) * o->block : (size_t)w * o->bpp;
    vk_eng_upload_rows(o->im.img, face, mip, w, h, tight, rows, src, row_bytes);
}

/* Where the view for a (surface, crossbar, format) was last found. The engine
 * asks for a sampled target's view on every draw that reads it, and the scan
 * behind a miss walks all VK_ENG_MAX_OBJ slots. A hint is only used after the
 * slot it names is checked to still be a view of exactly that key, so a view
 * released with its surface, or a slot reused for anything else, falls back
 * to the scan. At most one view per key exists (one is only made after the
 * scan found none), so a hint and the scan answer with the same handle. */
#define VK_ENG_VIEW_HINTS 256u
static u32 s_eview_hint[VK_ENG_VIEW_HINTS];
static unsigned long long s_eview_hits, s_eview_scans;
static u32 vk_eng_view_hint_slot(u32 surface, u32 remap, u32 rsx_format)
{
    u32 h = surface * 0x9E3779B1u ^ remap * 0x85EBCA77u ^ rsx_format * 0xC2B2AE3Du;
    h ^= h >> 15;
    return h & (VK_ENG_VIEW_HINTS - 1u);
}
static int vk_eng_view_is(u32 i, u32 surface, u32 remap, u32 rsx_format)
{
    return i && i < VK_ENG_MAX_OBJ && s_eobj[i].kind == VK_ENG_VIEW && s_eobj[i].surface == surface &&
           s_eobj[i].remap == remap && s_eobj[i].rsx_fmt == rsx_format;
}

/* A colour target sampled with a unit's crossbar: an extra view of the same
 * image, cached by (surface, crossbar, format) and released with the target. */
static u32 vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format)
{
    (void)user;
    vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t) return 0;
    u32* hint = &s_eview_hint[vk_eng_view_hint_slot(surface, remap, rsx_format)];
    if (vk_eng_view_is(*hint, surface, remap, rsx_format)) { s_eview_hits++; return *hint; }
    s_eview_scans++;
    for (u32 i = 1; i < VK_ENG_MAX_OBJ; i++)
        if (vk_eng_view_is(i, surface, remap, rsx_format)) { *hint = i; return i; }
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_eng_make_view(t->im.img, t->fmt, 1, vk_remap(remap, rsx_format), 0, &o->im.view)) {
        memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_VIEW; o->fmt = t->fmt; o->w = t->w; o->h = t->h;
    o->surface = surface; o->remap = remap; o->rsx_fmt = rsx_format;
    *hint = hd;
    return hd;
}
/* ---- colour feedback snapshots ---------------------------------------------
 * A draw that samples a colour target it is also writing (in-place deferred
 * lighting: read the albedo, write the lit colour over it) gets a copy of
 * that target taken just before it. The engine asks for one per such draw
 * (rsx_draw_backend.color_snapshot) and releases the previous one; without
 * this hook it falls back to the guest-memory bytes at the target's address,
 * which hold whatever was last stored there -- for Twisted Metal the previous
 * frame's UI, lit and composited into the next frame.
 *
 * Every operation here is one synchronous submission, so the copy is already
 * in command order: after the draws before it, before the draw that samples
 * it. The copy is a COLOR object marked `snapshot` so the engine can take a
 * crossbar view of it (vk_eng_surface_view); it is never an attachment.
 * Released copies keep their images in a small pool, since a lit frame asks
 * for one per light. */
#define VK_SNAP_POOL 8
static struct { vk_image im; VkFormat fmt; u32 w, h; } s_snap_pool[VK_SNAP_POOL];
static u32 s_snap_pool_n;

static void vk_eng_snapshot_recycle(vk_eng_obj* o)
{
    if (s_snap_pool_n < VK_SNAP_POOL && o->im.img) {
        s_snap_pool[s_snap_pool_n].im = o->im;
        s_snap_pool[s_snap_pool_n].fmt = o->fmt;
        s_snap_pool[s_snap_pool_n].w = o->w;
        s_snap_pool[s_snap_pool_n].h = o->h;
        s_snap_pool_n++;
    } else {
        vk_destroy_image(&o->im);
    }
}

static void vk_eng_snapshot_pool_drain(void)
{
    for (u32 i = 0; i < s_snap_pool_n; i++) vk_destroy_image(&s_snap_pool[i].im);
    s_snap_pool_n = 0;
}

static u32 vk_eng_color_snapshot(void* user, u32 surface)
{
    (void)user;
    static u32 logs;
#define VK_SNAP_FAIL(why) do { if (logs++ < 8) VK_LOG("engine: colour snapshot of %u failed: %s\n", surface, why); } while (0)
    const vk_eng_obj* t = vk_eng_get(surface, VK_ENG_COLOR);
    if (!t || t->snapshot) {
        if (logs < 8 && surface < VK_ENG_MAX_OBJ)
            VK_LOG("engine: colour snapshot of %u: kind %d retired %d native pin %d snapshot %d %ux%u\n", surface,
                   (int)s_eobj[surface].kind, (int)s_eobj_retired[surface], (int)s_eobj_native_pins[surface],
                   (int)s_eobj[surface].snapshot, s_eobj[surface].w, s_eobj[surface].h);
        VK_SNAP_FAIL(!t ? "no colour target" : "target is a snapshot"); return 0;
    }
    const u32 hd = vk_eng_alloc();
    if (!hd) { VK_SNAP_FAIL("no object slot"); return 0; }
    vk_eng_obj* o = &s_eobj[hd];
    int reused = 0;
    for (u32 i = 0; i < s_snap_pool_n; i++)
        if (s_snap_pool[i].fmt == t->fmt && s_snap_pool[i].w == t->w && s_snap_pool[i].h == t->h) {
            o->im = s_snap_pool[i].im;
            s_snap_pool[i] = s_snap_pool[--s_snap_pool_n];
            reused = 1;
            break;
        }
    if (!reused && vk_create_image(&o->im, t->fmt, t->w, t->h,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im);
        memset(o, 0, sizeof *o);
        VK_SNAP_FAIL("image creation");
        return 0;
    }
    o->kind = VK_ENG_COLOR; o->snapshot = 1;
    o->fmt = t->fmt; o->bpp = t->bpp; o->w = t->w; o->h = t->h;
    if (vk_begin()) { vk_eng_release(user, hd); VK_SNAP_FAIL("command begin"); return 0; }
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT,
                     reused ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy c = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { t->w, t->h, 1 },
    };
    pvkCmdCopyImage(s_vk.cmd, t->im.img, VK_IMAGE_LAYOUT_GENERAL,
                    o->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    /* The copy, made visible to whatever samples it next. */
    vk_barrier_image(o->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); VK_SNAP_FAIL("submission"); return 0; }
    return hd;
#undef VK_SNAP_FAIL
}

/* ---- E4: depth snapshots --------------------------------------------------
 * A depth target read as a texture, which the engine asks for only after a
 * depth-writing draw. Metal resolves it with a fullscreen pass into R32Float;
 * Vulkan can do it with two copies on the GPU, since a D32_SFLOAT depth
 * aspect copies out as 32-bit floats: depth -> buffer -> an R32_SFLOAT
 * texture. Other depth formats would need a conversion pass: they report 0.
 *
 * Each depth target keeps ONE snapshot texture (its handle in `surface`),
 * refilled on every request, so nothing accumulates however often the engine
 * invalidates and asks again; it goes when the depth target goes. */
static u32 vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z) return 0;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
        s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned_fmt;
        if (!warned_fmt) { VK_LOG("engine: depth snapshots need D32_SFLOAT, this device uses %d\n",
                                  (int)s_vk.depth_format); warned_fmt = 1; }
        return 0;
    }
    if (w > z->w) w = z->w;
    if (h > z->h) h = z->h;
    if (!w || !h) return 0;

    u32 snap = z->surface;
    vk_eng_obj* t = vk_eng_get(snap, VK_ENG_TEXTURE);
    if (!t || t->w != w || t->h != h) {
        if (t) vk_eng_release(user, snap);
        z->surface = 0;
        snap = vk_eng_alloc();
        if (!snap) return 0;
        t = &s_eobj[snap];
        if (vk_create_image(&t->im, VK_FORMAT_R32_SFLOAT, w, h,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT)) {
            vk_destroy_image(&t->im); memset(t, 0, sizeof *t); return 0;
        }
        t->kind = VK_ENG_TEXTURE; t->fmt = VK_FORMAT_R32_SFLOAT; t->bpp = 4; t->w = w; t->h = h;
        z = vk_eng_get(depth, VK_ENG_DEPTH);          /* same slot; reload for clarity */
        z->surface = snap;
        if (vk_begin()) return 0;
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
        if (vk_submit_and_wait()) return 0;
    }

    vk_buffer st = {0};
    if (vk_stage_acquire(&st, (VkDeviceSize)w * h * 4u)) return 0;
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        vk_barrier_image(t->im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                                  .imageExtent = { w, h, 1 } };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &out);
        VkBufferMemoryBarrier bb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 1, &bb, 0, NULL);
        VkBufferImageCopy in = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
        pvkCmdCopyBufferToImage(s_vk.cmd, st.buf, t->im.img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
        rc = vk_submit_and_wait();
    }
    vk_stage_release(&st);
    return rc ? 0 : snap;
}
/* The depth aspect of a D32_SFLOAT target copies out as 32-bit floats, which
 * is what the engine asks for; other depth formats report failure. */
static int vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch)
{
    (void)user;
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !out || !w || !h || x + w > z->w || y + h > z->h ||
        out_pitch < w * 4u || (s_vk.depth_format != VK_FORMAT_D32_SFLOAT &&
                                  s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT))
        return -1;
    vk_buffer st = {0};
    if (vk_create_host_buffer(&st, (VkDeviceSize)w * h * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        vk_destroy_buffer(&st); return -1;
    }
    int rc = vk_begin();
    if (!rc) {
        vk_barrier_image(z->im.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy region = {
            .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
            .imageOffset = { (int32_t)x, (int32_t)y, 0 }, .imageExtent = { w, h, 1 },
        };
        pvkCmdCopyImageToBuffer(s_vk.cmd, z->im.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
        VkBufferMemoryBarrier hb = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE,
        };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 1, &hb, 0, NULL);
        rc = vk_sync();
        if (!rc) {
            vk_invalidate(&st);
            for (u32 row = 0; row < h; row++)
                memcpy((u8*)out + (size_t)row * out_pitch,
                       (const u8*)st.ptr + (size_t)row * w * 4u, (size_t)w * 4u);
        }
    }
    vk_destroy_buffer(&st);
    return rc ? -1 : 0;
}
/* ---- E2: pipelines and draws ---------------------------------------------
 * A pipeline handle carries the two translated modules and the render state
 * the engine keyed it on. Topology is not part of that key (Metal sets it on
 * the encoder), but Vulkan 1.0 bakes it into the pipeline, so each handle
 * builds its VkPipeline per topology on first use.
 *
 * Bindings are the translator's contract (rsx_shader_spirv.h), and the set
 * layout, pipeline layout, descriptor set and constant UBO are the ones the
 * vtable guest path already creates. Vertex inputs are the engine's compact
 * layout: slot n is a float4 at n*16, at location n, as Metal reads it.
 * Known gaps, stated rather than guessed: vertex-texture units (their
 * samplers s0..s3 would alias the pixel samplers' binding), colour targets
 * other than RGBA8 and RGBA16F, depth-only passes, and stencil (the depth format
 * has no stencil aspect yet, so the test is inert). */
/* As many as the engine keeps (ENG_MAX_PIPELINES in rsx_draw_engine.c). God of
 * War: Ascension selects ~740 distinct fragment programs in its first level on
 * top of the menu's; with 512 slots every later pipeline failed silently and the
 * level drew nothing but its clears. */
#define VK_ENG_MAX_PIPE   8192
#define VK_ENG_TOPOLOGIES 5
#define VK_ENG_FB_CACHE   64

typedef struct vk_eng_pipe {
    int                 used;
    VkShaderModule      vs, fs;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_count;
    VkFormat            rt_fmt;          /* picks the render pass it is built against */
    u32                 cube_mask;       /* TextureCube units: the split set layout */
    VkPipeline          variant[VK_ENG_TOPOLOGIES];
} vk_eng_pipe;

static vk_eng_pipe s_epipe[VK_ENG_MAX_PIPE];

static struct {
    VkRenderPass rp[VK_ENG_RT_FORMATS][RSX_BE_MAX_COLOR_TARGETS + 1]; /* by format, colour count */
    struct { u32 key[RSX_BE_MAX_COLOR_TARGETS + 1]; VkFramebuffer fb; } fb[VK_ENG_FB_CACHE];
    u32          fb_count;
    vk_image     scratch_depth;                          /* when no zeta is bound */
    u32          scratch_w, scratch_h;
    u32          pipeline;
    u32          vs_bytes, ps_bytes;
    u32          tex[RSX_BE_MAX_TEXTURES];
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    u32          tex_mask;
    float        vp[4];
    u32          sc[4];
    int          have_vp, have_sc;
    u32          stencil_ref;
    float        depth_bounds[2];     /* min, max for the next draw */
    vk_buffer    indices;
    u32          warned;
    /* The split set layout (vk_eng_split_layout) and its one set, made the
     * first time a pipeline samples a cube map; the 1x1 cube a cube unit
     * with nothing bound samples. */
    VkDescriptorSetLayout split_layout;
    VkDescriptorPool      split_pool;
    VkDescriptorSet       split_set;
    VkPipelineLayout      split_pipe_layout;
    u32                   dummy_cube;
    u32                   dummy_2d;
} s_e2;

#define VK_ENG_SAMP_CACHE 64
static struct { u64 key; VkSampler s; } s_esamp[VK_ENG_SAMP_CACHE];
static u32 s_esamp_count;

static void vk_eng_drop_framebuffer_at(u32 i)
{
    if (s_e2.fb[i].fb && !vk_bat_retire(VK_RET_FRAMEBUFFER, &s_e2.fb[i].fb))
        pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    memmove(&s_e2.fb[i], &s_e2.fb[i + 1], (size_t)(s_e2.fb_count - i - 1) * sizeof s_e2.fb[0]);
    s_e2.fb_count--;
}

static void vk_eng_drop_framebuffers(void)
{
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (s_e2.fb[i].fb && !vk_bat_retire(VK_RET_FRAMEBUFFER, &s_e2.fb[i].fb))
            pvkDestroyFramebuffer(s_vk.device, s_e2.fb[i].fb, NULL);
    s_e2.fb_count = 0;
}

/* A released colour or depth target ends only the framebuffers that name it.
 * Dropping the whole cache on every release rebuilt every pass's framebuffer
 * each time any one target went, about twenty a frame in a busy scene. */
static void vk_eng_drop_framebuffers_for(u32 handle)
{
    for (u32 i = 0; i < s_e2.fb_count; ) {
        int uses = 0;
        for (u32 k = 0; k <= RSX_BE_MAX_COLOR_TARGETS; k++) uses |= s_e2.fb[i].key[k] == handle;
        if (uses) vk_eng_drop_framebuffer_at(i); else i++;
    }
}

/* The render pass for `n` colour attachments of format `cf` plus depth,
 * LOAD/STORE in GENERAL like the vtable path's: clears are separate
 * operations. An MRT set shares target A's format (engine contract). */
static VkRenderPass vk_eng_render_pass(VkFormat cf, u32 n)
{
    const int fi = vk_eng_rt_index(cf);
    if (fi < 0 || n < 1 || n > RSX_BE_MAX_COLOR_TARGETS) return VK_NULL_HANDLE;
    if (s_e2.rp[fi][n]) return s_e2.rp[fi][n];
    VkAttachmentDescription att[RSX_BE_MAX_COLOR_TARGETS + 1];
    VkAttachmentReference   cref[RSX_BE_MAX_COLOR_TARGETS];
    for (u32 i = 0; i <= n; i++) {
        att[i] = (VkAttachmentDescription){
            .format = (i < n) ? cf : s_vk.depth_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            /* Stencil lives across draws like depth does (a title writes it in
             * one pass and tests it in the next); ignored for colour and for
             * depth formats without stencil. */
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL };
        if (i < n) cref[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkAttachmentReference dref = { n, VK_IMAGE_LAYOUT_GENERAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = n, .pColorAttachments = cref,
                                 .pDepthStencilAttachment = &dref };
    VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = n + 1, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub };
    if (pvkCreateRenderPass(s_vk.device, &rci, NULL, &s_e2.rp[fi][n]) != VK_SUCCESS)
        s_e2.rp[fi][n] = VK_NULL_HANDLE;
    return s_e2.rp[fi][n];
}

/* Tables carried from the Metal backend, itself copied from the D3D12 live
 * draw engine a title has shipped on. */
static VkBlendFactor vk_gcm_blend_factor(u32 f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_gcm_blend_op(u32 e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;                  /* FUNC_ADD 0x8006 */
    }
}
static VkCompareOp vk_gcm_compare(u32 f)
{
    return (f >= 0x0200u && f <= 0x0206u) ? (VkCompareOp)(f - 0x0200u) : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_gcm_stencil_op(u32 op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;               /* GL_KEEP 0x1E00 */
    }
}
static VkStencilOpState vk_gcm_stencil_face(u32 func, u32 fail, u32 zfail, u32 zpass,
                                            u32 cmp_mask, u32 write_mask)
{
    VkStencilOpState s = { .failOp = vk_gcm_stencil_op(fail), .passOp = vk_gcm_stencil_op(zpass),
                           .depthFailOp = vk_gcm_stencil_op(zfail),
                           .compareOp = vk_gcm_compare(func),
                           .compareMask = cmp_mask & 0xFFu, .writeMask = write_mask & 0xFFu,
                           .reference = 0 };                /* dynamic */
    return s;
}

static int vk_eng_translate(const char* hlsl, int stage, VkShaderModule* out)
{
    u32 words = 0;
    if (rsx_hlsl_to_spirv(hlsl, stage, s_g.spv, VK_SPV_WORDS, &words, s_g.log, sizeof s_g.log) || !words) {
        VK_LOG("engine: %s translation failed: %s\n",
               stage == RSX_SHADER_STAGE_VERTEX ? "vertex" : "fragment", s_g.log);
        return -1;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = (size_t)words * 4u, .pCode = s_g.spv };
    return pvkCreateShaderModule(s_vk.device, &ci, NULL, out) == VK_SUCCESS ? 0 : -1;
}

/* A pixel program with a cube unit declares all sixteen units one by one
 * (TextureCube or Texture2D rsx_texN : register(tN)) instead of the
 * Texture2D array, and register(tN) is binding RSX_SPIRV_TEXTURE_BINDING + N
 * (rsx_shader_spirv.h): sixteen single bindings, where the shared layout has
 * one binding of sixteen. Those pipelines use this layout, made on first use
 * so a title without cube maps never builds it. */
static VkPipelineLayout vk_eng_split_layout(void)
{
    if (s_e2.split_pipe_layout) return s_e2.split_pipe_layout;
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[3 + RSX_MAX_TEXTURES];
    b[0] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_VPCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[1] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_PSCONST_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, NULL };
    b[2] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_SAMPLER_BINDING, VK_DESCRIPTOR_TYPE_SAMPLER,
                                           RSX_MAX_TEXTURES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
        b[3 + u] = (VkDescriptorSetLayoutBinding){ RSX_SPIRV_TEXTURE_BINDING + u,
                                                   VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 3 + RSX_MAX_TEXTURES, .pBindings = b };
    VkDescriptorPoolSize ps[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RSX_MAX_TEXTURES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, RSX_MAX_TEXTURES },
    };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = ps };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e2.split_layout) != VK_SUCCESS ||
        pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_e2.split_pool) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e2.split_pool, .descriptorSetCount = 1,
                                        .pSetLayouts = &s_e2.split_layout };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e2.split_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_e2.split_set) != VK_SUCCESS ||
        pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e2.split_pipe_layout) != VK_SUCCESS) {
        s_e2.split_pipe_layout = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    return s_e2.split_pipe_layout;
}

/* Which units a pixel program declares as TextureCube. */
static u32 vk_eng_cube_units(const char* ps_hlsl)
{
    u32 mask = 0;
    char decl[40];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        snprintf(decl, sizeof decl, "TextureCube rsx_tex%u ", u);
        if (strstr(ps_hlsl, decl)) mask |= 1u << u;
    }
    return mask;
}

static u32 vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count)
{
    (void)user;
    /* Same gate as Metal: no guest programs, no engine pipelines. */
    if (!s_g.on || !vs_hlsl || !ps_hlsl || !rs || !layout || !vertex_stride) return 0;
    u32 rt_bpp;
    const VkFormat cf = vk_eng_format(rt_fmt, &rt_bpp);
    if (vk_eng_rt_index(cf) < 0) {
        if (!(s_e2.warned & 1u)) { VK_LOG("engine: colour format %d not supported yet\n", (int)rt_fmt); s_e2.warned |= 1u; }
        return 0;
    }
    if (!rt_count) rt_count = 1;
    if (rt_count > RSX_BE_MAX_COLOR_TARGETS) rt_count = RSX_BE_MAX_COLOR_TARGETS;
    u32 slot = VK_ENG_MAX_PIPE;
    for (u32 i = 0; i < VK_ENG_MAX_PIPE; i++) if (!s_epipe[i].used) { slot = i; break; }
    if (slot == VK_ENG_MAX_PIPE) {
        if (!(s_e2.warned & 128u)) {
            VK_LOG("engine: pipeline table full (%u), draws will drop\n", (unsigned)VK_ENG_MAX_PIPE);
            s_e2.warned |= 128u;
        }
        return 0;
    }
    vk_eng_pipe* P = &s_epipe[slot];
    memset(P, 0, sizeof *P);
    if (vk_eng_translate(vs_hlsl, RSX_SHADER_STAGE_VERTEX, &P->vs)) return 0;
    if (vk_eng_translate(ps_hlsl, RSX_SHADER_STAGE_FRAGMENT, &P->fs)) {
        pvkDestroyShaderModule(s_vk.device, P->vs, NULL); memset(P, 0, sizeof *P); return 0;
    }
    P->rs = *rs;
    P->nslots = layout->count;
    P->stride = vertex_stride;
    P->rt_count = rt_count;
    P->rt_fmt = cf;
    P->cube_mask = vk_eng_cube_units(ps_hlsl);
    P->used = 1;
    return slot + 1u;
}

static void vk_eng_pipeline_release(void* user, u32 pipeline)
{
    (void)user;
    if (!pipeline || pipeline > VK_ENG_MAX_PIPE || !s_epipe[pipeline - 1].used) return;
    vk_eng_pipe* P = &s_epipe[pipeline - 1];
    for (u32 t = 0; t < VK_ENG_TOPOLOGIES; t++)
        if (P->variant[t] && !vk_bat_retire(VK_RET_PIPELINE, &P->variant[t]))
            pvkDestroyPipeline(s_vk.device, P->variant[t], NULL);
    if (P->vs) pvkDestroyShaderModule(s_vk.device, P->vs, NULL);
    if (P->fs) pvkDestroyShaderModule(s_vk.device, P->fs, NULL);
    memset(P, 0, sizeof *P);
    if (s_e2.pipeline == pipeline) s_e2.pipeline = 0;
}

static VkPipeline vk_eng_variant(vk_eng_pipe* P, u32 topo_idx)
{
    static const VkPrimitiveTopology topo[VK_ENG_TOPOLOGIES] = {
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (P->variant[topo_idx]) return P->variant[topo_idx];
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, P->rt_count);
    if (!rp || P->nslots > VK_GUEST_ATTRS) return VK_NULL_HANDLE;
    const VkPipelineLayout layout = P->cube_mask ? vk_eng_split_layout() : s_g.pipe_layout;
    if (!layout) return VK_NULL_HANDLE;
    const rsx_be_render_state* rs = &P->rs;

    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = P->vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = P->fs, .pName = "main" } };
    VkVertexInputBindingDescription vb = { 0, P->stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[VK_GUEST_ATTRS];
    for (u32 s = 0; s < P->nslots; s++)
        va[s] = (VkVertexInputAttributeDescription){ s, 0, VK_FORMAT_R32G32B32A32_SFLOAT, s * 16u };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
        .vertexAttributeDescriptionCount = P->nslots, .pVertexAttributeDescriptions = va };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topo[topo_idx] };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    /* CULL_FACE FRONT 0x0404 / BACK 0x0405 / FRONT_AND_BACK 0x0408 (front, as
     * in D3D12 and Metal); FRONT_FACE CCW 0x0901 maps directly: both APIs
     * judge winding on screen, and the Y flip in the vertex stage makes this
     * screen the same one D3D's viewport produces. */
    VkPipelineRasterizationStateCreateInfo ras = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = !rs->cull_enable ? VK_CULL_MODE_NONE
                  : (rs->cull_face == 0x0404u || rs->cull_face == 0x0408u) ? VK_CULL_MODE_FRONT_BIT
                                                                           : VK_CULL_MODE_BACK_BIT,
        .frontFace = (rs->front_face == 0x0901u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                 : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState front = vk_gcm_stencil_face(rs->s_func, rs->s_fail, rs->s_zfail, rs->s_zpass,
                                                 rs->s_func_mask, rs->s_write_mask);
    VkStencilOpState back = rs->stencil_two_sided
        ? vk_gcm_stencil_face(rs->bs_func, rs->bs_fail, rs->bs_zfail, rs->bs_zpass,
                              rs->s_func_mask, rs->s_write_mask)
        : front;
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = rs->depth_test ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = (rs->depth_test && rs->depth_write) ? VK_TRUE : VK_FALSE,
        .depthCompareOp = vk_gcm_compare(rs->depth_func),
        .stencilTestEnable = rs->stencil_enable ? VK_TRUE : VK_FALSE,
        .front = front, .back = back,
        /* The bounds themselves are dynamic (vk_eng_set_depth_bounds). */
        .depthBoundsTestEnable = (rs->depth_bounds_test && s_vk.depth_bounds_ok) ? VK_TRUE : VK_FALSE,
        .minDepthBounds = 0.0f, .maxDepthBounds = 1.0f };
    if (rs->depth_bounds_test && !s_vk.depth_bounds_ok && !(s_e2.warned & 64u)) {
        VK_LOG("engine: the device has no depthBounds; the depth bounds test is ignored\n");
        s_e2.warned |= 64u;
    }
    /* nv40 COLOR_MASK: B [0:7], G [8:15], R [16:23], A [24:31], any bit set.
     * Every attachment takes target A's blend and mask (see rsx_draw_engine.h). */
    VkColorComponentFlags wm = 0;
    if ((rs->color_mask >> 16) & 0xFFu) wm |= VK_COLOR_COMPONENT_R_BIT;
    if ((rs->color_mask >>  8) & 0xFFu) wm |= VK_COLOR_COMPONENT_G_BIT;
    if ((rs->color_mask >>  0) & 0xFFu) wm |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask >> 24) & 0xFFu) wm |= VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState cba[RSX_BE_MAX_COLOR_TARGETS];
    /* A device without blending on a float target draws it unblended; that is
     * reported once, never silently. */
    const int blend = rs->blend_enable && vk_eng_rt_blendable(P->rt_fmt);
    if (rs->blend_enable && !blend && !(s_e2.warned & 4096u)) {
        VK_LOG("engine: blending unsupported for colour format %d on this device; drawing unblended\n", (int)P->rt_fmt);
        s_e2.warned |= 4096u;
    }
    for (u32 r = 0; r < P->rt_count; r++)
        cba[r] = (VkPipelineColorBlendAttachmentState){
            .blendEnable = blend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = vk_gcm_blend_factor(rs->sf_rgb),
            .dstColorBlendFactor = vk_gcm_blend_factor(rs->df_rgb),
            .colorBlendOp = vk_gcm_blend_op(rs->eq_rgb),
            .srcAlphaBlendFactor = vk_gcm_blend_factor(rs->sf_a),
            .dstAlphaBlendFactor = vk_gcm_blend_factor(rs->df_a),
            .alphaBlendOp = vk_gcm_blend_op(rs->eq_a),
            .colorWriteMask = wm };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = P->rt_count, .pAttachments = cba };
    VkDynamicState dyn[4] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = s_vk.depth_bounds_ok ? 4u : 3u, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &ras, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = layout, .renderPass = rp, .subpass = 0 };
    vk_pcache_created();
    if (pvkCreateGraphicsPipelines(s_vk.device, s_vk.pcache, 1, &gci, NULL,
                                   &P->variant[topo_idx]) != VK_SUCCESS) {
        VK_LOG("engine: vkCreateGraphicsPipelines failed\n");
        P->variant[topo_idx] = VK_NULL_HANDLE;
    }
    return P->variant[topo_idx];
}

static void vk_eng_bind_pipeline(void* user, u32 pipeline) { (void)user; s_e2.pipeline = pipeline; }

static void vk_eng2_shutdown(void* user)
{
    for (u32 i = 1; i <= VK_ENG_MAX_PIPE; i++) vk_eng_pipeline_release(user, i);
    vk_eng_drop_framebuffers();
    for (u32 f = 0; f < VK_ENG_RT_FORMATS; f++)
        for (u32 n = 0; n <= RSX_BE_MAX_COLOR_TARGETS; n++)
            if (s_e2.rp[f][n]) pvkDestroyRenderPass(s_vk.device, s_e2.rp[f][n], NULL);
    vk_destroy_image(&s_e2.scratch_depth);
    vk_destroy_buffer(&s_e2.indices);
    if (s_e2.split_pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_e2.split_pipe_layout, NULL);
    if (s_e2.split_pool)        pvkDestroyDescriptorPool(s_vk.device, s_e2.split_pool, NULL);
    if (s_e2.split_layout)      pvkDestroyDescriptorSetLayout(s_vk.device, s_e2.split_layout, NULL);
    memset(&s_e2, 0, sizeof s_e2);
    for (u32 i = 0; i < s_esamp_count; i++) pvkDestroySampler(s_vk.device, s_esamp[i].s, NULL);
    s_esamp_count = 0;
}

/* Constants go straight into the shared UBO: vertex at 0, pixel at ps_off.
 * Every operation is waited on, so overwriting per draw is safe. Batched,
 * they go to a CPU shadow that each draw copies into its batch's arena. */
static u8 s_bat_shadow_fail[16];
static u8* vk_bat_shadow(int ps, u32 cap)
{
    u8** p = ps ? &s_bat.psc : &s_bat.vsc;
    u32* c = ps ? &s_bat.psc_cap : &s_bat.vsc_cap;
    if (*c < cap) {
        u8* n = (u8*)realloc(*p, cap);
        if (!n) { vk_e5_fail("constant shadow"); return s_bat_shadow_fail; }
        *p = n; *c = cap;
    }
    return *p;
}
static void vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.vs_bytes = 0; return; }
    const u32 cap = (u32)s_g.ps_off;
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 2u)) { VK_LOG("engine: VS constants truncated\n"); s_e2.warned |= 2u; } }
    /* Batched: a shadow the draw copies into its batch's arena. */
    memcpy(s_bat_on ? (void*)vk_bat_shadow(0, cap) : s_g.ubo.ptr, data, bytes);
    s_e2.vs_bytes = bytes;
}
static void vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes)
{
    (void)user;
    if (!s_g.on || !data) { s_e2.ps_bytes = 0; return; }
    const u32 cap = (u32)(s_g.ubo.size - s_g.ps_off);
    if (bytes > cap) { bytes = cap; if (!(s_e2.warned & 4u)) { VK_LOG("engine: PS constants truncated\n"); s_e2.warned |= 4u; } }
    memcpy(s_bat_on ? (void*)vk_bat_shadow(1, cap) : (void*)((u8*)s_g.ubo.ptr + s_g.ps_off), data, bytes);
    s_e2.ps_bytes = bytes;
}
static void vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user;
    for (u32 u = 0; u < RSX_BE_MAX_TEXTURES; u++) {
        const int on = (mask >> u) & 1u;
        s_e2.tex[u] = on ? textures[u] : 0;
        if (on && samplers) s_e2.samp[u] = samplers[u];
    }
    s_e2.tex_mask = mask;
}
static void vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask)
{
    (void)user; (void)textures; (void)samplers;
    if (mask && !(s_e2.warned & 8u)) { VK_LOG("engine: vertex textures not supported yet\n"); s_e2.warned |= 8u; }
}
static void vk_eng_set_viewport(void* user, float x, float y, float w, float h)
{
    (void)user;
    s_e2.vp[0] = x; s_e2.vp[1] = y; s_e2.vp[2] = w; s_e2.vp[3] = h; s_e2.have_vp = 1;
}
static void vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h)
{
    (void)user;
    s_e2.sc[0] = x; s_e2.sc[1] = y; s_e2.sc[2] = w; s_e2.sc[3] = h; s_e2.have_sc = 1;
}
static void vk_eng_set_stencil_ref(void* user, u32 ref) { (void)user; s_e2.stencil_ref = ref; }

/* Vulkan wants the bounds inside [0, 1] without VK_EXT_depth_range_unrestricted;
 * a NaN clamps to the open end so a garbage register cannot reject a draw. */
static float vk_depth_bound(float v, float open)
{
    if (!(v == v)) return open;
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static void vk_eng_set_depth_bounds(void* user, float min, float max)
{
    (void)user;
    s_e2.depth_bounds[0] = vk_depth_bound(min, 0.0f);
    s_e2.depth_bounds[1] = vk_depth_bound(max, 1.0f);
}

static VkFramebuffer vk_eng_framebuffer(VkRenderPass rp, const u32* rt, u32 n, VkImageView dview,
                                        u32 dkey, u32 w, u32 h)
{
    u32 key[RSX_BE_MAX_COLOR_TARGETS + 1] = { 0 };
    for (u32 i = 0; i < n; i++) key[i] = rt[i];
    key[RSX_BE_MAX_COLOR_TARGETS] = dkey;
    for (u32 i = 0; i < s_e2.fb_count; i++)
        if (!memcmp(s_e2.fb[i].key, key, sizeof key)) return s_e2.fb[i].fb;
    if (s_e2.fb_count >= VK_ENG_FB_CACHE) vk_eng_drop_framebuffer_at(0);   /* the oldest */
    VkImageView views[RSX_BE_MAX_COLOR_TARGETS + 1];
    for (u32 i = 0; i < n; i++) views[i] = s_eobj[rt[i]].im.view;
    views[n] = dview;
    VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp, .attachmentCount = n + 1, .pAttachments = views,
                                    .width = w, .height = h, .layers = 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (pvkCreateFramebuffer(s_vk.device, &fci, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    memcpy(s_e2.fb[s_e2.fb_count].key, key, sizeof key);
    s_e2.fb[s_e2.fb_count++].fb = fb;
    return fb;
}

/* Depth when the draw names no zeta: one scratch image, grown as needed and
 * cleared to the far plane when (re)created. Key 0xFFFFFFFF in the cache. */
static VkImageView vk_eng_scratch_depth(u32 w, u32 h)
{
    if (s_e2.scratch_depth.view && s_e2.scratch_w >= w && s_e2.scratch_h >= h)
        return s_e2.scratch_depth.view;
    vk_eng_drop_framebuffers();
    vk_destroy_image(&s_e2.scratch_depth);
    if (vk_create_image(&s_e2.scratch_depth, s_vk.depth_format, w, h,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        s_vk.depth_aspect) || vk_begin())
        return VK_NULL_HANDLE;
    vk_barrier_image(s_e2.scratch_depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange r = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_e2.scratch_depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &r);
    if (vk_submit_and_wait()) return VK_NULL_HANDLE;
    s_e2.scratch_w = w; s_e2.scratch_h = h;
    return s_e2.scratch_depth.view;
}

static int vk_eng_ensure_indices(VkDeviceSize bytes)
{
    if (s_e2.indices.buf && s_e2.indices.size >= bytes) return 0;
    VkDeviceSize size = s_e2.indices.size ? s_e2.indices.size : 64 * 1024;
    while (size < bytes) size *= 2;
    vk_destroy_buffer(&s_e2.indices);
    if (vk_create_host_buffer(&s_e2.indices, size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return -1;
    return s_e2.indices.coherent ? 0 : -1;
}

/* rsx_be_sampler_desc is the registers decoded the D3D12 way; the wrap codes
 * stay the guest's 1..8, which vk_gcm_wrap already translates. */
static VkSampler vk_eng_sampler(const rsx_be_sampler_desc* d)
{
    u32 lo, hi;
    memcpy(&lo, &d->min_lod, 4); memcpy(&hi, &d->max_lod, 4);
    const u64 key = (u64)d->min_linear | ((u64)d->mag_linear << 1) | ((u64)d->mip_linear << 2)
                  | ((u64)d->mip_present << 3) | ((u64)(d->wrap_s & 0xF) << 4)
                  | ((u64)(d->wrap_t & 0xF) << 8) | ((u64)(d->wrap_r & 0xF) << 12)
                  | ((u64)(lo >> 16) << 16) | ((u64)(hi >> 16) << 32);
    for (u32 i = 0; i < s_esamp_count; i++) if (s_esamp[i].key == key) return s_esamp[i].s;
    if (s_esamp_count >= VK_ENG_SAMP_CACHE) return s_vk.sampler;
    float min_lod = d->min_lod, max_lod = d->mip_present ? d->max_lod : d->min_lod;
    if (max_lod < min_lod) max_lod = min_lod;
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_gcm_wrap(d->wrap_s), .addressModeV = vk_gcm_wrap(d->wrap_t),
        .addressModeW = vk_gcm_wrap(d->wrap_r),
        .minLod = min_lod, .maxLod = max_lod,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler smp;
    if (pvkCreateSampler(s_vk.device, &sci, NULL, &smp) != VK_SUCCESS) return s_vk.sampler;
    s_esamp[s_esamp_count].key = key;
    s_esamp[s_esamp_count++].s = smp;
    return smp;
}

/* The view a bound engine handle samples through: an uploaded texture, a
 * colour target (bound directly when surface_view was not asked), or a view.
 * A unit gets a view of its own kind only -- a cube for a TextureCube unit, a
 * 2D one otherwise -- and NULL for anything else, which then samples the
 * dummy of the unit's kind. */
static VkImageView vk_eng_sample_view(u32 handle, int want_cube)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return VK_NULL_HANDLE;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind != VK_ENG_TEXTURE && o->kind != VK_ENG_COLOR && o->kind != VK_ENG_VIEW)
        return VK_NULL_HANDLE;
    return ((int)o->cube == want_cube) ? o->im.view : VK_NULL_HANDLE;
}

/* What an engine draw samples on a unit with nothing bound: transparent
 * black (0,0,0,0), the Metal backend's placeholder, as a 1x1 2D texture and
 * a 1x1 cube, each made on first use. A title's material that reads a unit it
 * never binds then adds nothing. The magenta it read before showed through as
 * a pink cast on character skin. */
static const u8 s_eng_unbound_texel[4] = { 0x00, 0x00, 0x00, 0x00 };
static VkImageView vk_eng_dummy_cube_view(void)
{
    if (!vk_eng_get(s_e2.dummy_cube, VK_ENG_TEXTURE)) {
        s_e2.dummy_cube = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 6,
                                                0xAAE4u, 0x85u);
        if (!s_e2.dummy_cube) return VK_NULL_HANDLE;
        for (u32 f = 0; f < 6; f++)
            vk_eng_texture_upload(NULL, s_e2.dummy_cube, f, 0, 1, 1, s_eng_unbound_texel, 4, 1);
    }
    return s_eobj[s_e2.dummy_cube].im.view;
}
static VkImageView vk_eng_dummy_2d_view(void)
{
    if (!vk_eng_get(s_e2.dummy_2d, VK_ENG_TEXTURE)) {
        s_e2.dummy_2d = vk_eng_texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 1, 1, 1, 1, 0xAAE4u, 0x85u);
        if (!s_e2.dummy_2d) return VK_NULL_HANDLE;
        vk_eng_texture_upload(NULL, s_e2.dummy_2d, 0, 0, 1, 1, s_eng_unbound_texel, 4, 1);
    }
    return s_eobj[s_e2.dummy_2d].im.view;
}

/* Fill the set the bound pipeline uses: the shared one (sixteen textures in
 * one binding), or for a pipeline with cube units the split one (one binding
 * per unit). Returns the set, or VK_NULL_HANDLE. */
static VkDescriptorSet vk_eng_write_descriptors(const vk_eng_pipe* P, VkDescriptorSet set,
                                                const VkDescriptorBufferInfo* constants)
{
    const int split = P->cube_mask != 0;
    if (!set) set = split ? s_e2.split_set : s_g.set;
    if (!set) return VK_NULL_HANDLE;
    const VkImageView dummy_cube = split ? vk_eng_dummy_cube_view() : VK_NULL_HANDLE;
    if (split && !dummy_cube) return VK_NULL_HANDLE;
    const VkImageView dummy_2d = vk_eng_dummy_2d_view();
    if (!dummy_2d) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[2] = {
        { s_g.ubo.buf, 0, s_e2.vs_bytes ? s_e2.vs_bytes : 16u },
        { s_g.ubo.buf, s_g.ps_off, s_e2.ps_bytes ? s_e2.ps_bytes : 16u },
    };
    if (constants) { bi[0] = constants[0]; bi[1] = constants[1]; }
    VkDescriptorImageInfo ii[RSX_MAX_TEXTURES], si[RSX_MAX_TEXTURES];
    for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) {
        const int cube = (P->cube_mask >> u) & 1u;
        VkImageView v = ((s_e2.tex_mask >> u) & 1u) ? vk_eng_sample_view(s_e2.tex[u], cube)
                                                     : VK_NULL_HANDLE;
        VkSampler smp = v ? vk_eng_sampler(&s_e2.samp[u]) : s_vk.sampler;
        ii[u] = (VkDescriptorImageInfo){ VK_NULL_HANDLE,
                                         v ? v : (cube ? dummy_cube : dummy_2d),
                                         VK_IMAGE_LAYOUT_GENERAL };
        si[u] = (VkDescriptorImageInfo){ smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    }
    VkWriteDescriptorSet w[3 + RSX_MAX_TEXTURES];
    u32 n = 0;
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_VPCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[0] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_PSCONST_BINDING, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[1] };
    w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
          .dstBinding = RSX_SPIRV_SAMPLER_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = si };
    if (split) {
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++)
            w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = set, .dstBinding = RSX_SPIRV_TEXTURE_BINDING + u, .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[u] };
    } else {
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
              .dstBinding = RSX_SPIRV_TEXTURE_BINDING, .descriptorCount = RSX_MAX_TEXTURES,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii };
    }
    pvkUpdateDescriptorSets(s_vk.device, n, w, 0, NULL);
    return set;
}

static void vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count)
{
    (void)user;
    if (!s_e2.pipeline || !vertices || !vertex_count || !stride) return;
    if (topology < RSX_TOPOLOGY_POINTS || topology > RSX_TOPOLOGY_TRIANGLE_STRIP) return;
    vk_eng_pipe* P = &s_epipe[s_e2.pipeline - 1];
    if (!P->used || stride != P->stride) return;

    /* Targets: every member of a set is target A's size (engine contract). */
    const u32 n = s_eng_bound.nrt;
    if (n < 1 || n != P->rt_count) {
        if (!(s_e2.warned & 16u)) { VK_LOG("engine: draw with %u colour targets for a %u-target pipeline skipped\n", n, P->rt_count); s_e2.warned |= 16u; }
        return;
    }
    vk_eng_obj* rt0 = vk_eng_get(s_eng_bound.rt[0], VK_ENG_COLOR);
    if (!rt0) return;
    for (u32 i = 1; i < n; i++)
        if (!vk_eng_get(s_eng_bound.rt[i], VK_ENG_COLOR)) return;
    if (rt0->fmt != P->rt_fmt) {
        if (!(s_e2.warned & 32u)) { VK_LOG("engine: draw into colour format %d with a pipeline built for %d skipped\n", (int)rt0->fmt, (int)P->rt_fmt); s_e2.warned |= 32u; }
        return;
    }
    const u32 w = rt0->w, h = rt0->h;
    vk_eng_obj* zo = vk_eng_get(s_eng_bound.depth, VK_ENG_DEPTH);
    VkImageView dview = (zo && zo->w >= w && zo->h >= h) ? zo->im.view : vk_eng_scratch_depth(w, h);
    if (!dview) return;
    const u32 dkey = (zo && dview == zo->im.view) ? s_eng_bound.depth : 0xFFFFFFFFu;

    VkPipeline pipe = vk_eng_variant(P, (u32)topology - 1u);
    VkRenderPass rp = vk_eng_render_pass(P->rt_fmt, n);
    VkFramebuffer fb = vk_eng_framebuffer(rp, s_eng_bound.rt, n, dview, dkey, w, h);
    if (!pipe || !rp || !fb) return;

    const VkDeviceSize vbytes = (VkDeviceSize)vertex_count * stride;
    VkBuffer vbuf = s_vk.vertices.buf, ibuf = s_e2.indices.buf;
    VkDeviceSize voff = 0, ioff = 0;
    VkDescriptorSet set;
    if (s_bat_on) {
        /* Everything the GPU reads from host memory, in this batch's arena;
         * a set of its own. The unbound-unit dummies are made before the
         * reservation. */
        if (P->cube_mask && !vk_eng_dummy_cube_view()) return;
        if (!vk_eng_dummy_2d_view()) return;
        const VkDeviceSize ibytes = indices && index_count ? (VkDeviceSize)index_count * 4u : 0;
        const u32 vsb = s_e2.vs_bytes ? s_e2.vs_bytes : 16u, psb = s_e2.ps_bytes ? s_e2.ps_bytes : 16u;
        if (vk_bat_reserve(vbytes + ibytes + vsb + psb, 1)) return;
        VkDescriptorBufferInfo cb[2];
        void* vp = vk_bat_alloc(vbytes, &vbuf, &voff);
        void* ip = ibytes ? vk_bat_alloc(ibytes, &ibuf, &ioff) : NULL;
        void* vc = vk_bat_alloc(vsb, &cb[0].buffer, &cb[0].offset);
        void* pc = vk_bat_alloc(psb, &cb[1].buffer, &cb[1].offset);
        if (!vp || (ibytes && !ip) || !vc || !pc) return;
        memcpy(vp, vertices, (size_t)vbytes);
        if (ip) memcpy(ip, indices, (size_t)ibytes);
        memset(vc, 0, vsb); memset(pc, 0, psb);
        if (s_e2.vs_bytes && s_bat.vsc) memcpy(vc, s_bat.vsc, s_e2.vs_bytes);
        if (s_e2.ps_bytes && s_bat.psc) memcpy(pc, s_bat.psc, s_e2.ps_bytes);
        cb[0].range = vsb; cb[1].range = psb;
        const VkDescriptorSet fresh = vk_bat_set(P->cube_mask ? s_e2.split_layout : s_g.set_layout);
        if (!fresh) return;
        set = vk_eng_write_descriptors(P, fresh, cb);
    } else {
        if (vk_ensure_vertex_buffer(vbytes)) return;
        memcpy(s_vk.vertices.ptr, vertices, (size_t)vbytes);
        if (indices && index_count) {
            if (vk_eng_ensure_indices((VkDeviceSize)index_count * 4u)) return;
            memcpy(s_e2.indices.ptr, indices, (size_t)index_count * 4u);
        }
        vbuf = s_vk.vertices.buf; ibuf = s_e2.indices.buf;   /* after any growth */
        set = vk_eng_write_descriptors(P, VK_NULL_HANDLE, NULL);
    }
    if (!set) return;

    if (vk_begin()) return;
    /* Everything submitted before is complete (each submission is waited
     * on); this orders memory for the surfaces this draw touches. */
    for (u32 i = 0; i < n; i++)
        vk_barrier_image(s_eobj[s_eng_bound.rt[i]].im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp, .framebuffer = fb,
                                  .renderArea = { { 0, 0 }, { w, h } } };
    pvkCmdBeginRenderPass(s_vk.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    if (s_e2.have_vp && s_e2.vp[2] > 0.0f && s_e2.vp[3] > 0.0f) {
        vp.x = s_e2.vp[0]; vp.y = s_e2.vp[1]; vp.width = s_e2.vp[2]; vp.height = s_e2.vp[3];
    }
    VkRect2D sc = { { 0, 0 }, { w, h } };
    if (s_e2.have_sc) {                       /* clamped: Vulkan rejects negative or over-size */
        u32 x0 = s_e2.sc[0] < w ? s_e2.sc[0] : w, y0 = s_e2.sc[1] < h ? s_e2.sc[1] : h;
        u32 x1 = s_e2.sc[0] + s_e2.sc[2], y1 = s_e2.sc[1] + s_e2.sc[3];
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0;
        sc.extent.width = x1 > x0 ? x1 - x0 : 0; sc.extent.height = y1 > y0 ? y1 - y0 : 0;
    }
    pvkCmdSetViewport(s_vk.cmd, 0, 1, &vp);
    pvkCmdSetScissor(s_vk.cmd, 0, 1, &sc);
    pvkCmdSetStencilReference(s_vk.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s_e2.stencil_ref & 0xFFu);
    if (s_vk.depth_bounds_ok)
        pvkCmdSetDepthBounds(s_vk.cmd, s_e2.depth_bounds[0], s_e2.depth_bounds[1]);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             P->cube_mask ? s_e2.split_pipe_layout : s_g.pipe_layout,
                             0, 1, &set, 0, NULL);
    pvkCmdBindVertexBuffers(s_vk.cmd, 0, 1, &vbuf, &voff);
    if (indices && index_count) {
        pvkCmdBindIndexBuffer(s_vk.cmd, ibuf, ioff, VK_INDEX_TYPE_UINT32);
        pvkCmdDrawIndexed(s_vk.cmd, index_count, 1, 0, 0, 0);
    } else {
        pvkCmdDraw(s_vk.cmd, vertex_count, 1, 0, 0);
    }
    pvkCmdEndRenderPass(s_vk.cmd);
    vk_submit_and_wait();
}

/* ---- E5: completion tickets, checked flushes and GPU depth conversion -----
 *
 * What the engine's deferred reports, checked SET_REFERENCE, early label
 * release and frames-ahead bound need from a backend, and the Z24S8 depth
 * paths (depth_store_z24, depth_z24_texture, depth_z24_view).
 *
 * Tickets. Every engine operation above is still one submission the CPU
 * waits for, so by the time the engine asks for a ticket there is no
 * recorded-but-unsubmitted work. A ticket is therefore a fence submitted on
 * the queue after everything so far: vkQueueSubmit signals it only once every
 * earlier submission on the queue has completed -- this backend's, the
 * native renderer's and the present's -- so a signalled ticket is a genuine
 * completion of all of it. Tickets live in a ring of VK_E5_SLOTS fences,
 * retired strictly in submission order; a full ring waits for its oldest
 * entry instead of failing. Nothing is retired from a callback, and nothing
 * here ever writes guest memory: completion only advances counters, frees
 * backend-private buffers and destroys released images.
 *
 * A failed submission, fence wait or recording call (s_vk_eng_failed) is
 * sticky: completion_status answers -1 and submit_and_wait_checked 0 from
 * then on, which is what freezes checked REF and deferred publication.
 *
 * Depth conversion. The depth aspect of a D32_SFLOAT(_S8_UINT) target is
 * copied into a buffer as floats; rsx_vk_depth_z24.comp turns each float into
 * the D24S8 word guest memory holds, in place. The store (synchronous, as the
 * engine expects) then copies the words to a cached host buffer; the view
 * (asynchronous) copies them into a fresh RGBA8 image whose component mapping
 * composes the A8R8G8B8 decode's channel rotation with the unit's crossbar,
 * so sampling it equals sampling the decoded upload of the stored bytes.
 * Every recording starts with a full dependency on all earlier queue work and
 * ends with one before all later work, so ordering against draws, clears and
 * the native renderer never depends on what those record themselves.
 *
 * PS3RECOMP_VK_GPU_Z24=1 enables the store and its alias texture (default
 * off: the engine keeps depth_readback plus its CPU conversion). The view is
 * used only under the engine's PS3RECOMP_RSX_GPU_DEPTH_VIEW=1. Other depth
 * formats are refused with one message; the engine then takes its fallback.
 * -------------------------------------------------------------------------*/
#define VK_E5_SLOTS 64u
#define VK_E5_SPARES 4u
#define VK_E5_RETIRE 64u

typedef struct {
    VkFence         fence;
    VkCommandBuffer cb;
    VkDescriptorSet set;
    vk_buffer       words;      /* view: device-local words, retired with the slot */
    u64             ticket;
} vk_e5_slot;

static struct {
    int             ready;      /* pool and fences exist */
    VkCommandPool   pool;
    vk_e5_slot      slot[VK_E5_SLOTS];
    u32             head, count;            /* busy slots, oldest at head */
    u64             issued, completed;      /* tickets, in queue order */
    /* conversion pipeline */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkShaderModule  module;
    VkPipeline      pipe;
    VkDescriptorPool dpool;
    VkDescriptorSet sync_set;
    int             pipe_failed;
    /* the last successful store: device-local words and their host copy */
    vk_buffer       store_dev, store_host;
    u32             store_w, store_h;
    int             store_valid;
    vk_buffer       spare[VK_E5_SPARES];    /* device-local word buffers to reuse */
    u32             nspare;
    struct { vk_image im; u64 ticket; } retire[VK_E5_RETIRE];
    u32             nretire;
    /* counters for PS3RECOMP_RSX_INFLIGHT_LOG=1 */
    u64             cap_waits, views_made, view_buffers_reused, stores, deferred_destroys;
    int             log;
} s_e5;

static int vk_e5_failed(void) { return atomic_load(&s_vk_eng_failed); }
static void vk_e5_fail(const char* what)
{
    if (!atomic_exchange(&s_vk_eng_failed, 1)) VK_LOG("engine: %s failed; completion tickets now report failure\n", what);
    vulkan_owner_fault();
}

static int vk_e5_gpu_z24(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PS3RECOMP_VK_GPU_Z24"); on = e && !strcmp(e, "1"); }
    return on;
}

static int vk_e5_init(void)
{
    if (s_e5.ready) return 0;
    if (!s_vk.device) return -1;
    { const char* e = getenv("PS3RECOMP_RSX_INFLIGHT_LOG"); s_e5.log = e && !strcmp(e, "1"); }
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    if (pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_e5.pool) != VK_SUCCESS) return -1;
    VkCommandBuffer cbs[VK_E5_SLOTS];
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_e5.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = VK_E5_SLOTS };
    if (pvkAllocateCommandBuffers(s_vk.device, &cai, cbs) != VK_SUCCESS) {
        pvkDestroyCommandPool(s_vk.device, s_e5.pool, NULL); s_e5.pool = VK_NULL_HANDLE; return -1;
    }
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (u32 i = 0; i < VK_E5_SLOTS; i++) {
        s_e5.slot[i].cb = cbs[i];
        if (pvkCreateFence(s_vk.device, &fci, NULL, &s_e5.slot[i].fence) != VK_SUCCESS) {
            for (u32 j = 0; j < i; j++) pvkDestroyFence(s_vk.device, s_e5.slot[j].fence, NULL);
            pvkDestroyCommandPool(s_vk.device, s_e5.pool, NULL);
            memset(&s_e5, 0, sizeof s_e5);
            return -1;
        }
    }
    s_e5.ready = 1;
    return 0;
}

static void vk_e5_destroy_spare_or_keep(vk_buffer* b)
{
    if (!b->buf) return;
    if (s_e5.nspare < VK_E5_SPARES) { s_e5.spare[s_e5.nspare++] = *b; memset(b, 0, sizeof *b); }
    else vk_destroy_buffer(b);
}

/* Retire every slot whose fence has signalled, oldest first; with `wait`,
 * block until `until` is retired. 0 on success, -1 on a failed fence. */
static int vk_e5_reap(u64 until, int wait)
{
    while (s_e5.count) {
        vk_e5_slot* sl = &s_e5.slot[s_e5.head];
        VkResult r = vk_fence_status(sl->fence);
        if (r == VK_NOT_READY && wait && sl->ticket <= until) {
            r = vk_fence_wait(sl->fence);
            if (r == VK_SUCCESS) r = vk_fence_status(sl->fence);
        }
        if (r == VK_NOT_READY) break;
        if (r != VK_SUCCESS) { vk_e5_fail("completion fence"); return -1; }
        s_e5.completed = sl->ticket;
        vk_e5_destroy_spare_or_keep(&sl->words);
        s_e5.head = (s_e5.head + 1u) % VK_E5_SLOTS;
        s_e5.count--;
    }
    if (s_bat_on) vk_bat_reap();
    if (s_pin_retire_n) vk_pin_retire_reap(s_e5.completed, 0);
    /* Images released while their writer was in flight. */
    for (u32 i = 0; i < s_e5.nretire; ) {
        if (s_e5.retire[i].ticket <= s_e5.completed) {
            vk_destroy_image(&s_e5.retire[i].im);
            s_e5.retire[i] = s_e5.retire[--s_e5.nretire];
        } else i++;
    }
    return 0;
}

/* The next free slot, its fence reset and its command buffer ready to record
 * (begun, after a full dependency on everything earlier) when `record`. */
static vk_e5_slot* vk_e5_acquire(int record)
{
    if (vk_e5_failed() || vk_e5_init()) return NULL;
    if (vk_e5_reap(0, 0)) return NULL;
    if (s_e5.count == VK_E5_SLOTS) {
        s_e5.cap_waits++;
        if (vk_e5_reap(s_e5.slot[s_e5.head].ticket, 1)) return NULL;
    }
    if (s_e5.issued == UINT64_MAX) { vk_e5_fail("ticket counter"); return NULL; }
    vk_e5_slot* sl = &s_e5.slot[(s_e5.head + s_e5.count) % VK_E5_SLOTS];
    if (pvkResetFences(s_vk.device, 1, &sl->fence) != VK_SUCCESS) { vk_e5_fail("vkResetFences"); return NULL; }
    if (record) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        if (pvkResetCommandBuffer(sl->cb, 0) != VK_SUCCESS || pvkBeginCommandBuffer(sl->cb, &bi) != VK_SUCCESS) {
            vk_e5_fail("async command buffer"); return NULL;
        }
        vk_cmd_after_everything(sl->cb);
    }
    return sl;
}

/* Submit an acquired slot (with its recording, or none) and return its
 * ticket; 0 on failure. */
static u64 vk_e5_submit(vk_e5_slot* sl, int recorded)
{
    if (recorded) {
        vk_cmd_after_everything(sl->cb);     /* before every later submission */
        if (pvkEndCommandBuffer(sl->cb) != VK_SUCCESS) { vk_e5_fail("vkEndCommandBuffer (async)"); return 0; }
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = recorded ? 1u : 0u,
                        .pCommandBuffers = recorded ? &sl->cb : NULL };
    if (vk_queue_submit(&si, sl->fence) != VK_SUCCESS) { vk_e5_fail("vkQueueSubmit (async)"); return 0; }
    sl->ticket = ++s_e5.issued;
    s_e5.count++;
    return sl->ticket;
}

/* ---- E6: batched engine recording (PS3RECOMP_VK_BATCHED_ENGINE=1) ----------
 *
 * Default off. Read at engine init. Without it every operation above is one
 * submission the CPU waits for, and so is every wait behind native work.
 * With it, operations are recorded into one command buffer per batch, and a
 * batch is submitted only at a point that needs it:
 *   - a completion ticket (submit_async), the checked flush, submit_and_wait;
 *   - a read the CPU needs (readback, depth readback, the z24 store), which
 *     also waits;
 *   - the native boundary and every present (submitted, not waited);
 *   - a batch that has grown to PS3RECOMP_VK_BATCH_MAX_OPS operations
 *     (default 2048), or whose arena or descriptor pool is full.
 * A submitted batch is an E5 ticket, so it is retired in queue order with the
 * rest and certifies everything before it.
 *
 * What makes recording instead of waiting safe:
 *   - Order. Each operation begins with a full memory dependency on
 *     everything recorded or submitted before it, exactly as the separate
 *     submissions were ordered, so overwriting a target, a snapshot image or a
 *     store buffer that earlier recorded work still reads is ordered by the
 *     queue.
 *   - CPU-written bytes. Vertices, indices, constants and upload rows are
 *     copied into the batch's own host-visible arena, never into a buffer an
 *     unfinished batch may read. A batch slot, and its arena (4 MiB, grown
 *     on demand) and descriptor pool (1024 sets), are reused only after its
 *     ticket has completed: PS3RECOMP_VK_BATCH_SLOTS batches in flight
 *     (default 16, at most 32), a full ring waits for the oldest. Every
 *     ticket submits the open batch, so the ring is what bounds how far the
 *     consumer runs ahead of work queued behind native passes.
 *   - Descriptors. Every draw gets a fresh set from the batch's pool.
 *   - Lifetime. Images, buffers, framebuffers and pipelines released while
 *     any batch may still use them are destroyed when the last ticket that
 *     could reference them retires.
 * Nothing about guest memory changes: inputs are still copied when recorded,
 * and every result the CPU reads is still waited for at its stream position.
 * -------------------------------------------------------------------------*/
#define VK_BAT_SETS 1024u
static int vk_bat_slot_init(vk_bat_slot* sl)
{
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    if (pvkCreateCommandPool(s_vk.device, &pci, NULL, &sl->pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = sl->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if (pvkAllocateCommandBuffers(s_vk.device, &cai, &sl->cb) != VK_SUCCESS) return -1;
    /* A draw's set: two constant buffers, 16 samplers and 16 images (shared
     * or split layout); a GPU depth view's: one storage buffer. */
    VkDescriptorPoolSize ps[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u * VK_BAT_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 16u * VK_BAT_SETS },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16u * VK_BAT_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_BAT_SETS } };
    VkDescriptorPoolCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = VK_BAT_SETS, .poolSizeCount = 4, .pPoolSizes = ps };
    if (pvkCreateDescriptorPool(s_vk.device, &dci, NULL, &sl->dpool) != VK_SUCCESS) return -1;
    sl->sets_left = VK_BAT_SETS;
    return 0;
}

static void vk_bat_destroy_now(vk_bat_retired* r)
{
    switch (r->kind) {
    case VK_RET_IMAGE:       vk_destroy_image_now(&r->u.im); break;
    case VK_RET_BUFFER:      vk_destroy_buffer_now(&r->u.buf); break;
    case VK_RET_SPARE:       vk_e5_destroy_spare_or_keep(&r->u.buf); break;
    case VK_RET_STAGE:       vk_stage_keep_or_destroy(&r->u.buf); break;
    case VK_RET_FRAMEBUFFER: pvkDestroyFramebuffer(s_vk.device, r->u.fb, NULL); break;
    case VK_RET_PIPELINE:    pvkDestroyPipeline(s_vk.device, r->u.pipe, NULL); break;
    default: break;
    }
}

/* Destroy whatever no unfinished ticket can still reference. */
static void vk_bat_reap(void)
{
    for (u32 i = 0; i < s_bat.nret; ) {
        if (s_bat.ret[i].ticket && s_bat.ret[i].ticket <= s_e5.completed) {
            vk_bat_retired r = s_bat.ret[i];
            s_bat.ret[i] = s_bat.ret[--s_bat.nret];
            vk_bat_destroy_now(&r);
            s_bat.retired++;
        } else i++;
    }
}

/* Take `object` (of `kind`) for destruction once every batch that may use it
 * has completed. 1 when taken; 0 means nothing can be using it: destroy now. */
static int vk_bat_retire(int kind, const void* object)
{
    if (!s_bat_on) return 0;
    if (s_e5.ready && vk_e5_reap(0, 0)) return 0;
    const u64 ticket = s_bat.open ? 0 : (s_e5.issued > s_e5.completed ? s_e5.issued : UINT64_MAX);
    if (ticket == UINT64_MAX) return 0;
    if (s_bat.nret == s_bat.cap) {
        const u32 cap = s_bat.cap ? s_bat.cap * 2u : 256u;
        vk_bat_retired* n = (vk_bat_retired*)realloc(s_bat.ret, (size_t)cap * sizeof *n);
        if (!n) { vk_e5_fail("retire list"); return 0; }   /* destroy now: fail closed */
        s_bat.ret = n; s_bat.cap = cap;
    }
    vk_bat_retired* r = &s_bat.ret[s_bat.nret++];
    memset(r, 0, sizeof *r);
    r->kind = kind; r->ticket = ticket;
    switch (kind) {
    case VK_RET_IMAGE:       r->u.im = *(const vk_image*)object; break;
    case VK_RET_BUFFER:
    case VK_RET_SPARE:
    case VK_RET_STAGE:       r->u.buf = *(const vk_buffer*)object; break;
    case VK_RET_FRAMEBUFFER: r->u.fb = *(const VkFramebuffer*)object; break;
    case VK_RET_PIPELINE:    r->u.pipe = *(const VkPipeline*)object; break;
    default: break;
    }
    return 1;
}

/* Open the current slot for recording: wait until its last batch completed,
 * then reset its command buffer, pool and arena. */
static int vk_bat_open(void)
{
    if (s_bat.open) return 0;
    if (vk_e5_failed() || vk_e5_init()) return -1;
    vk_bat_slot* sl = &s_bat.s[s_bat.cur];
    if (sl->ticket && sl->ticket > s_e5.completed) {
        s_bat.slot_waits++;
        if (vk_e5_reap(sl->ticket, 1)) return -1;
    }
    vk_bat_reap();
    if (pvkResetCommandBuffer(sl->cb, 0) != VK_SUCCESS || pvkResetDescriptorPool(s_vk.device, sl->dpool, 0) != VK_SUCCESS) {
        vk_e5_fail("batch reset"); return -1;
    }
    sl->sets_left = VK_BAT_SETS; sl->used = 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (pvkBeginCommandBuffer(sl->cb, &bi) != VK_SUCCESS) { vk_e5_fail("batch begin"); return -1; }
    s_vk.cmd = sl->cb;
    s_vk.frontend_recording = 1;
    s_vk.present_order = 0;           /* every operation orders after everything anyway */
    s_bat.open = 1; s_bat.ops = 0;
    return 0;
}

static int vk_bat_begin_op(void)
{
    if (vk_bat_open()) return -1;
    vk_cmd_after_everything(s_vk.cmd);
    return 0;
}

/* Submit the open batch (if any) as a ticket; with `wait`, wait for it.
 * Returns 0, or -1 after a failure (latched). */
static u64 s_bat_last_ticket;
static int vk_bat_flush(int wait)
{
    if (!s_bat.open) {
        if (wait && s_e5.issued && vk_e5_reap(s_e5.issued, 1)) return -1;
        return vk_e5_failed() ? -1 : 0;
    }
    vk_bat_slot* bs = &s_bat.s[s_bat.cur];
    vk_cmd_after_everything(bs->cb);
    s_bat.open = 0;
    s_vk.frontend_recording = 0;
    s_vk.cmd = s_bat.legacy_cmd;
    if (pvkEndCommandBuffer(bs->cb) != VK_SUCCESS) { vk_e5_fail("batch end"); return -1; }
    vk_e5_slot* sl = vk_e5_acquire(0);
    if (!sl) return -1;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &bs->cb };
    if (vk_queue_submit(&si, sl->fence) != VK_SUCCESS) { vk_e5_fail("vkQueueSubmit (batch)"); return -1; }
    sl->ticket = ++s_e5.issued;
    s_e5.count++;
    bs->ticket = sl->ticket;
    s_bat_last_ticket = sl->ticket;
    for (u32 i = 0; i < s_bat.nret; i++) if (!s_bat.ret[i].ticket) s_bat.ret[i].ticket = sl->ticket;
    s_bat.batches++; s_bat.batch_ops += s_bat.ops;
    s_bat.cur = (s_bat.cur + 1u) % s_bat.nslots;
    if (wait) { s_bat.waits_for_cpu++; if (vk_e5_reap(sl->ticket, 1)) return -1; }
    return vk_e5_failed() ? -1 : 0;
}

static int vk_bat_end_op(void)
{
    if (!s_bat.open) return -1;
    if (++s_bat.ops >= s_bat.max_ops) return vk_bat_flush(0);
    return 0;
}

/* Make room in the open batch for `bytes` of arena and `sets` descriptor
 * sets, submitting it first when it is full, so one operation's allocations
 * never span two batches. */
static int vk_bat_reserve(VkDeviceSize bytes, u32 sets)
{
    if (vk_bat_open()) return -1;
    const VkDeviceSize need = bytes + 4u * s_bat.align;     /* slack for alignment */
    vk_bat_slot* sl = &s_bat.s[s_bat.cur];
    if ((sl->arena.size - sl->used < need || sl->sets_left < sets) && s_bat.ops) {
        if (vk_bat_flush(0) || vk_bat_open()) return -1;
        sl = &s_bat.s[s_bat.cur];
    }
    if (sl->arena.size - sl->used < need) {
        if (sl->used) { vk_e5_fail("batch arena"); return -1; }   /* fresh slot: cannot happen */
        VkDeviceSize size = sl->arena.size ? sl->arena.size : 4u << 20;
        while (size < need) size *= 2u;
        /* The slot is idle (just opened), but other batches may be pending. */
        if (sl->arena.buf) vk_destroy_buffer(&sl->arena);
        if (vk_create_host_buffer(&sl->arena, size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) ||
            !sl->arena.coherent) {
            vk_destroy_buffer_now(&sl->arena); vk_e5_fail("batch arena allocation"); return -1;
        }
        s_bat.arena_grows++;
    }
    if (sl->sets_left < sets) { vk_e5_fail("batch descriptor pool"); return -1; }
    return 0;
}

/* `bytes` of the open batch's arena (after vk_bat_reserve), aligned for any
 * use: its CPU pointer and its offset in s_bat arena buffer. */
static void* vk_bat_alloc(VkDeviceSize bytes, VkBuffer* buf, VkDeviceSize* off)
{
    vk_bat_slot* sl = &s_bat.s[s_bat.cur];
    const VkDeviceSize o = (sl->used + s_bat.align - 1u) / s_bat.align * s_bat.align;
    if (o + bytes > sl->arena.size) { vk_e5_fail("batch arena overrun"); return NULL; }
    sl->used = o + bytes;
    *buf = sl->arena.buf; *off = o;
    return (u8*)sl->arena.ptr + o;
}

static VkDescriptorSet vk_bat_set(VkDescriptorSetLayout layout)
{
    vk_bat_slot* sl = &s_bat.s[s_bat.cur];
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = sl->dpool, .descriptorSetCount = 1, .pSetLayouts = &layout };
    if (!sl->sets_left || pvkAllocateDescriptorSets(s_vk.device, &dai, &set) != VK_SUCCESS) {
        vk_e5_fail("batch descriptor set"); return VK_NULL_HANDLE;
    }
    sl->sets_left--;
    return set;
}

/* The end of an operation whose result the CPU reads next. */
static int vk_sync(void)
{
    if (!s_bat_on) return vk_submit_and_wait();
    if (vk_bat_end_op()) return -1;
    return vk_bat_flush(1);
}

static void vk_bat_log(void)
{
    if (!s_bat_on || !s_e5.log) return;
    static struct timespec last;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec && now.tv_sec - last.tv_sec < 2) return;
    last = now;
    fprintf(stderr, "[rsx inflight vulkan batch] batches=%llu ops=%llu ops_per_batch=%.1f slot_waits=%llu cpu_waits=%llu "
            "retired=%llu pending_retire=%u arena_grows=%llu\n",
            (unsigned long long)s_bat.batches, (unsigned long long)s_bat.batch_ops,
            s_bat.batches ? (double)s_bat.batch_ops / (double)s_bat.batches : 0.0,
            (unsigned long long)s_bat.slot_waits, (unsigned long long)s_bat.waits_for_cpu,
            (unsigned long long)s_bat.retired, s_bat.nret, (unsigned long long)s_bat.arena_grows);
}

static int vk_bat_init(void)
{
    const char* e = getenv("PS3RECOMP_VK_BATCHED_ENGINE");
    if (!e || strcmp(e, "1")) return 0;
    memset(&s_bat, 0, sizeof s_bat);
    { const char* n = getenv("PS3RECOMP_VK_BATCH_SLOTS"); long v = n ? strtol(n, NULL, 0) : 16;
      s_bat.nslots = v < 2 ? 2u : v > (long)VK_BAT_MAX_SLOTS ? VK_BAT_MAX_SLOTS : (u32)v; }
    { const char* n = getenv("PS3RECOMP_VK_BATCH_MAX_OPS"); long v = n ? strtol(n, NULL, 0) : 2048;
      s_bat.max_ops = v < 1 ? 1u : v > 65536 ? 65536u : (u32)v; }
    VkPhysicalDeviceProperties p; pvkGetPhysicalDeviceProperties(s_vk.phys, &p);
    s_bat.align = 256u;
    if (p.limits.minUniformBufferOffsetAlignment > s_bat.align) s_bat.align = p.limits.minUniformBufferOffsetAlignment;
    if (p.limits.minStorageBufferOffsetAlignment > s_bat.align) s_bat.align = p.limits.minStorageBufferOffsetAlignment;
    if (vk_e5_init()) return -1;
    for (u32 i = 0; i < s_bat.nslots; i++) if (vk_bat_slot_init(&s_bat.s[i])) return -1;
    s_bat.legacy_cmd = s_vk.cmd;
    s_bat_on = 1;
    VK_LOG("engine: batched recording on (%u batches in flight, at most %u operations each)\n", s_bat.nslots, s_bat.max_ops);
    return 0;
}

/* Submit and wait for everything, destroy every retired object, then leave
 * batched mode. */
static void vk_bat_shutdown(void)
{
    if (!s_bat_on && !s_bat.nslots) return;
    if (s_bat_on) (void)vk_bat_flush(1);
    vk_device_idle();
    s_bat_on = 0;
    for (u32 i = 0; i < s_bat.nret; i++) vk_bat_destroy_now(&s_bat.ret[i]);
    free(s_bat.ret);
    vk_stage_drain();
    vk_img_pool_drain();
    for (u32 i = 0; i < s_bat.nslots; i++) {
        vk_destroy_buffer_now(&s_bat.s[i].arena);
        if (s_bat.s[i].dpool) pvkDestroyDescriptorPool(s_vk.device, s_bat.s[i].dpool, NULL);
        if (s_bat.s[i].pool) pvkDestroyCommandPool(s_vk.device, s_bat.s[i].pool, NULL);
    }
    free(s_bat.vsc); free(s_bat.psc);
    if (s_bat.legacy_cmd) s_vk.cmd = s_bat.legacy_cmd;
    s_vk.frontend_recording = 0;
    memset(&s_bat, 0, sizeof s_bat);
}

static int vk_e5_is_ticket_fence(VkFence f)
{
    if (!f || !s_e5.ready) return 0;
    for (u32 i = 0; i < s_e5.count; i++)
        if (s_e5.slot[(s_e5.head + i) % VK_E5_SLOTS].fence == f) return 1;
    return 0;
}

static void vk_e5_log(void)
{
    if (!s_e5.log) return;
    static struct timespec last;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (last.tv_sec && now.tv_sec - last.tv_sec < 2) return;
    last = now;
    fprintf(stderr, "[rsx inflight vulkan] tickets=%llu completed=%llu outstanding=%u cap_waits=%llu "
            "views=%llu view_buffers_reused=%llu stores=%llu deferred_destroys=%llu pending_destroys=%u failed=%d\n",
            (unsigned long long)s_e5.issued, (unsigned long long)s_e5.completed, s_e5.count,
            (unsigned long long)s_e5.cap_waits, (unsigned long long)s_e5.views_made,
            (unsigned long long)s_e5.view_buffers_reused, (unsigned long long)s_e5.stores,
            (unsigned long long)s_e5.deferred_destroys, s_e5.nretire, vk_e5_failed());
}

static u64 vk_eng_submit_async(void* user, u32 reason)
{
    (void)user; (void)reason;
    /* Batched: the recorded work is the ticket. */
    if (s_bat_on && s_bat.open) {
        const int rc = vk_bat_flush(0);
        vk_e5_log(); vk_bat_log();
        return rc || vk_e5_failed() ? 0 : s_bat_last_ticket;
    }
    vk_e5_slot* sl = vk_e5_acquire(0);
    const u64 ticket = sl ? vk_e5_submit(sl, 0) : 0;
    vk_e5_log();
    return vk_e5_failed() ? 0 : ticket;
}

static int vk_eng_completion_status(void* user, u64 ticket, int wait)
{
    (void)user;
    if (!ticket || ticket > s_e5.issued || vk_e5_failed()) return -1;
    if (vk_e5_reap(ticket, wait)) return -1;
    return ticket <= s_e5.completed ? 1 : 0;
}

static int vk_eng_submit_and_wait_checked(void* user, u32 reason)
{
    const u64 ticket = vk_eng_submit_async(user, reason);
    return ticket && vk_eng_completion_status(user, ticket, 1) == 1;
}

/* An engine image that the GPU may still be writing goes on the retire list
 * instead of being destroyed. Returns 1 when it took the image. */
static int vk_e5_defer_destroy(u32 handle)
{
    const u64 t = s_e5_obj_ticket[handle];
    s_e5_obj_ticket[handle] = 0;
    if (!t || !s_e5.ready) return 0;
    if (vk_e5_reap(0, 0) || t <= s_e5.completed) return 0;
    if (s_e5.nretire == VK_E5_RETIRE && vk_e5_reap(s_e5.retire[0].ticket, 1)) return 0;
    if (s_e5.nretire == VK_E5_RETIRE) return 0;      /* reaped above; cannot happen */
    s_e5.retire[s_e5.nretire].im = s_eobj[handle].im;
    s_e5.retire[s_e5.nretire].ticket = t;
    s_e5.nretire++;
    s_e5.deferred_destroys++;
    memset(&s_eobj[handle].im, 0, sizeof s_eobj[handle].im);
    return 1;
}

static void vk_e5_shutdown(void)
{
    if (!s_vk.device) { memset(&s_e5, 0, sizeof s_e5); return; }
    if (s_e5.ready || s_e5.nretire || s_e5.pipe || s_pin_retire_n) vk_device_idle();
    vk_pin_retire_reap(0, 1);
    for (u32 i = 0; i < s_e5.nretire; i++) vk_destroy_image(&s_e5.retire[i].im);
    for (u32 i = 0; i < VK_E5_SLOTS; i++) {
        vk_destroy_buffer(&s_e5.slot[i].words);
        if (s_e5.slot[i].fence) pvkDestroyFence(s_vk.device, s_e5.slot[i].fence, NULL);
    }
    for (u32 i = 0; i < s_e5.nspare; i++) vk_destroy_buffer(&s_e5.spare[i]);
    vk_stage_drain();
    vk_img_pool_drain();
    vk_destroy_buffer(&s_e5.store_dev);
    vk_destroy_buffer(&s_e5.store_host);
    if (s_e5.pool) pvkDestroyCommandPool(s_vk.device, s_e5.pool, NULL);
    if (s_e5.pipe) pvkDestroyPipeline(s_vk.device, s_e5.pipe, NULL);
    if (s_e5.module) pvkDestroyShaderModule(s_vk.device, s_e5.module, NULL);
    if (s_e5.layout) pvkDestroyPipelineLayout(s_vk.device, s_e5.layout, NULL);
    if (s_e5.dpool) pvkDestroyDescriptorPool(s_vk.device, s_e5.dpool, NULL);
    if (s_e5.set_layout) pvkDestroyDescriptorSetLayout(s_vk.device, s_e5.set_layout, NULL);
    memset(&s_e5, 0, sizeof s_e5);
    memset(s_e5_obj_ticket, 0, sizeof s_e5_obj_ticket);
}

/* A device-local buffer the conversion can copy into, run on and copy out of. */
static int vk_e5_device_buffer(vk_buffer* b, VkDeviceSize size)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    memset(b, 0, sizeof *b);
    if (pvkCreateBuffer(s_vk.device, &bci, NULL, &b->buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    pvkGetBufferMemoryRequirements(s_vk.device, b->buf, &req);
    u32 type;
    if (vk_find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        vk_find_memory_type(req.memoryTypeBits, 0, &type)) { vk_destroy_buffer(b); return -1; }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = type };
    if (pvkAllocateMemory(s_vk.device, &mai, NULL, &b->mem) != VK_SUCCESS ||
        pvkBindBufferMemory(s_vk.device, b->buf, b->mem, 0) != VK_SUCCESS) { vk_destroy_buffer(b); return -1; }
    b->size = size;
    return 0;
}

static int vk_e5_pipeline(void)
{
    if (s_e5.pipe) return 0;
    if (s_e5.pipe_failed) return -1;
    s_e5.pipe_failed = 1;
    if (vk_e5_init()) return -1;
    VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1, .pBindings = &b };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &lci, NULL, &s_e5.set_layout) != VK_SUCCESS) return -1;
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                        .setLayoutCount = 1, .pSetLayouts = &s_e5.set_layout,
                                        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    if (pvkCreatePipelineLayout(s_vk.device, &plci, NULL, &s_e5.layout) != VK_SUCCESS) return -1;
    if (vk_create_shader(rsx_vk_depth_z24_comp_spv, sizeof rsx_vk_depth_z24_comp_spv, &s_e5.module)) return -1;
    VkComputePipelineCreateInfo cci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = s_e5.module, .pName = "main" },
        .layout = s_e5.layout };
    vk_pcache_created();
    if (pvkCreateComputePipelines(s_vk.device, s_vk.pcache, 1, &cci, NULL, &s_e5.pipe) != VK_SUCCESS) {
        s_e5.pipe = VK_NULL_HANDLE; return -1;
    }
    /* One set per ticket slot, so an in-flight view's set is never rewritten,
     * and one for the synchronous store. */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_E5_SLOTS + 1u };
    VkDescriptorPoolCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = VK_E5_SLOTS + 1u, .poolSizeCount = 1, .pPoolSizes = &ps };
    if (pvkCreateDescriptorPool(s_vk.device, &dci, NULL, &s_e5.dpool) != VK_SUCCESS) return -1;
    VkDescriptorSetLayout layouts[VK_E5_SLOTS + 1u];
    VkDescriptorSet sets[VK_E5_SLOTS + 1u];
    for (u32 i = 0; i <= VK_E5_SLOTS; i++) layouts[i] = s_e5.set_layout;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = s_e5.dpool, .descriptorSetCount = VK_E5_SLOTS + 1u,
                                        .pSetLayouts = layouts };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, sets) != VK_SUCCESS) return -1;
    for (u32 i = 0; i < VK_E5_SLOTS; i++) s_e5.slot[i].set = sets[i];
    s_e5.sync_set = sets[VK_E5_SLOTS];
    s_e5.pipe_failed = 0;
    return 0;
}

/* Which depth targets the conversion reads: those whose depth aspect copies
 * out as 32-bit floats. Says once when the device's depth format is not one. */
static vk_eng_obj* vk_e5_depth(u32 depth, u32 w, u32 h)
{
    vk_eng_obj* z = vk_eng_get(depth, VK_ENG_DEPTH);
    if (!z || !w || !h || w > z->w || h > z->h || (u64)w * h > UINT32_MAX / 4u) return NULL;
    if (s_vk.depth_format != VK_FORMAT_D32_SFLOAT && s_vk.depth_format != VK_FORMAT_D32_SFLOAT_S8_UINT) {
        static int warned;
        if (!warned) { VK_LOG("engine: GPU z24 conversion needs D32_SFLOAT, this device uses %d; depth reads keep the CPU path\n",
                              (int)s_vk.depth_format); warned = 1; }
        return NULL;
    }
    return z;
}

/* Record: the float bits in `buf` (just written by a transfer) -> D24S8
 * words in place, then the words available to `next_stage`/`next_access`.
 * `set` must not be in use by pending work. */
static void vk_e5_record_words(VkCommandBuffer cb, VkDescriptorSet set, VkBuffer buf, u32 w, u32 h,
                               VkPipelineStageFlags next_stage, VkAccessFlags next_access)
{
    VkBufferMemoryBarrier toc = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, NULL, 1, &toc, 0, NULL);
    VkDescriptorBufferInfo bi = { buf, 0, (VkDeviceSize)w * h * 4u };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    pvkUpdateDescriptorSets(s_vk.device, 1, &wr, 0, NULL);
    pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_e5.pipe);
    pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_e5.layout, 0, 1, &set, 0, NULL);
    const u32 extent[2] = { w, h };
    pvkCmdPushConstants(cb, s_e5.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof extent, extent);
    pvkCmdDispatch(cb, (w + 7u) / 8u, (h + 7u) / 8u, 1);
    VkBufferMemoryBarrier fromc = toc;
    fromc.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    fromc.dstAccessMask = next_access;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, next_stage, 0, 0, NULL, 1, &fromc, 0, NULL);
}

/* Record: the top-left w x h of depth -> `buf` as floats -> D24S8 words. */
static void vk_e5_record_convert(VkCommandBuffer cb, VkDescriptorSet set, const vk_eng_obj* z,
                                 VkBuffer buf, u32 w, u32 h,
                                 VkPipelineStageFlags next_stage, VkAccessFlags next_access)
{
    VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 },
                              .imageExtent = { w, h, 1 } };
    pvkCmdCopyImageToBuffer(cb, z->im.img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &out);
    vk_e5_record_words(cb, set, buf, w, h, next_stage, next_access);
}

/* The A8R8G8B8 decode puts guest bytes A,R,G,B into host R,G,B,A by rotating
 * (unless TEX_RGBA), and D8R8G8B8 reads its alpha as one. The raw image holds
 * the guest bytes in components 0..3; this is the component each decoded
 * channel comes from. */
static VkComponentSwizzle vk_e5_raw(VkComponentSwizzle decoded, int rotate, int alpha_one)
{
    switch (decoded) {
    case VK_COMPONENT_SWIZZLE_R: return rotate ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_R;
    case VK_COMPONENT_SWIZZLE_G: return rotate ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_G;
    case VK_COMPONENT_SWIZZLE_B: return rotate ? VK_COMPONENT_SWIZZLE_A : VK_COMPONENT_SWIZZLE_B;
    case VK_COMPONENT_SWIZZLE_A: return alpha_one ? VK_COMPONENT_SWIZZLE_ONE
                                       : rotate ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_A;
    default:                     return decoded;
    }
}

/* A sampled RGBA8 engine texture that reads raw D24S8 words as the A8R8G8B8 /
 * D8R8G8B8 decode plus crossbar would. Image in UNDEFINED; caller fills it. */
static u32 vk_e5_alias_texture(u32 w, u32 h, u32 remap, u32 rsx_fmt)
{
    const u32 base = rsx_fmt & 0x9Fu;
    if (!w || !h || w > vk_max_dim() || h > vk_max_dim() || (base != 0x85u && base != 0x9Eu)) return 0;
    const int rotate = !rsx_texture_argb_is_rgba(), alpha_one = base == 0x9Eu;
    const VkComponentMapping dec = vk_remap(remap, base);
    const VkComponentMapping map = { vk_e5_raw(dec.r, rotate, alpha_one), vk_e5_raw(dec.g, rotate, alpha_one),
                                     vk_e5_raw(dec.b, rotate, alpha_one), vk_e5_raw(dec.a, rotate, alpha_one) };
    const u32 hd = vk_eng_alloc();
    if (!hd) return 0;
    vk_eng_obj* o = &s_eobj[hd];
    if (vk_create_image_pooled(&o->im, VK_FORMAT_R8G8B8A8_UNORM, w, h,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    pvkDestroyImageView(s_vk.device, o->im.view, NULL);
    o->im.view = VK_NULL_HANDLE;
    if (vk_eng_make_view(o->im.img, VK_FORMAT_R8G8B8A8_UNORM, 1, map, 0, &o->im.view)) {
        vk_destroy_image(&o->im); memset(o, 0, sizeof *o); return 0;
    }
    o->kind = VK_ENG_TEXTURE; o->fmt = VK_FORMAT_R8G8B8A8_UNORM; o->bpp = 4; o->w = w; o->h = h;
    return hd;
}

/* Record: `words` (row length `row` texels) -> the w x h alias image, then
 * the image readable by any later work. */
static void vk_e5_record_fill(VkCommandBuffer cb, VkBuffer words, u32 row, VkImage img, u32 w, u32 h)
{
    VkImageMemoryBarrier to = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &to);
    VkBufferImageCopy in = { .bufferRowLength = row, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageExtent = { w, h, 1 } };
    pvkCmdCopyBufferToImage(cb, words, img, VK_IMAGE_LAYOUT_GENERAL, 1, &in);
    VkImageMemoryBarrier rd = to;
    rd.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    rd.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
    rd.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 0, NULL, 0, NULL, 1, &rd);
}

static int vk_eng_depth_store_z24(void* user, u32 depth, u32 w, u32 h, u8* dst, u32 dst_pitch)
{
    (void)user;
    if (!vk_e5_gpu_z24() || vk_e5_failed() || !dst || dst_pitch < w * 4u) return -1;
    const vk_eng_obj* z = vk_e5_depth(depth, w, h);
    if (!z || vk_e5_pipeline()) return -1;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4u;
    s_e5.store_valid = 0;
    if (s_e5.store_dev.size < bytes) {
        vk_destroy_buffer(&s_e5.store_dev);
        if (vk_e5_device_buffer(&s_e5.store_dev, bytes)) return -1;
    }
    if (s_e5.store_host.size < bytes) {
        vk_destroy_buffer(&s_e5.store_host);
        if (vk_create_host_buffer(&s_e5.store_host, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            vk_destroy_buffer(&s_e5.store_host); return -1;
        }
    }
    if (vk_begin()) return -1;
    vk_cmd_after_everything(s_vk.cmd);
    vk_e5_record_convert(s_vk.cmd, s_e5.sync_set, z, s_e5.store_dev.buf, w, h,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c = { 0, 0, bytes };
    pvkCmdCopyBuffer(s_vk.cmd, s_e5.store_dev.buf, s_e5.store_host.buf, 1, &c);
    VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_e5.store_host.buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    if (vk_sync()) return -1;
    vk_invalidate(&s_e5.store_host);
    const u8* src = (const u8*)s_e5.store_host.ptr;
    if (dst_pitch == w * 4u) memcpy(dst, src, (size_t)bytes);
    else for (u32 y = 0; y < h; y++) memcpy(dst + (size_t)y * dst_pitch, src + (size_t)y * w * 4u, (size_t)w * 4u);
    s_e5.store_w = w; s_e5.store_h = h; s_e5.store_valid = 1;
    s_e5.stores++;
    return 0;
}

static u32 vk_eng_depth_z24_texture(void* user, u32 w, u32 h, u32 remap, u32 rsx_fmt)
{
    (void)user;
    if (!vk_e5_gpu_z24() || vk_e5_failed() || !s_e5.store_valid || !w || !h ||
        w > s_e5.store_w || h > s_e5.store_h)
        return 0;
    const u32 hd = vk_e5_alias_texture(w, h, remap, rsx_fmt);
    if (!hd) return 0;
    if (vk_begin()) { vk_eng_release(user, hd); return 0; }
    vk_cmd_after_everything(s_vk.cmd);
    vk_e5_record_fill(s_vk.cmd, s_e5.store_dev.buf, s_e5.store_w, s_eobj[hd].im.img, w, h);
    if (vk_submit_and_wait()) { vk_eng_release(user, hd); return 0; }
    return hd;
}

static u32 vk_eng_depth_z24_view(void* user, u32 depth, u32 w, u32 h, u32 remap, u32 rsx_fmt)
{
    (void)user;
    const u32 base = rsx_fmt & 0x9Fu;
    if (vk_e5_failed() || (base != 0x85u && base != 0x9Eu)) return 0;
    const vk_eng_obj* z = vk_e5_depth(depth, w, h);
    if (!z || vk_e5_pipeline()) return 0;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4u;
    vk_buffer words = {0};
    for (u32 i = 0; i < s_e5.nspare; i++)
        if (s_e5.spare[i].size >= bytes && s_e5.spare[i].size <= bytes * 2u) {
            words = s_e5.spare[i]; s_e5.spare[i] = s_e5.spare[--s_e5.nspare];
            s_e5.view_buffers_reused++;
            break;
        }
    if (!words.buf && vk_e5_device_buffer(&words, bytes)) return 0;
    const u32 hd = vk_e5_alias_texture(w, h, remap, rsx_fmt);
    if (s_bat_on) {
        /* Batched: recorded after the draws so far in the open batch, which
         * the engine's submit_async right after submits as the ticket. */
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!hd || vk_bat_reserve(0, 1) || !(set = vk_bat_set(s_e5.set_layout)) || vk_begin()) {
            if (hd) vk_eng_release(user, hd);
            vk_destroy_buffer(&words);
            return 0;
        }
        vk_e5_record_convert(s_vk.cmd, set, z, words.buf, w, h,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vk_e5_record_fill(s_vk.cmd, words.buf, w, s_eobj[hd].im.img, w, h);
        (void)vk_submit_and_wait();                       /* ends the recorded operation */
        if (!vk_bat_retire(VK_RET_SPARE, &words)) vk_e5_destroy_spare_or_keep(&words);
        s_e5.views_made++;
        return hd;
    }
    vk_e5_slot* sl = hd ? vk_e5_acquire(1) : NULL;
    if (!sl) {
        if (hd) vk_eng_release(user, hd);
        vk_e5_destroy_spare_or_keep(&words);
        return 0;
    }
    vk_e5_record_convert(sl->cb, sl->set, z, words.buf, w, h,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    vk_e5_record_fill(sl->cb, words.buf, w, s_eobj[hd].im.img, w, h);
    sl->words = words;           /* freed (or kept as a spare) when the slot retires */
    const u64 ticket = vk_e5_submit(sl, 1);
    if (!ticket) {
        /* Never submitted: nothing on the GPU references either. */
        vk_destroy_buffer(&sl->words);
        vk_eng_release(user, hd);
        return 0;
    }
    s_e5_obj_ticket[hd] = ticket;
    s_e5_obj_ticket[depth] = ticket;     /* its copy reads the depth target */
    s_e5.views_made++;
    return hd;
}

/* Every registered original backend entry shares the host owner gate. */
static int owner_vk_eng_init(void* user, u32 width, u32 height){
    if(!vulkan_owner_begin(1)){return -1;}uint64_t before=s_vulkan_commits;
    int result=vk_eng_init(user,width,height);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static void owner_vk_eng_shutdown(void* user){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_shutdown(user);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_submit_and_wait(void* user, u32 reason){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_submit_and_wait(user,reason);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static u32 owner_vk_eng_texture_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                 u32 mips, u32 faces, u32 remap, u32 rsx_fmt){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_texture_create(user,fmt,w,h,mips,faces,remap,rsx_fmt);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static void owner_vk_eng_texture_upload(void* user, u32 texture, u32 face, u32 mip, u32 w, u32 h,
                                  const void* src, u32 row_bytes, u32 rows){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_texture_upload(user,texture,face,mip,w,h,src,row_bytes,rows);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_release(void* user, u32 handle){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_release(user,handle);vulkan_owner_finish(vulkan_owner_boundary(before));
}
/* The engine releases each object through the call for its kind. Handles are
 * slots that get reused, so a release naming an object of another kind is a
 * stale handle: freeing it would destroy a live object that now owns the
 * slot (a colour target the next feedback draw snapshots). It is refused and
 * reported instead. */
static int vk_eng_release_kind_ok(u32 handle, int want_color, int want_depth)
{
    if (!handle || handle >= VK_ENG_MAX_OBJ) return 1;
    const vk_eng_obj* o = &s_eobj[handle];
    if (o->kind == VK_ENG_FREE || s_eobj_retired[handle]) return 1;   /* vk_eng_release ignores these */
    const int color = o->kind == VK_ENG_COLOR && !o->snapshot, depth = o->kind == VK_ENG_DEPTH;
    const int ok = want_color ? color : want_depth ? depth : (!color && !depth);
    if (!ok) {
        static u32 logs;
        if (logs++ < 16)
            VK_LOG("engine: stale %s release of object %u (kind %d%s, %ux%u) refused\n",
                   want_color ? "colour target" : want_depth ? "depth target" : "texture", handle,
                   (int)o->kind, o->snapshot ? " snapshot" : "", o->w, o->h);
    }
    return ok;
}
static void owner_vk_eng_texture_release(void* user, u32 handle){
    if(vk_eng_release_kind_ok(handle,0,0))owner_vk_eng_release(user,handle);
}
static void owner_vk_eng_color_target_release(void* user, u32 handle){
    if(vk_eng_release_kind_ok(handle,1,0))owner_vk_eng_release(user,handle);
}
static void owner_vk_eng_depth_target_release(void* user, u32 handle){
    if(vk_eng_release_kind_ok(handle,0,1))owner_vk_eng_release(user,handle);
}
static u32 owner_vk_eng_color_target_create(void* user, rsx_be_format fmt, u32 w, u32 h,
                                      const void* seed, u32 seed_row_bytes){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_color_target_create(user,fmt,w,h,seed,seed_row_bytes);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_surface_view(void* user, u32 surface, u32 remap, u32 rsx_format){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_surface_view(user,surface,remap,rsx_format);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_depth_target_create(void* user, u32 w, u32 h){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_depth_target_create(user,w,h);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_depth_snapshot(void* user, u32 depth, u32 w, u32 h){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_depth_snapshot(user,depth,w,h);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_color_snapshot(void* user, u32 surface){
    if(!vulkan_owner_begin(1)){static u32 logs;if(logs++<8)VK_LOG("engine: colour snapshot of %u refused by the native owner gate\n",surface);return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_color_snapshot(user,surface);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_pipeline_create(void* user, const char* vs_hlsl, const char* ps_hlsl,
                                  const rsx_be_render_state* rs, const rsx_vertex_layout_plan* layout,
                                  u32 vertex_stride, rsx_be_format rt_fmt, u32 rt_count){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_pipeline_create(user,vs_hlsl,ps_hlsl,rs,layout,vertex_stride,rt_fmt,rt_count);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static void owner_vk_eng_pipeline_release(void* user, u32 pipeline){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_pipeline_release(user,pipeline);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_targets(void* user, const u32* surfaces, u32 count, u32 depth){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_targets(user,surfaces,count,depth);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_pipeline(void* user, u32 pipeline){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_pipeline(user,pipeline);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_vs_constants(void* user, const void* data, u32 bytes){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_vs_constants(user,data,bytes);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_ps_constants(void* user, const void* data, u32 bytes){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_ps_constants(user,data,bytes);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_textures(void* user, const u32* textures,
                                 const rsx_be_sampler_desc* samplers, u32 mask){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_textures(user,textures,samplers,mask);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_bind_vertex_textures(void* user, const u32* textures,
                                        const rsx_be_sampler_desc* samplers, u32 mask){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_bind_vertex_textures(user,textures,samplers,mask);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_set_viewport(void* user, float x, float y, float w, float h){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_set_viewport(user,x,y,w,h);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_set_scissor(void* user, u32 x, u32 y, u32 w, u32 h){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_set_scissor(user,x,y,w,h);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_set_stencil_ref(void* user, u32 ref){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_set_stencil_ref(user,ref);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_set_depth_bounds(void* user, float min, float max){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_set_depth_bounds(user,min,max);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_draw(void* user, rsx_topology topology, const void* vertices, u32 vertex_count,
                        u32 stride, const u32* indices, u32 index_count){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_draw(user,topology,vertices,vertex_count,stride,indices,index_count);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_clear_color(void* user, u32 surface, const float rgba[4]){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_clear_color(user,surface,rgba);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_clear_depth_stencil(void* user, u32 depth, u32 flags,
                                       float depth_value, u8 stencil){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_clear_depth_stencil(user,depth,flags,depth_value,stencil);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_present(void* user, u32 surface){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_present(user,surface);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_eng_readback(void* user, u32 surface, u32 x, u32 y, u32 w, u32 h,
                            void* out, u32 out_pitch){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_readback(user,surface,x,y,w,h,out,out_pitch);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static int owner_vk_eng_depth_readback(void* user, u32 depth, u32 x, u32 y, u32 w, u32 h,
                                 float* out, u32 out_pitch){
    if(!vulkan_owner_begin(1)){return -1;}uint64_t before=s_vulkan_commits;
    int result=vk_eng_depth_readback(user,depth,x,y,w,h,out,out_pitch);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static void owner_vk_eng_color_target_write(void* user, u32 surface, u32 x, u32 y,
                                      u32 w, u32 h, const void* rows, u32 row_bytes){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_eng_color_target_write(user,surface,x,y,w,h,rows,row_bytes);vulkan_owner_finish(vulkan_owner_boundary(before));
}

static u64 owner_vk_eng_submit_async(void* user, u32 reason){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u64 result=vk_eng_submit_async(user,reason);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static int owner_vk_eng_completion_status(void* user, u64 ticket, int wait){
    if(!vulkan_owner_begin(0)){return -1;}
    int result=vk_eng_completion_status(user,ticket,wait);vulkan_owner_finish(1);return result;
}
static int owner_vk_eng_submit_and_wait_checked(void* user, u32 reason){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    int result=vk_eng_submit_and_wait_checked(user,reason);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static int owner_vk_eng_depth_store_z24(void* user, u32 depth, u32 w, u32 h, u8* dst, u32 dst_pitch){
    if(!vulkan_owner_begin(1)){return -1;}uint64_t before=s_vulkan_commits;
    int result=vk_eng_depth_store_z24(user,depth,w,h,dst,dst_pitch);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_depth_z24_texture(void* user, u32 w, u32 h, u32 remap, u32 rsx_fmt){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_depth_z24_texture(user,w,h,remap,rsx_fmt);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static u32 owner_vk_eng_depth_z24_view(void* user, u32 depth, u32 w, u32 h, u32 remap, u32 rsx_fmt){
    if(!vulkan_owner_begin(1)){return 0;}uint64_t before=s_vulkan_commits;
    u32 result=vk_eng_depth_z24_view(user,depth,w,h,remap,rsx_fmt);vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}

static const rsx_draw_backend s_vk_engine_backend = {
    .user                 = NULL,
    .init                 = owner_vk_eng_init,
    .shutdown             = owner_vk_eng_shutdown,
    .submit_and_wait      = owner_vk_eng_submit_and_wait,
    .texture_create       = owner_vk_eng_texture_create,
    .texture_upload       = owner_vk_eng_texture_upload,
    .texture_release      = owner_vk_eng_texture_release,
    .color_target_create  = owner_vk_eng_color_target_create,
    .color_target_release = owner_vk_eng_color_target_release,
    .surface_view         = owner_vk_eng_surface_view,
    .depth_target_create  = owner_vk_eng_depth_target_create,
    .depth_target_release = owner_vk_eng_depth_target_release,
    .depth_snapshot       = owner_vk_eng_depth_snapshot,
    .color_snapshot       = owner_vk_eng_color_snapshot,
    .pipeline_create      = owner_vk_eng_pipeline_create,
    .pipeline_release     = owner_vk_eng_pipeline_release,
    .bind_targets         = owner_vk_eng_bind_targets,
    .bind_pipeline        = owner_vk_eng_bind_pipeline,
    .bind_vs_constants    = owner_vk_eng_bind_vs_constants,
    .bind_ps_constants    = owner_vk_eng_bind_ps_constants,
    .bind_textures        = owner_vk_eng_bind_textures,
    .bind_vertex_textures = owner_vk_eng_bind_vertex_textures,
    .set_viewport         = owner_vk_eng_set_viewport,
    .set_scissor          = owner_vk_eng_set_scissor,
    .set_stencil_ref      = owner_vk_eng_set_stencil_ref,
    .set_depth_bounds     = owner_vk_eng_set_depth_bounds,
    .draw                 = owner_vk_eng_draw,
    .clear_color          = owner_vk_eng_clear_color,
    .clear_depth_stencil  = owner_vk_eng_clear_depth_stencil,
    .present              = owner_vk_eng_present,
    .readback             = owner_vk_eng_readback,
    .depth_readback       = owner_vk_eng_depth_readback,
    .color_target_write   = owner_vk_eng_color_target_write,
    .depth_store_z24      = owner_vk_eng_depth_store_z24,
    .depth_z24_texture    = owner_vk_eng_depth_z24_texture,
    .depth_z24_view       = owner_vk_eng_depth_z24_view,
    .submit_and_wait_checked = owner_vk_eng_submit_and_wait_checked,
    .submit_async         = owner_vk_eng_submit_async,
    .completion_status    = owner_vk_eng_completion_status,
};

/* Every registered original backend entry shares the host owner gate. */
static void owner_vk_cb_track_state(void* ud, const rsx_state* state){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_track_state(ud,state);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_cb_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_clear(ud,flags,color,depth,stencil);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_cb_draw_arrays(void* ud, u32 primitive, u32 first, u32 count){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_draw_arrays(ud,primitive,first,count);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_cb_draw_indexed(void* ud, u32 primitive, u32 offset, u32 count){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_draw_indexed(ud,primitive,offset,count);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_cb_bind_texture(void* ud, u32 unit, const rsx_texture_state* t){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_bind_texture(ud,unit,t);vulkan_owner_finish(vulkan_owner_boundary(before));
}
static void owner_vk_cb_present(void* ud, u32 buffer_id){
    if(!vulkan_owner_begin(1)){return;}uint64_t before=s_vulkan_commits;
    vk_cb_present(ud,buffer_id);vulkan_owner_finish(vulkan_owner_boundary(before));
}

static rsx_backend s_vulkan_backend = {
    .userdata           = &s_vk,
    .set_render_target  = owner_vk_cb_track_state,
    .set_vertex_attribs = owner_vk_cb_track_state,
    .set_depth_stencil  = owner_vk_cb_track_state,
    .clear              = owner_vk_cb_clear,
    .draw_arrays        = owner_vk_cb_draw_arrays,
    .draw_indexed       = owner_vk_cb_draw_indexed,
    .bind_texture       = owner_vk_cb_bind_texture,
    .present            = owner_vk_cb_present,
};

/* ---------------------------------------------------------------------------
 * Public entry points
 * -------------------------------------------------------------------------*/
static int vk_init_all(u32 width, u32 height, const char* title)
{
    pthread_mutex_init(&s_vk.queue_mu, NULL);
    pthread_mutex_init(&s_vk.present_mu, NULL);
    s_vk.width  = width  ? width  : 1280;
    s_vk.height = height ? height : 720;
    s_vk.dump_path = getenv("PS3RECOMP_VK_DUMP");
    /* Writing a full frame to disk on every present costs more than the
     * present itself and slows a title enough to change what it does; a
     * live title wants one now and then, not all of them. */
    { const char* e = getenv("PS3RECOMP_VK_DUMP_EVERY");
      s_vk.dump_every = (e && atoi(e) > 0) ? (u32)atoi(e) : 1; }
    { const char* e = getenv("PS3RECOMP_VK_FPS");
      s_vk.fps_on = e && !strcmp(e, "1");
      metal_frame_rate_reset(&s_vk.fps, vk_now_seconds()); }

    vk_window_open(title);           /* no-op unless PS3RECOMP_VK_WINDOW is set */
    if (vk_load_library()) return -1;

    const char *frontend = getenv("PS3RECOMP_VK_FRONTEND");
    s_vk.frontend_enabled = (frontend && !strcmp(frontend, "1")) || vulkan_owner_enabled();
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "ps3recomp", .pEngineName = "ps3recomp RSX",
                              .apiVersion = s_vk.frontend_enabled ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app };
    if (s_vk.windowed) {
        ci.enabledExtensionCount   = s_vk.inst_ext_count;
        ci.ppEnabledExtensionNames = s_vk.inst_exts;
    }
    VkResult ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    if (ir != VK_SUCCESS && s_vk.windowed) {
        vk_window_drop("vkCreateInstance with the surface extensions failed");
        ci.enabledExtensionCount = 0;
        ci.ppEnabledExtensionNames = NULL;
        ir = pvkCreateInstance(&ci, NULL, &s_vk.instance);
    }
    if (ir != VK_SUCCESS) { VK_LOG("vkCreateInstance failed (VkResult %d)\n", (int)ir); return -1; }
    if (vk_load_instance_functions()) return -1;
    if (s_vk.windowed) {
        if (vk_load_wsi_instance_functions()) {
            vk_window_drop("the driver has no surface functions");
        } else if (vk_create_window_surface()) {
            char why[256];
#ifdef _WIN32
            snprintf(why, sizeof why, "vkCreateWin32SurfaceKHR failed");
#else
            snprintf(why, sizeof why, "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
#endif
            vk_window_drop(why);
        }
    }
    if (vk_pick_device()) {
        if (!s_vk.windowed) return -1;
        vk_window_drop("no device can present to the window");
        if (vk_pick_device()) return -1;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = s_vk.queue_family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    static const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    /* DXT (BC1-3) is how PS3 titles ship most textures; Vulkan gates it
     * behind a feature, enabled only when the device reports it. */
    VkPhysicalDeviceFeatures have, want;
    memset(&want, 0, sizeof want);
    pvkGetPhysicalDeviceFeatures(s_vk.phys, &have);
    want.textureCompressionBC = have.textureCompressionBC;
    s_vk.bc_ok = have.textureCompressionBC ? 1 : 0;
    /* The depth bounds test: a deferred renderer bounds its light pass to
     * the geometry and its sky to the far plane with it. Optional in
     * Vulkan; without it the test stays off, as before. */
    want.depthBounds = have.depthBounds;
    s_vk.depth_bounds_ok = have.depthBounds ? 1 : 0;
    /* Anisotropic filtering, for a native frontend's samplers on this device
     * (the owner reports it enabled). The engine's own samplers never ask
     * for it, so enabling it changes nothing for them. */
    want.samplerAnisotropy = have.samplerAnisotropy;
    s_vk.anisotropy_ok = have.samplerAnisotropy ? 1 : 0;
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = s_vk.windowed ? 1u : 0u,
                               .ppEnabledExtensionNames = s_vk.windowed ? dev_exts : NULL,
                               .pEnabledFeatures = &want };
    /* External synthetic fixtures may enable a checked, supported feature for
     * deterministic GPU delay on this same public-init owner. Production is
     * exactly the original loaded API call, with no extra feature requested. */
#ifndef PS3RECOMP_VK_TEST_CREATE_DEVICE
#define PS3RECOMP_VK_TEST_CREATE_DEVICE pvkCreateDevice
#endif
    VK_CHECK(PS3RECOMP_VK_TEST_CREATE_DEVICE(s_vk.phys, &dci, NULL, &s_vk.device), "vkCreateDevice");
    if (vk_load_device_functions()) return -1;
    if (s_vk.windowed && vk_load_wsi_device_functions())
        vk_window_drop("the driver has no swapchain functions");
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    vk_pcache_open();

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = s_vk.queue_family };
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = s_vk.pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, &s_vk.cmd), "vkAllocateCommandBuffers");
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.fence), "vkCreateFence");
    /* The present's own (see vk_state). */
    VK_CHECK(pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.present_pool), "vkCreateCommandPool");
    cai.commandPool = s_vk.present_pool;
    cai.commandBufferCount = VK_PRESENT_SLOTS;
    VK_CHECK(pvkAllocateCommandBuffers(s_vk.device, &cai, s_vk.present_cmds), "vkAllocateCommandBuffers");
    cai.commandBufferCount = 1;
    for (u32 i = 0; i < VK_PRESENT_SLOTS; i++)
        VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.present_fences[i]), "vkCreateFence");
    s_vk.present_slot = 0;
    s_vk.present_cmd = s_vk.present_cmds[0];
    s_vk.present_fence = s_vk.present_fences[0];
    VK_CHECK(pvkCreateFence(s_vk.device, &fci, NULL, &s_vk.window_fence), "vkCreateFence");

    if (vk_pick_depth_format()) return -1;
    if (vk_create_image(&s_vk.color, VK_FORMAT_R8G8B8A8_UNORM, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return -1;
    if (vk_create_image(&s_vk.depth, s_vk.depth_format, s_vk.width, s_vk.height,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, s_vk.depth_aspect)) return -1;
    if (vk_create_host_buffer(&s_vk.readback, (VkDeviceSize)s_vk.width * s_vk.height * 4u,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return -1;
    if (vk_create_render_pass()) return -1;
    if (vk_create_pipeline_objects()) return -1;
    if (vk_guest_init()) {
        VK_LOG("guest programs: setup failed, fixed-function path only\n");
        vk_guest_shutdown();
    }

    /* Both targets UNDEFINED -> GENERAL once. Colour starts opaque black so a
     * present before the first clear reads defined memory; depth starts at
     * the FAR plane, not zero -- zero is the near plane, and a guest that
     * enables LESS before its first depth clear would draw nothing (the
     * null backend makes the same choice). */
    if (vk_begin()) return -1;
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED);
    vk_barrier_image(s_vk.depth.img, s_vk.depth_aspect, VK_IMAGE_LAYOUT_UNDEFINED);
    VkClearColorValue black = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkClearDepthStencilValue far_plane = { 1.0f, 0 };
    VkImageSubresourceRange cr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange dr = { s_vk.depth_aspect, 0, 1, 0, 1 };
    pvkCmdClearColorImage(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &cr);
    pvkCmdClearDepthStencilImage(s_vk.cmd, s_vk.depth.img, VK_IMAGE_LAYOUT_GENERAL, &far_plane, 1, &dr);
    if (vk_submit_and_wait()) return -1;

    if (s_vk.windowed) {
        VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (pvkCreateSemaphore(s_vk.device, &semi, NULL, &s_vk.sem_acquire) != VK_SUCCESS ||
            vk_create_swapchain())
            vk_window_drop("the swapchain could not be created");
    }
    return 0;
}

static int vulkan_original_init(u32 width, u32 height, const char* title)
{
    memset(&s_vk, 0, sizeof s_vk);
    if (vk_init_all(width, height, title)) {
        rsx_vulkan_backend_shutdown();
        return -1;
    }
    /* One path at a time: the FIFO walker feeds both the rsx_state vtable
     * and the register-file engine, so registering both would record every
     * draw twice (the rule the Metal backend's init follows).
     *
     * With guest programs on, the engine is the default -- as it became
     * Metal's once every host mode passed through it. It builds every
     * pipeline from the guest's programs, so without them it has nothing to
     * draw with, and the vtable's fixed-function path stays the default.
     * Either way PS3RECOMP_RSX_ENGINE=dispatch|vtable overrides. */
    rsx_draw_engine_set_backend(&s_vk_engine_backend);
    rsx_draw_engine_set_default(s_g.on ? 1 : 0);
    if (rsx_draw_engine_enabled() && rsx_draw_engine_init(s_vk.width, s_vk.height) == 0)
        s_vk.eng_active = 1;
    else {
        rsx_set_backend(&s_vulkan_backend);
        if (rsx_present_thread_requested())
            rsx_present_thread_unsupported("vulkan fixed-function path", "the draw engine is off");
    }
    VK_LOG("%s path\n", s_vk.eng_active ? "register-file draw engine" : "vtable");
    VK_LOG("offscreen target %ux%u ready, depth format %d (%s)\n",
           s_vk.width, s_vk.height, (int)s_vk.depth_format, title ? title : "");
    return 0;
}

static int vk_frontend_begin(void *user)
{
    vk_state *state = user;
    if (state->frontend_recording || pthread_mutex_trylock(&state->present_mu)) return 0;
    return 1;
}
static void vk_frontend_end(void *user) { pthread_mutex_unlock(&((vk_state *)user)->present_mu); }
int rsx_vulkan_backend_frontend_available(void)
{
    if (!s_vk.device || !s_vk.frontend_enabled) return 0;
    VkPhysicalDeviceProperties properties; pvkGetPhysicalDeviceProperties(s_vk.phys, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) return 0;
    uint32_t count = 0; pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &count, NULL);
    if (s_vk.queue_family >= count || count > 64) return 0;
    VkQueueFamilyProperties queues[64]; pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &count, queues);
    return (queues[s_vk.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
}
rsx_frontend_device *rsx_vulkan_backend_frontend(void)
{
    if(vulkan_owner_enabled())return NULL; /* exclusive extracted native owner */
    if (!rsx_vulkan_backend_frontend_available()) return NULL;
    if (!s_vk_frontend) {
        rsx_frontend_vulkan_owner owner = {0};
        owner.device = s_vk.device; owner.queue = s_vk.queue; owner.queue_family = s_vk.queue_family;
        pvkGetPhysicalDeviceProperties(s_vk.phys, &owner.properties); owner.memory = s_vk.memprops;
        owner.get_proc = pvkGetDeviceProcAddr; owner.queue_mutex = &s_vk.queue_mu;
        owner.begin_submit = vk_frontend_begin; owner.end_submit = vk_frontend_end; owner.user = &s_vk;
        s_vk_frontend = rsx_frontend_vulkan_create(&owner);
    }
    return s_vk_frontend;
}

static void vulkan_original_shutdown(void)
{
    rsx_frontend_close(s_vk_frontend); s_vk_frontend = NULL;
    rsx_vulkan_backend_present_finish();   /* a host that began one and quit */
    if (rsx_get_backend() == &s_vulkan_backend) rsx_set_backend(NULL);
    if (s_vk.eng_active) { rsx_draw_engine_shutdown(); s_vk.eng_active = 0; }
    rsx_draw_engine_set_backend(NULL);
    if (s_vk.windowed && s_vk.swapchain && s_vk.presented && s_vk.hold_seconds > 0.0) {
        VK_LOG("holding the last frame on screen for %.1f s "
               "(Esc or closing the window ends it early)\n", s_vk.hold_seconds);
        const double end = vk_now_seconds() + s_vk.hold_seconds;
        while (s_vk.windowed && vk_now_seconds() < end) {
            if (rsx_vulkan_backend_pump_messages() != 0) break;
            vk_present_window();         /* FIFO: paced by the display */
        }
    }
    if (s_vk.windowed || s_vk.window) vk_window_drop(NULL);
    if (s_vk.device) {
        vk_device_idle();
        if(vulkan_owner_enabled()&&s_vulkan_owner.failed)return;
        vk_guest_shutdown();
        vk_bat_shutdown();
        vk_e5_shutdown();
        for (int i = 0; i < VK_PIPELINE_SLOTS; i++)
            if (s_vk.pipelines[i]) pvkDestroyPipeline(s_vk.device, s_vk.pipelines[i], NULL);
        if (s_vk.pipe_layout) pvkDestroyPipelineLayout(s_vk.device, s_vk.pipe_layout, NULL);
        if (s_vk.desc_pool)   pvkDestroyDescriptorPool(s_vk.device, s_vk.desc_pool, NULL);
        if (s_vk.set_layout)  pvkDestroyDescriptorSetLayout(s_vk.device, s_vk.set_layout, NULL);
        if (s_vk.sampler)     pvkDestroySampler(s_vk.device, s_vk.sampler, NULL);
        for (u32 i = 0; i < s_vk.samp_count; i++)
            pvkDestroySampler(s_vk.device, s_vk.samp_cache[i].s, NULL);
        if (s_vk.vs)          pvkDestroyShaderModule(s_vk.device, s_vk.vs, NULL);
        if (s_vk.fs)          pvkDestroyShaderModule(s_vk.device, s_vk.fs, NULL);
        if (s_vk.framebuffer) pvkDestroyFramebuffer(s_vk.device, s_vk.framebuffer, NULL);
        if (s_vk.render_pass) pvkDestroyRenderPass(s_vk.device, s_vk.render_pass, NULL);
        for (u32 u = 0; u < RSX_MAX_TEXTURES; u++) vk_destroy_image(&s_vk.tex[u]);
        vk_destroy_image(&s_vk.dummy);
        vk_destroy_image(&s_vk.depth);
        vk_destroy_image(&s_vk.color);
        vk_destroy_buffer(&s_vk.vertices);
        vk_destroy_buffer(&s_vk.readback);
        s_vulkan_boundary_fence=VK_NULL_HANDLE;s_vulkan_boundary_commit=s_vulkan_boundary_generation=0;
        if (s_vk.fence) pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.pool)  pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        for (u32 i = 0; i < VK_PRESENT_SLOTS; i++) {
            if (s_vk.present_fences[i]) pvkDestroyFence(s_vk.device, s_vk.present_fences[i], NULL);
            if (s_vk.present_acquire[i]) pvkDestroySemaphore(s_vk.device, s_vk.present_acquire[i], NULL);
        }
        for (u32 i = 0; i < VK_SHOW_SLOTS; i++) {
            if (s_show.fences[i]) pvkDestroyFence(s_vk.device, s_show.fences[i], NULL);
            if (s_show.acquire[i]) pvkDestroySemaphore(s_vk.device, s_show.acquire[i], NULL);
        }
        if (s_vk.window_fence)  pvkDestroyFence(s_vk.device, s_vk.window_fence, NULL);
        if (s_vk.present_pool)  pvkDestroyCommandPool(s_vk.device, s_vk.present_pool, NULL);
        vk_pcache_close();
        pvkDestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance && pvkDestroyInstance) pvkDestroyInstance(s_vk.instance, NULL);
#ifdef _WIN32
    if (s_vk.lib) FreeLibrary((HMODULE)s_vk.lib);
#else
    if (s_vk.lib) dlclose(s_vk.lib);
#endif
    free(s_vk.cpu_verts);
    pthread_mutex_destroy(&s_vk.queue_mu);
    pthread_mutex_destroy(&s_vk.present_mu);
    memset(&s_vk, 0, sizeof s_vk);
    memset(&s_show, 0, sizeof s_show);
}

int rsx_vulkan_backend_pump_messages(void)
{
    vk_show_pump();
#ifdef _WIN32
    /* The message queue belongs to the thread that created the window: the
     * host pumps from that thread, as for the D3D12 backend. The native OSK's
     * Win32 host shares this pump. */
#ifdef OSK_NATIVE_HAS_WIN32_HOST
    osk_native_win32_pump();
#endif
    MSG msg;
    int quit = 0;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { quit = 1; continue; }
#ifdef OSK_NATIVE_HAS_WIN32_HOST
        if (osk_native_win32_handle_message(&msg)) continue;
#endif
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!s_vk.windowed) return quit ? -1 : 0;
    return (quit || s_vk.window_closed) ? -1 : 0;
#else
    if (!s_vk.windowed) return 0;
    int quit = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
#ifdef OSK_NATIVE_HAS_SDL_HOST
        /* An open dialog owns the keyboard and its own window's events. */
        if (osk_native_sdl_handle_event(&e)) continue;
#endif
        if (e.type == SDL_QUIT) quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) quit = 1;
        /* With a dialog open the game window is not the last one, so SDL
         * sends no SDL_QUIT when it closes; its own close event still ends. */
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
                 s_vk.window && e.window.windowID == SDL_GetWindowID(s_vk.window)) quit = 1;
    }
#ifdef OSK_NATIVE_HAS_SDL_HOST
    osk_native_sdl_pump();   /* queued open/close/confirm, then redraw */
#endif
#ifdef PAD_HAS_SDL_KEYBOARD
    {
        int dialog = 0;
#ifdef OSK_NATIVE_HAS_SDL_HOST
        dialog = osk_native_sdl_input_active();
#endif
        ps3recomp_pad_sdl_keyboard_sample(s_vk.window, dialog);
    }
#endif
    return quit ? -1 : 0;
#endif
}

void rsx_vulkan_backend_present(void)
{
    rsx_vulkan_backend_present_begin();
    rsx_vulkan_backend_present_finish();
}

static void vulkan_original_present_begin(void)
{
    if (!s_vk.device) return;
    s_vk.present_split = 1;
    if (s_vk.eng_active) rsx_draw_engine_present();
    else                 vk_cb_present(&s_vk, 0);
    s_vk.present_split = 0;
}

static void vulkan_original_present_finish(void)
{
    if (!s_vk.device) return;
    pthread_mutex_lock(&s_vk.present_mu);
    if (vk_defer_held_present() && s_vk.present_inflight && vk_hold_active()) {
        if (!s_show.readback_pending) { s_show.readback_pending = 1; s_show.deferred++; vk_show_report(); }
    } else {
        s_show.readback_pending = 0;
        vk_present_complete_locked();
    }
    pthread_mutex_unlock(&s_vk.present_mu);
}

u32 rsx_vulkan_backend_presented_center(void)
{
    /* Only a read-back present keeps the centre pixel. */
    return s_vk.present_readback_count ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_debug_color(void) { return s_vk.clear_argb; }

static u32 vulkan_original_readback_center(void)
{
    if (s_vk.eng_active) return rsx_draw_engine_readback_center();
    return s_vk.presented ? (0xFF000000u | s_vk.last_center) : 0u;
}

u32 rsx_vulkan_backend_guest_draws(void)
{
    return s_vk.eng_active ? rsx_draw_engine_guest_draws() : s_g.last_draws;
}

int rsx_vulkan_backend_depth_bounds(void)
{
    return s_vk.eng_active && s_vk.depth_bounds_ok;
}


int rsx_vulkan_backend_init(u32 w,u32 h,const char* title){
    if(!vulkan_owner_begin(1))return -1;
    if(vulkan_owner_enabled()&&(s_vulkan_owner.retained||s_vk.device||s_vk_frontend)){vulkan_owner_finish(0);return -1;}
    if(vulkan_owner_enabled())vulkan_owner_install_hooks();
    int result=vulkan_original_init(w,h,title);
    if(vulkan_owner_enabled()&&!result&&vulkan_owner_activate())result=-1;
    vulkan_owner_finish(0);return result;
}
void rsx_vulkan_backend_shutdown(void){
    if(!vulkan_owner_begin(1))return;
    if(vulkan_owner_enabled()){
        s_vulkan_owner.closing=1;s_vulkan_shutdown_pending=s_vk.device!=VK_NULL_HANDLE;
        if(s_vulkan_owner.retained||s_vulkan_owner.failed){vulkan_owner_finish(0);return;}
    }
    vulkan_original_shutdown();if(vulkan_owner_enabled()&&!s_vk.device)s_vulkan_shutdown_pending=0;
    vulkan_owner_finish(0);
}
void rsx_vulkan_backend_present_begin(void){
    if(!vulkan_owner_begin(1))return;
    uint64_t before=s_vulkan_commits;
    vulkan_original_present_begin();vulkan_owner_finish(vulkan_owner_boundary(before));
}
void rsx_vulkan_backend_present_finish(void){
    if(!vulkan_owner_begin(0))return;
    vulkan_original_present_finish();vulkan_owner_finish(1);
}
u32 rsx_vulkan_backend_readback_center(void){
    if(!vulkan_owner_begin(1))return 0;
    uint64_t before=s_vulkan_commits;
    u32 result=vulkan_original_readback_center();vulkan_owner_finish(vulkan_owner_boundary(before));return result;
}
static int vulkan_borrow_retain(void* user,uint64_t life){return user==&s_vulkan_owner?rsx_owner_retain(&s_vulkan_owner,&s_vulkan_owner_thread,life):-3;}
static void vulkan_borrow_release(void* user,uint64_t life){if(user==&s_vulkan_owner)(void)rsx_owner_release(&s_vulkan_owner,&s_vulkan_owner_thread,life);}
static int vulkan_borrow_begin(void* user,uint64_t life){
    if(user!=&s_vulkan_owner)return -3;
    int status=rsx_owner_native_begin(&s_vulkan_owner,&s_vulkan_owner_thread,life);if(status)return status;
    if(!s_vk.device||!s_vk.queue||atomic_load(&s_vulkan_unscoped_submit)){
        s_vulkan_owner.failed=1;rsx_owner_native_end(&s_vulkan_owner,&s_vulkan_owner_thread);return -6;}
    if(s_vulkan_partial_primitives||s_vk.frontend_recording||pthread_mutex_trylock(&s_vk.present_mu)){rsx_owner_native_end(&s_vulkan_owner,&s_vulkan_owner_thread);return -4;}
    if(pthread_mutex_trylock(&s_vk.queue_mu)){pthread_mutex_unlock(&s_vk.present_mu);rsx_owner_native_end(&s_vulkan_owner,&s_vulkan_owner_thread);return -4;}
    /* A reservation holds original work: nothing native may overtake it. */
    if(s_hold.active){pthread_mutex_unlock(&s_vk.queue_mu);pthread_mutex_unlock(&s_vk.present_mu);rsx_owner_native_end(&s_vulkan_owner,&s_vulkan_owner_thread);return -4;}
    return 0;
}
static void vulkan_borrow_end(void* user,uint64_t life){
    if(user==&s_vulkan_owner&&s_vulkan_owner_thread.gate==&s_vulkan_owner&&s_vulkan_owner_thread.native&&life==s_vulkan_owner.generation){
        pthread_mutex_unlock(&s_vk.queue_mu);pthread_mutex_unlock(&s_vk.present_mu);rsx_owner_native_end(&s_vulkan_owner,&s_vulkan_owner_thread);}
}
static rsx_native_proc vulkan_owner_instance_proc(void* instance,const char* name){return (rsx_native_proc)pvkGetInstanceProcAddr((VkInstance)instance,name);}
static rsx_native_proc vulkan_owner_device_proc(void* device,const char* name){return (rsx_native_proc)pvkGetDeviceProcAddr((VkDevice)device,name);}
static int vulkan_borrow_owner(rsx_native_owner* out,rsx_native_owner_v2 *extended){
    if(!out||out->abi_version!=1||out->struct_size!=sizeof(*out))return -1;
    if(!vulkan_owner_enabled())return -2;
    if(s_vulkan_owner_thread.gate||!rsx_owner_lock_try(&s_vulkan_owner.lock))return -4;
    int status=0;
    if(!s_vk.device||!s_vk.queue||s_vulkan_owner.closing)status=-3;
    else if(s_vk_frontend)status=-2;
    else if(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit))status=-6;
    else if(!rsx_vulkan_backend_frontend_available())status=-2;
    else{
        if(!s_vulkan_owner.generation){if(s_vulkan_lifetime==UINT64_MAX)status=-6;else s_vulkan_owner.generation=++s_vulkan_lifetime;}
        if(!status)vulkan_owner_install_hooks();
        if(!status)*out=(rsx_native_owner){.abi_version=1,.struct_size=sizeof(*out),.backend=RSX_NATIVE_OWNER_VULKAN,
            .lifetime=s_vulkan_owner.generation,.device=(void*)s_vk.device,.queue=(void*)s_vk.queue,.userdata=&s_vulkan_owner,.queue_family=s_vk.queue_family,
            .instance=(void*)s_vk.instance,.physical_device=(void*)s_vk.phys,.get_instance_proc=vulkan_owner_instance_proc,.get_device_proc=vulkan_owner_device_proc,
            .retain=vulkan_borrow_retain,.release=vulkan_borrow_release,.try_begin=vulkan_borrow_begin,.end=vulkan_borrow_end};
    }
    if(!status && extended){
        extended->base=*out;
        /* These are the exact VkDeviceCreateInfo enabled fields, not a fresh
         * physical-device query. The generation and facts share this lock. */
        extended->known_features=RSX_NATIVE_OWNER_FEATURE_BC|RSX_NATIVE_OWNER_FEATURE_ANISOTROPY|RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS;
        extended->enabled_features=(s_vk.bc_ok?RSX_NATIVE_OWNER_FEATURE_BC:0)|
            (s_vk.anisotropy_ok?RSX_NATIVE_OWNER_FEATURE_ANISOTROPY:0)|
            (s_vk.depth_bounds_ok?RSX_NATIVE_OWNER_FEATURE_DEPTH_BOUNDS:0);
    }
    rsx_owner_lock_release(&s_vulkan_owner.lock);return status;
}
int rsx_vulkan_backend_borrow_owner(rsx_native_owner *out){return vulkan_borrow_owner(out,NULL);}
int rsx_vulkan_backend_native_boundary(uint64_t *ticket){
    if(!ticket)return -1;
    *ticket=0;
    if(!vulkan_owner_enabled())return -2;
    if(!vulkan_owner_begin(0))return -4;
    int status=0;
    if(!s_vk.device||!s_vk.queue||!s_vulkan_owner.generation||s_vulkan_owner.closing)status=-3;
    else if(!s_vk.eng_active)status=-2;
    else if(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit))status=-6;
    else if(s_vulkan_partial_primitives)status=-4;
    /* Batched: what was recorded so far is submitted (not waited for) and
     * becomes the boundary native work queues behind. */
    else if(s_bat_on&&vk_bat_flush(0))status=-6;
    else if(s_vk.frontend_recording)status=-4;
    /* Reuse the real common-engine ready/checked-failure/partial-BEGIN guard.
     * The outer owner/consumer scope stays held after its nested READ ends. */
    if(!status){uint32_t method=NV4097_SET_BEGIN_END,value=0;status=rsx_draw_engine_native_registers(&method,&value,1);}
    if(!status){
        if(pthread_mutex_trylock(&s_vk.present_mu))status=-4;
        else{
            if(pthread_mutex_trylock(&s_vk.queue_mu))status=-4;
            else{
                if(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit))status=-6;
                else if(s_vulkan_owner.closing)status=-3;
                else if(s_vulkan_partial_primitives||s_vk.frontend_recording)status=-4;
                else if(!s_vulkan_boundary_commit||!s_vulkan_boundary_fence)status=-4;
                else if(s_vulkan_boundary_generation!=s_vulkan_owner.generation)status=-3;
                else{
                    /* Original work held behind a reservation reaches the
                     * queue as soon as the reserved native batch is
                     * submitted, which needs no owner lease: wait for that a
                     * bounded time rather than refuse at once. */
                    if(vk_hold_has_locked(s_vulkan_boundary_fence)){
                        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
                        until.tv_nsec+=VK_BOUNDARY_HOLD_WAIT_MS*1000000L;
                        until.tv_sec+=until.tv_nsec/1000000000L;until.tv_nsec%=1000000000L;
                        s_boundary_hold_waits++;
                        while(vk_hold_has_locked(s_vulkan_boundary_fence)&&
                              pthread_cond_timedwait(&s_hold_cv,&s_vk.queue_mu,&until)==0){}
                    }
                    VkResult result=vk_hold_has_locked(s_vulkan_boundary_fence)?VK_NOT_READY:pvkGetFenceStatus(s_vk.device,s_vulkan_boundary_fence);
                    if(result==VK_NOT_READY&&vk_hold_has_locked(s_vulkan_boundary_fence)&&
                       (++s_boundary_hold_refusals<=8||s_boundary_hold_refusals%256==0))
                        VK_LOG("native boundary: original work still held behind a reservation after %d ms (refusals=%llu waits=%llu)\n",
                               VK_BOUNDARY_HOLD_WAIT_MS,(unsigned long long)s_boundary_hold_refusals,(unsigned long long)s_boundary_hold_waits);
                    /* An asynchronous completion ticket (E5) is an ordered,
                     * submitted boundary even before it signals, as Metal's
                     * async boundary is: native work queued after it runs
                     * after it, and native passes order their imported
                     * images against all earlier queue work themselves. */
                    if(result==VK_NOT_READY&&vk_e5_is_ticket_fence(s_vulkan_boundary_fence))result=VK_SUCCESS;
                    /* So is a direct present still running: it is on the queue
                     * (never held here) and reads only the offscreen target. */
                    if(result==VK_NOT_READY&&vk_present_is_fence(s_vulkan_boundary_fence))result=VK_SUCCESS;
                    /* And a present thread's stage on the queue: it reads the
                     * flipped surface and writes only its staging image. */
                    if(result==VK_NOT_READY&&!vk_hold_has_locked(s_vulkan_boundary_fence)&&
                       vk_pt_is_stage_fence(s_vulkan_boundary_fence))result=VK_SUCCESS;
                    if(result==VK_NOT_READY)status=-4;
                    else if(result!=VK_SUCCESS){vulkan_owner_fault();status=-6;}
                    else{
                        *ticket=s_vulkan_boundary_commit;
                        /* Original drawing is synchronous; the exact latest
                         * submitted fence (complete, or a pending E5 ticket)
                         * plus common quiescence is the ordered boundary. */
                        rsx_owner_submitted(&s_vulkan_owner,&s_vulkan_owner_thread,1);
                    }
                }
                pthread_mutex_unlock(&s_vk.queue_mu);
            }
            pthread_mutex_unlock(&s_vk.present_mu);
        }
    }
    if(status==-6)vulkan_owner_fault();
    vulkan_owner_finish(1);return status;
}
int rsx_vulkan_backend_borrow_owner_v2(rsx_native_owner_v2 *out){
    if(!out||out->abi_version!=2||out->struct_size!=sizeof(*out))return -1;
    rsx_native_owner base={.abi_version=1,.struct_size=sizeof(base)};
    return vulkan_borrow_owner(&base,out);
}
int rsx_vulkan_backend_owner_shutdown_requested(uint64_t life){
    if(!vulkan_owner_enabled())return -2;
    if(!vulkan_owner_begin(0))return -4;
    int status=life&&life==s_vulkan_owner.generation?s_vulkan_shutdown_pending:-3;vulkan_owner_finish(1);return status;
}
int rsx_vulkan_backend_owner_retry_shutdown(uint64_t life){
    if(!vulkan_owner_enabled())return -2;
    if(!vulkan_owner_begin(0))return -4;
    int status=0;
    if(!life||life!=s_vulkan_owner.generation)status=-3;
    else if(!s_vulkan_shutdown_pending)status=0;
    else if(s_vulkan_owner.retained)status=-4;
    else if(s_vulkan_owner.failed)status=-6;
    else{vulkan_original_shutdown();status=s_vk.device?-6:1;if(status==1)s_vulkan_shutdown_pending=0;}
    vulkan_owner_finish(status==1?0:1);return status;
}
static int vulkan_image_identity(uint64_t life,uint64_t id,uint64_t generation,int retired_ok){
    if(!life||life!=s_vulkan_owner.generation||!id||id>=VK_ENG_MAX_OBJ||!generation||
       s_eobj_generation[id]!=generation||!s_eobj[id].im.img||
       (s_eobj[id].kind!=VK_ENG_COLOR&&s_eobj[id].kind!=VK_ENG_DEPTH)||
       (!retired_ok&&s_eobj_retired[id]))return -3;
    return 0;
}
static int vulkan_image_retain(void *user,uint64_t life,uint64_t id,uint64_t generation){
    if(user!=&s_vulkan_owner)return -3;
    if(s_vulkan_owner_thread.gate||!rsx_owner_lock_try(&s_vulkan_owner.lock))return -4;
    int status=vulkan_image_identity(life,id,generation,0);
    if(!status&&s_vulkan_owner.closing)status=-3;
    if(!status&&(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit)))status=-6;
    if(!status&&(s_vulkan_owner.retained==UINT_MAX||s_eobj_native_pins[id]==UINT_MAX))status=-4;
    if(!status){++s_vulkan_owner.retained;++s_eobj_native_pins[id];}
    rsx_owner_lock_release(&s_vulkan_owner.lock);return status;
}
static void vulkan_image_release(void *user,uint64_t life,uint64_t id,uint64_t generation){
    if(user!=&s_vulkan_owner||s_vulkan_owner_thread.gate)return;
    rsx_owner_lock_acquire(&s_vulkan_owner.lock);
    if(!vulkan_image_identity(life,id,generation,1)&&s_eobj_native_pins[id]&&s_vulkan_owner.retained){
        --s_eobj_native_pins[id];--s_vulkan_owner.retained;
        if(!s_eobj_native_pins[id]&&s_eobj_retired[id]){
            /* Original release already drained original presentation users;
             * the final native pin is released after genuine native fences.
             * Never call vk_destroy_image here: it can wait for presentation. */
            vk_image *image=&s_eobj[id].im;
            const u64 ticket=s_eobj_pin_ticket[id];s_eobj_pin_ticket[id]=0;
            if(ticket){
                /* Released without waiting: the room was reserved then. */
                pthread_mutex_lock(&s_pin_retire_mu);s_pin_retire_reserved--;
                if(ticket>__atomic_load_n(&s_e5.completed,__ATOMIC_ACQUIRE)){
                    s_pin_retire[s_pin_retire_n].im=*image;s_pin_retire[s_pin_retire_n].ticket=ticket;
                    s_pin_retire_n++;s_pin_deferred++;memset(image,0,sizeof *image);
                }else s_pin_immediate++;
                pthread_mutex_unlock(&s_pin_retire_mu);
            }
            if(image->view)pvkDestroyImageView(s_vk.device,image->view,NULL);
            if(image->img)pvkDestroyImage(s_vk.device,image->img,NULL);
            if(image->mem)pvkFreeMemory(s_vk.device,image->mem,NULL);
            memset(&s_eobj[id],0,sizeof s_eobj[id]);
            memset(&s_eobj_native_image[id],0,sizeof s_eobj_native_image[id]);
            s_eobj_retired[id]=0;
        }
    }
    rsx_owner_lock_release(&s_vulkan_owner.lock);
}
static int vulkan_image_validate(void *user,uint64_t life,uint64_t id,uint64_t generation){
    if(user!=&s_vulkan_owner)return -3;
    if(s_vulkan_owner_thread.gate!=&s_vulkan_owner||!s_vulkan_owner_thread.native)return -4;
    if(s_vulkan_owner.closing)return -3;
    if(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit))return -6;
    return vulkan_image_identity(life,id,generation,0);
}
int rsx_vulkan_backend_validate_image_planes(void *user,uint64_t life,uint64_t id,uint64_t generation,uint32_t required){
    if(!required||(required&~7u)||id>UINT32_MAX)return -1;
    int status=vulkan_image_validate(user,life,id,generation);if(status)return status;
    return rsx_draw_engine_native_image_clear_planes((uint32_t)id,required);
}
static uint32_t vulkan_native_image_format(VkFormat format){
    switch(format){
        case VK_FORMAT_R8G8B8A8_UNORM:return RSX_NATIVE_IMAGE_RGBA8;
        case VK_FORMAT_B8G8R8A8_UNORM:return RSX_NATIVE_IMAGE_BGRA8;
        case VK_FORMAT_R16G16B16A16_SFLOAT:return RSX_NATIVE_IMAGE_RGBA16F;
        case VK_FORMAT_R32G32B32A32_SFLOAT:return RSX_NATIVE_IMAGE_RGBA32F;
        case VK_FORMAT_R32_SFLOAT:return RSX_NATIVE_IMAGE_R32F;
        case VK_FORMAT_D32_SFLOAT:return RSX_NATIVE_IMAGE_D32F;
        case VK_FORMAT_D24_UNORM_S8_UINT:return RSX_NATIVE_IMAGE_D24S8;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:return RSX_NATIVE_IMAGE_D32S8;
        default:return 0;
    }
}
int rsx_vulkan_backend_borrow_image(uint64_t life,uint32_t handle,rsx_native_image *out){
    if(!out||out->abi_version!=RSX_NATIVE_IMAGE_ABI||out->struct_size!=sizeof(*out))return -1;
    if(!vulkan_owner_enabled())return -2;
    if(!vulkan_owner_begin(0))return -4;
    int status=0;
    if(!s_vk.device||s_vulkan_owner.closing||!handle||handle>=VK_ENG_MAX_OBJ||
       vulkan_image_identity(life,handle,s_eobj_generation[handle],0))status=-3;
    else if(s_vulkan_owner.failed||atomic_load(&s_vulkan_unscoped_submit))status=-6;
    else {
        vk_eng_obj *object=&s_eobj[handle];vk_image *image=&object->im;
        uint32_t format=vulkan_native_image_format(image->format);
        if(!format||image->format!=object->fmt||image->levels!=1||image->layers!=1||
           image->samples!=VK_SAMPLE_COUNT_1_BIT)status=-2;
        if(!status){
            rsx_native_vulkan_image *payload=&s_eobj_native_image[handle];
            if(!payload->abi_version){
                *payload=(rsx_native_vulkan_image){.abi_version=RSX_NATIVE_VULKAN_IMAGE_ABI,.struct_size=sizeof(*payload),
                    .format=image->format,.usage=image->usage,.aspect=image->aspect,.mip_count=image->levels,
                    .layers=image->layers,.samples=image->samples,.queue_family=s_vk.queue_family,.layout=VK_IMAGE_LAYOUT_GENERAL};
                _Static_assert(sizeof(image->img)<=sizeof(payload->image_bits),"Vulkan opaque handle transport");
                memcpy(&payload->image_bits,&image->img,sizeof(image->img));
            }
            *out=(rsx_native_image){.abi_version=RSX_NATIVE_IMAGE_ABI,.struct_size=sizeof(*out),
                .backend=RSX_NATIVE_OWNER_VULKAN,.format=format,.width=object->w,.height=object->h,
                .owner_lifetime=life,.resource_id=handle,.resource_generation=s_eobj_generation[handle],
                .image=payload,.userdata=&s_vulkan_owner,.retain=vulkan_image_retain,.release=vulkan_image_release,.validate=vulkan_image_validate};
        }
    }
    vulkan_owner_finish(1);return status;
}
/* Reserved queue order, see vk_queue_submit. reserve: under the native lease
 * only (try_begin held on this thread). submit: any thread, without it. */
static int vulkan_order_reserve(void* user, uint64_t life, uint64_t* token)
{
    if (!token) return -1;
    *token = 0;
    if (user != &s_vulkan_owner || !life || life != s_vulkan_owner.generation) return -3;
    /* The lease holds queue_mu on this thread: hold state is ours to change. */
    if (s_vulkan_owner_thread.gate != &s_vulkan_owner || !s_vulkan_owner_thread.native) return -4;
    if (s_vulkan_owner.failed || atomic_load(&s_vk_eng_failed)) return -6;
    if (s_hold.active) return -4;
    if (s_hold.next_token == UINT64_MAX) return -6;
    s_hold.active = 1; s_hold.token = ++s_hold.next_token; s_hold.reservations++;
    *token = s_hold.token;
    return 0;
}
static int vulkan_order_submit(void* user, uint64_t life, uint64_t token, const void* submit_info, void* fence)
{
    if (user != &s_vulkan_owner || !life || life != s_vulkan_owner.generation) return -3;
    pthread_mutex_lock(&s_vk.queue_mu);
    if (!s_hold.active || !token || token != s_hold.token) { pthread_mutex_unlock(&s_vk.queue_mu); return -3; }
    VkResult r = VK_SUCCESS;
    if (submit_info) {
        r = pvkQueueSubmit(s_vk.queue, 1, (const VkSubmitInfo*)submit_info, (VkFence)fence);
        if (r != VK_SUCCESS) s_hold.failures++;
    } else s_hold.abandoned++;
    vk_hold_release_locked();            /* after the native batch, whatever its result */
    pthread_mutex_unlock(&s_vk.queue_mu);
    return (int)r;
}
int rsx_vulkan_backend_queue_order(rsx_native_queue_order* out)
{
    if (!out || out->abi_version != RSX_NATIVE_QUEUE_ORDER_ABI || out->struct_size != sizeof(*out)) return -1;
    if (!vulkan_owner_enabled()) return -2;
    if (!s_vk.device || !s_vk.queue || !s_vulkan_owner.generation || s_vulkan_owner.closing) return -3;
    *out = (rsx_native_queue_order){ .abi_version = RSX_NATIVE_QUEUE_ORDER_ABI, .struct_size = sizeof(*out),
        .lifetime = s_vulkan_owner.generation, .userdata = &s_vulkan_owner,
        .reserve = vulkan_order_reserve, .submit = vulkan_order_submit };
    return 0;
}
#endif /* !_WIN32 || PS3RECOMP_RSX_VULKAN */
