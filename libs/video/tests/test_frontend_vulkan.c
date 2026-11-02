/* Source-bound headless owner fixture. No game, window or original draw path. */
#include "../rsx_vulkan_backend.c"
#include <assert.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "frontend_shader_fixture.h"

typedef struct transform_bytes { float matrix[16]; uint32_t counts[4]; } transform_bytes;
typedef struct candidate_bytes { float value[4]; uint32_t keep, padding[3]; } candidate_bytes;
_Static_assert(sizeof(transform_bytes) == 80, "transform layout");
_Static_assert(sizeof(candidate_bytes) == 32, "candidate stride");

static rsx_frontend_buffer make_buffer(rsx_frontend_device *device, const void *bytes, uint64_t length) {
    rsx_frontend_buffer_desc desc = {sizeof(desc), 1, length, bytes}; rsx_frontend_buffer ref;
    assert(rsx_frontend_buffer_create(device, &desc, &ref) == 0); return ref;
}
static void release_buffer(rsx_frontend_device *device, rsx_frontend_buffer ref) { assert(rsx_frontend_buffer_release(device, ref) == 0); }
static rsx_frontend_pipeline make_pipeline(rsx_frontend_device *device, const shader_fixture *shader) {
    rsx_frontend_binding_layout bindings[16]; memcpy(bindings, shader->bindings, sizeof(*bindings) * shader->count);
    rsx_frontend_pipeline_desc desc = {0}; desc.struct_size = sizeof(desc); desc.abi_version = 1;
    desc.shader_language = RSX_FRONTEND_SPIRV; desc.source_bytes = shader->source; desc.source_length = shader->length;
    desc.entry_point = shader->entry; desc.entry_point_length = (uint32_t)strlen(shader->entry);
    desc.bindings = bindings; desc.binding_count = shader->count; memcpy(desc.threads_per_group, shader->threads, sizeof(desc.threads_per_group));
    rsx_frontend_pipeline pipeline, unused;
    assert(rsx_frontend_pipeline_create(device, &desc, &pipeline) == 0);
    desc.shader_language = RSX_FRONTEND_MSL;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_UNSUPPORTED && !unused.id);
    desc.shader_language = RSX_FRONTEND_SPIRV;
    uint32_t alignment = bindings[0].alignment_bytes; bindings[0].alignment_bytes = 3;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_INVALID); bindings[0].alignment_bytes = alignment;
    bindings[0].minimum_bytes += bindings[0].runtime_array_stride;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_UNSUPPORTED);
    bindings[0].minimum_bytes -= bindings[0].runtime_array_stride;
    desc.threads_per_group[0] = shader->threads[0] == 1 ? 2 : 1;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_INVALID);
    return pipeline;
}
static rsx_frontend_frame make_frame(rsx_frontend_pipeline p, const rsx_frontend_binding *bindings, uint32_t count) {
    return (rsx_frontend_frame){sizeof(rsx_frontend_frame), 1, p, bindings, count, {1,1,1}};
}
static double now(void) { struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value); return value.tv_sec + value.tv_nsec / 1e9; }
static void drain(rsx_frontend_device *device, const rsx_frontend_ticket *tickets, uint32_t expected) {
    uint32_t seen = 0; double deadline = now() + 5;
    while (seen != (1u << expected) - 1) {
        rsx_frontend_event events[2]; uint32_t count;
        assert(rsx_frontend_poll(device, events, 2, &count) == 0);
        for (uint32_t i = 0; i < count; ++i) {
            assert(events[i].succeeded && events[i].no_further_gpu_access && events[i].native_status == VK_SUCCESS);
            uint32_t match;
            for (match = 0; match < expected; ++match)
                if (events[i].ticket.device_generation == tickets[match].device_generation && events[i].ticket.submission_id == tickets[match].submission_id) break;
            assert(match < expected && !(seen & (1u << match))); seen |= 1u << match;
        }
        assert(now() < deadline); if (!count) usleep(1000);
    }
    rsx_frontend_event events[2]; uint32_t count;
    assert(rsx_frontend_poll(device, events, 2, &count) == 0 && !count);
}
static void equal_floats(const float *actual, const float *expected, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) assert(fabsf(actual[i] - expected[i]) < 0.00001f);
}
static void transform_test(rsx_frontend_device *device) {
    rsx_frontend_pipeline p = make_pipeline(device, &copy_transform_fixture);
    const float input[12] = {1,2,3,1,4,5,6,1,7,8,9,1};
    const float sentinel[8] = {-99,-99,-99,-99,-99,-99,-99,-99};
    const float expected[8] = {12,26,42,1,18,35,54,1};
    transform_bytes parameters = {{2,0,0,0,0,3,0,0,0,0,4,0,10,20,30,1},{0}};
    rsx_frontend_buffer src = make_buffer(device, input, sizeof(input));
    rsx_frontend_buffer dst = make_buffer(device, sentinel, sizeof(sentinel));
    rsx_frontend_buffer uniform = make_buffer(device, &parameters, sizeof(parameters));
    rsx_frontend_binding bindings[3] = {{0,0,src,0,sizeof(input)}, {1,0,dst,0,sizeof(sentinel)}, {2,0,uniform,0,sizeof(parameters)}};
    rsx_frontend_frame frame = make_frame(p, bindings, 3); rsx_frontend_ticket ticket;
    assert(rsx_frontend_submit(device, &frame, &ticket) == 0); drain(device, &ticket, 1);
    float output[8]; assert(rsx_frontend_buffer_read(device, dst, 0, output, sizeof(output)) == 0); equal_floats(output, sentinel, 8);
    release_buffer(device, uniform); parameters.counts[0] = UINT32_MAX;
    uniform = make_buffer(device, &parameters, sizeof(parameters)); bindings[2].buffer = uniform;
    s_vk.frontend_recording = 1;
    assert(rsx_frontend_submit(device, &frame, &ticket) == RSX_FRONTEND_BUSY && !ticket.submission_id); s_vk.frontend_recording = 0;
    pthread_mutex_lock(&s_vk.present_mu);
    assert(rsx_frontend_submit(device, &frame, &ticket) == RSX_FRONTEND_BUSY); pthread_mutex_unlock(&s_vk.present_mu);
    unsigned char padding[96] = {0}; rsx_frontend_buffer bad = make_buffer(device, padding, sizeof(padding));
    bindings[2].buffer = bad; bindings[2].offset = 4;
    assert(rsx_frontend_submit(device, &frame, &ticket) == RSX_FRONTEND_INVALID && !ticket.submission_id);
    bindings[2].buffer = uniform; bindings[2].offset = 0; release_buffer(device, bad);

    PFN_vkSignalSemaphoreKHR signal_gate = (PFN_vkSignalSemaphoreKHR)pvkGetDeviceProcAddr(s_vk.device, "vkSignalSemaphoreKHR");
    assert(signal_gate);
    VkSemaphore gate;
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo event_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    assert(pvkCreateSemaphore(s_vk.device, &event_info, NULL, &gate) == VK_SUCCESS);
    VkCommandPool pool; VkCommandBuffer command;
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = s_vk.queue_family};
    assert(pvkCreateCommandPool(s_vk.device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    assert(pvkAllocateCommandBuffers(s_vk.device, &allocation, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(pvkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
    /* This prior barrier participates in the following frontend command's
     * ALL_COMMANDS dependency. Its queue wait is a valid host timeline signal. */
    pvkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    assert(pvkEndCommandBuffer(command) == VK_SUCCESS);
    const uint64_t wait_value = 1; VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &wait_value};
    VkSubmitInfo prior = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &timeline,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &gate, .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1, .pCommandBuffers = &command};
    assert(vk_queue_submit(&prior, VK_NULL_HANDLE) == VK_SUCCESS);
    rsx_frontend_ticket pending[2];
    assert(rsx_frontend_submit(device, &frame, &pending[0]) == 0);
    assert(rsx_frontend_buffer_read(device, dst, 0, output, sizeof(output)) == RSX_FRONTEND_BUSY);
    rsx_frontend_buffer dst2 = make_buffer(device, sentinel, sizeof(sentinel)); bindings[1].buffer = dst2;
    assert(rsx_frontend_submit(device, &frame, &pending[1]) == 0);
    assert(pending[1].submission_id > pending[0].submission_id);
    assert(rsx_frontend_submit(device, &frame, &ticket) == RSX_FRONTEND_BUSY && !ticket.submission_id);
    rsx_frontend_event events[2]; uint32_t count;
    assert(rsx_frontend_poll(device, events, 2, &count) == 0 && !count);
    const float changed[12] = {9,9,9,9,9,9,9,9,9,9,9,9};
    rsx_frontend_buffer_desc update = {sizeof(update),1,sizeof(changed),changed}; rsx_frontend_buffer newer;
    assert(rsx_frontend_buffer_update(device, src, &update, &newer) == 0 && newer.version > src.version && newer.id == src.id);
    release_buffer(device, src); assert(rsx_frontend_buffer_release(device, src) == RSX_FRONTEND_STALE);
    assert(rsx_frontend_pipeline_release(device, p) == 0);
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    assert(signal_gate(s_vk.device, &signal) == VK_SUCCESS); drain(device, pending, 2);
    assert(rsx_frontend_buffer_read(device, dst, 0, output, sizeof(output)) == 0); equal_floats(output, expected, 8);
    assert(rsx_frontend_buffer_read(device, dst2, 0, output, sizeof(output)) == 0); equal_floats(output, expected, 8);
    pvkDestroySemaphore(s_vk.device, gate, NULL); pvkDestroyCommandPool(s_vk.device, pool, NULL);
    release_buffer(device, newer); release_buffer(device, dst); release_buffer(device, dst2); release_buffer(device, uniform);
    puts("PASS Vulkan transform: zero/oversized bounds, exact outputs, original/present ordering gates, delayed two-ticket retention and immutable CPU publication");
}
static void compact_test(rsx_frontend_device *device) {
    rsx_frontend_pipeline p = make_pipeline(device, &compact_fixture);
    const candidate_bytes input[4] = {{{1,2,3,1},1,{0}},{{4,5,6,1},0,{0}},{{7,8,9,1},7,{0}},{{10,11,12,1},1,{0}}};
    const float sentinel[8] = {-99,-99,-99,-99,-99,-99,-99,-99}; const float expected[8] = {1,2,3,1,7,8,9,1};
    for (uint32_t test = 0; test < 3; ++test) {
        uint32_t parameters[4] = {test == 0 ? 0 : test == 1 ? 4 : UINT32_MAX,0,0,0}, counters[4] = {99,99,99,99};
        rsx_frontend_buffer refs[4] = {make_buffer(device,input,sizeof(input)), make_buffer(device,sentinel,sizeof(sentinel)),
            make_buffer(device,counters,sizeof(counters)), make_buffer(device,parameters,sizeof(parameters))};
        rsx_frontend_binding bindings[4] = {{0,0,refs[0],0,sizeof(input)}, {1,0,refs[1],0,sizeof(sentinel)},
            {2,0,refs[2],0,sizeof(counters)}, {3,0,refs[3],0,sizeof(parameters)}};
        rsx_frontend_frame frame = make_frame(p, bindings, 4); rsx_frontend_ticket ticket;
        assert(rsx_frontend_submit(device, &frame, &ticket) == 0); drain(device, &ticket, 1);
        float values[8]; assert(rsx_frontend_buffer_read(device,refs[1],0,values,sizeof(values)) == 0); equal_floats(values,test ? expected : sentinel,8);
        assert(rsx_frontend_buffer_read(device,refs[2],0,counters,sizeof(counters)) == 0);
        assert(counters[0] == (test ? 2 : 0) && counters[1] == parameters[0] && counters[2] == (test ? 4 : 0) && counters[3] == (test ? 1 : 0));
        for (unsigned i = 0; i < 4; ++i) release_buffer(device,refs[i]);
    }
    assert(rsx_frontend_pipeline_release(device,p) == 0);
    puts("PASS Vulkan stable compaction: selected order, output truncation, exact counters, zero and oversized batches");
}

static void owner_init(void)
{
    memset(&s_vk, 0, sizeof(s_vk));
    pthread_mutex_init(&s_vk.queue_mu, NULL); pthread_mutex_init(&s_vk.present_mu, NULL);
    assert(vk_load_library() == 0);
    VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &application};
    const char *validation = "VK_LAYER_KHRONOS_validation";
    if (getenv("MAG_FRONTEND_REQUIRE_VALIDATION")) {
        instance.enabledLayerCount = 1; instance.ppEnabledLayerNames = &validation;
        puts("Vulkan validation layer explicitly required");
    }
    assert(pvkCreateInstance(&instance, NULL, &s_vk.instance) == VK_SUCCESS);
    assert(vk_load_instance_functions() == 0 && vk_pick_device() == 0);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s_vk.queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    /* Timeline signaling is enabled only on this explicit fixture device.
     * Ordinary SDK backend requirements remain Vulkan1.1 without timeline. */
    assert(vk_has_device_extension(s_vk.phys, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME));
    PFN_vkGetPhysicalDeviceFeatures2 query = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(s_vk.instance, "vkGetPhysicalDeviceFeatures2");
    assert(query);
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline};
    query(s_vk.phys, &features); assert(timeline.timelineSemaphore);
    const char *extension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue, .pNext = &timeline, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &extension};
    assert(pvkCreateDevice(s_vk.phys, &device, NULL, &s_vk.device) == VK_SUCCESS);
    assert(vk_load_device_functions() == 0);
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
}
int main(int argc, char **argv)
{
    assert(argc == 4 && "specify hardware-required, expected vendor and device IDs");
    assert(!rsx_vulkan_backend_frontend_available()); owner_init();
    VkPhysicalDeviceProperties properties; pvkGetPhysicalDeviceProperties(s_vk.phys, &properties);
    int hardware = atoi(argv[1]);
    if (hardware) {
        assert(properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        assert(properties.vendorID == strtoul(argv[2], NULL, 0));
        assert(properties.deviceID == strtoul(argv[3], NULL, 0));
    }
    printf("Vulkan device: %s vendor=0x%x device=0x%x type=%u api=%u.%u.%u hardware_required=%d\n",
        properties.deviceName, properties.vendorID, properties.deviceID, properties.deviceType,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion), hardware);
    assert(!rsx_vulkan_backend_frontend_available()); /* opt-in remains explicit */
    s_vk.frontend_enabled = 1;
    assert(rsx_vulkan_backend_frontend_available());
    rsx_frontend_device *frontend = rsx_vulkan_backend_frontend();
    assert(frontend && "explicit Vulkan frontend implementation is required");
    rsx_frontend_caps_desc caps = {sizeof(caps), 1};
    assert(rsx_frontend_caps(frontend, &caps) == 0 && caps.compute_supported && !caps.draws_supported && caps.max_pending == 2);
    transform_test(frontend); compact_test(frontend);
    rsx_frontend_close(s_vk_frontend); s_vk_frontend = NULL;
    vk_device_idle(); pvkDestroyDevice(s_vk.device, NULL); pvkDestroyInstance(s_vk.instance, NULL); dlclose(s_vk.lib);
    pthread_mutex_destroy(&s_vk.queue_mu); pthread_mutex_destroy(&s_vk.present_mu);
    puts(hardware ? "PASS real hardware Vulkan compute on explicit shared owner" : "PASS software/unspecified Vulkan diagnostics only; hardware acceptance pending");
    return 0;
}
