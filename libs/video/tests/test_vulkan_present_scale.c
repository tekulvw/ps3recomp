/* rsx_vulkan_backend_set_present_scale, headless under the Khronos
 * validation layer: the display image (and the present thread's staging
 * images) takes the requested percentage of the display size, a flipped
 * colour target of that size reaches it pixel for pixel, and the read-back
 * present reads it whole. With 100 everything keeps the display size; on the
 * fixed-function path the request is refused and the display size kept.
 *
 * argv[1]: the percentage (100..400); argv[2]: "direct" (the engine path's
 * default present), "readback" (PS3RECOMP_VK_PRESENT_READBACK=1), "thread"
 * (PS3RECOMP_RSX_PRESENT_THREAD=1) or "fixed" (no guest programs, so the
 * fixed-function path). No window, guest memory or game data.
 * python3 tools/test_vulkan_present_scale.py */
#ifdef NDEBUG
#undef NDEBUG
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "../rsx_vulkan_backend.c"
#include <assert.h>
#include <time.h>
u8* vm_base;u32 ppu_vm_size,ppu_hle_inject_base;
u32 cellGcmResolveLocated(int local,u32 offset){(void)local;(void)offset;assert(!"fixture has no guest memory");return 0;}
u32 cellGcmResolveIO(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
u32 gcm_io_offset_to_ea(u32 offset){(void)offset;assert(!"fixture has no guest IO");return 0;}
void vm_write32(uint32_t a,uint32_t v){(void)a;(void)v;assert(!"fixture cannot publish guest memory");}
void ps3_ms(const char* key){(void)key;}
unsigned long long ps3_ms_now(void){return 0;}
void cellGcm_replay_display_buffers_to_engine(void){}
u32 cellGcm_flip_request_count(void){return 0;}
void cellGcm_fifo_kick_notify(void){}
rsx_frontend_device* rsx_frontend_vulkan_create(const rsx_frontend_vulkan_owner* owner){(void)owner;assert(!"legacy frontend must not be created");return NULL;}
void rsx_frontend_close(rsx_frontend_device* device){assert(!device);}

static int failures, checks;
#define CHECK(v, name) do { checks++; if (!(v)) { failures++; fprintf(stderr, "FAIL %s\n", name); } } while (0)

/* A display small enough to compare whole, with sides 150% does not divide
 * evenly (45 -> 67.5 -> 68), so the rounding is exercised too. */
enum { DW = 64, DH = 45 };

static u32 scaled(u32 n, u32 percent) { return (u32)(((u64)n * percent + 50u) / 100u); }

static void pattern(u8* p, u32 x, u32 y)
{
    p[0] = (u8)x; p[1] = (u8)y; p[2] = (u8)(x * 7u + y * 13u); p[3] = 0xFF;
}

/* The display image's bytes, after every present submitted so far. */
static void read_display(u8* out)
{
    vk_present_wait_gpu();
    vk_device_idle();
    const VkDeviceSize bytes = (VkDeviceSize)s_vk.out_w * s_vk.out_h * 4u;
    vk_buffer st = {0};
    assert(vk_create_host_buffer(&st, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);
    assert(vk_begin() == 0);
    vk_barrier_image(s_vk.color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { s_vk.out_w, s_vk.out_h, 1 } };
    pvkCmdCopyImageToBuffer(s_vk.cmd, s_vk.color.img, VK_IMAGE_LAYOUT_GENERAL, st.buf, 1, &region);
    VkBufferMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = st.buf, .offset = 0, .size = VK_WHOLE_SIZE };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &hb, 0, NULL);
    assert(vk_sync() == 0);
    vk_invalidate(&st);
    memcpy(out, st.ptr, (size_t)bytes);
    vk_destroy_buffer_now(&st);
}

static void wait_shown(uint64_t want)
{
    for (int i = 0; i < 2000; i++) {
        rsx_present_thread_stats st;
        rsx_present_thread_get_stats(s_vpt.pt, &st);
        if (st.presented >= want) return;
        struct timespec t = { 0, 1000000 };
        nanosleep(&t, NULL);
    }
    CHECK(0, "the present thread shows the staged frame");
}

int main(int argc, char** argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s percent direct|readback|thread|fixed\n", argv[0]); return 2; }
    const u32 percent = (u32)strtoul(argv[1], NULL, 10);
    const char* mode = argv[2];
    const int fixed = !strcmp(mode, "fixed"), thread = !strcmp(mode, "thread");
    const int readback = !strcmp(mode, "readback");
    if (!fixed && !thread && !readback && strcmp(mode, "direct")) return 2;

    CHECK(rsx_vulkan_backend_set_present_scale(99) == -1, "a percentage under 100 is refused");
    CHECK(rsx_vulkan_backend_set_present_scale(401) == -1, "a percentage over 400 is refused");
    CHECK(rsx_vulkan_backend_set_present_scale(percent) == 0, "the percentage is accepted");

    setenv("PS3RECOMP_VK_WINDOW", "0", 1);
    if (fixed) { unsetenv("PS3RECOMP_VK_GUEST_PROGRAMS"); setenv("PS3RECOMP_RSX_ENGINE", "vtable", 1); }
    else { setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1); setenv("PS3RECOMP_RSX_ENGINE", "dispatch", 1); }
    if (readback) setenv("PS3RECOMP_VK_PRESENT_READBACK", "1", 1); else unsetenv("PS3RECOMP_VK_PRESENT_READBACK");
    if (thread) setenv("PS3RECOMP_RSX_PRESENT_THREAD", "1", 1); else unsetenv("PS3RECOMP_RSX_PRESENT_THREAD");
    if (rsx_vulkan_backend_init(DW, DH, "display extent synthetic")) {
        fprintf(stderr, "no Vulkan device\n");
        return 1;
    }
    const u32 ow = fixed ? DW : scaled(DW, percent), oh = fixed ? DH : scaled(DH, percent);
    CHECK(s_vk.width == DW && s_vk.height == DH, "the display size stays the guest's");
    CHECK(s_vk.out_w == ow && s_vk.out_h == oh, "the display image takes the requested extent");
    CHECK(s_vk.color.w == ow && s_vk.color.h == oh, "the display image is created at that extent");
    CHECK(s_vk.readback.size >= (VkDeviceSize)ow * oh * 4u, "the readback buffer holds the whole display image");
    if (fixed) {
        CHECK(!s_vk.eng_active, "fixed-function path");
        rsx_vulkan_backend_shutdown();
        printf("present scale %u%% %s: display image %ux%u; %d checks, %d failures\n",
               percent, mode, ow, oh, checks, failures);
        return failures ? 1 : 0;
    }
    CHECK(s_vk.eng_active, "register-file draw engine path");
    CHECK(thread == (s_vpt.pt != NULL), "the present thread runs only when asked for");

    /* A colour target of the display image's extent, every pixel distinct. */
    u8* seed = malloc((size_t)ow * oh * 4u);
    u8* got = malloc((size_t)ow * oh * 4u);
    assert(seed && got);
    for (u32 y = 0; y < oh; y++) for (u32 x = 0; x < ow; x++) pattern(seed + ((size_t)y * ow + x) * 4u, x, y);
    const u32 rt = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, ow, oh, seed, ow * 4u);
    CHECK(rt != 0, "a display-image-sized colour target");
    vk_eng_submit_and_wait(NULL, 0);

    vk_eng_present(NULL, rt);
    if (thread) wait_shown(1);
    if (readback) {
        pthread_mutex_lock(&s_vk.present_mu);
        vk_present_complete_locked();
        pthread_mutex_unlock(&s_vk.present_mu);
        u8 c[4]; pattern(c, ow / 2, oh / 2);
        CHECK(s_vk.last_center == (((u32)c[0] << 16) | ((u32)c[1] << 8) | c[2]),
              "the read-back present reads the centre of the whole display image");
    }
    if (thread) {
        u32 staged = 0;
        for (u32 i = 0; i < RSX_PRESENT_THREAD_MAX_DEPTH; i++)
            if (s_vpt.img[i].img) {
                staged++;
                CHECK(s_vpt.img_w[i] == ow && s_vpt.img_h[i] == oh, "staging images take the display image's extent");
            }
        CHECK(staged > 0, "the frame went through a staging image");
    }
    read_display(got);
    CHECK(!memcmp(got, seed, (size_t)ow * oh * 4u), "the flipped target reaches the display image pixel for pixel");

    /* A display-sized target still fills the whole (larger) image. */
    const u32 small = vk_eng_color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, DW, DH, seed, ow * 4u);
    CHECK(small != 0, "a display-sized colour target");
    vk_eng_present(NULL, small);
    if (thread) wait_shown(2);
    if (readback) {
        pthread_mutex_lock(&s_vk.present_mu);
        vk_present_complete_locked();
        pthread_mutex_unlock(&s_vk.present_mu);
    }
    read_display(got);
    const u8* corner = got + ((size_t)(oh - 1) * ow + (ow - 1)) * 4u;
    CHECK(corner[0] >= DW - 2 && corner[1] >= DH - 2, "a display-sized target is stretched over the whole image");

    vk_eng_release(NULL, small);
    vk_eng_release(NULL, rt);
    free(seed); free(got);
    rsx_vulkan_backend_shutdown();
    printf("present scale %u%% %s: display image %ux%u; %d checks, %d failures\n",
           percent, mode, ow, oh, checks, failures);
    return failures ? 1 : 0;
}
