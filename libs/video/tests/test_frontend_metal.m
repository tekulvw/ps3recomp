/* Synthetic GPU contract test, no guest or window. */
#include "../rsx_metal_backend.m"
#include "../rsx_frontend.c"
#include "../rsx_frontend_metal.m"
#include <assert.h>
#include <math.h>
#include <unistd.h>

typedef struct TransformBytes { float matrix[16]; uint32_t counts[4]; } TransformBytes;
typedef struct CandidateBytes { float value[4]; uint32_t keep, padding[3]; } CandidateBytes;
_Static_assert(sizeof(TransformBytes) == 80, "transform layout");
_Static_assert(sizeof(CandidateBytes) == 32, "candidate stride");

static rsx_frontend_buffer buffer(rsx_frontend_device *device, const void *bytes, uint64_t length) {
    rsx_frontend_buffer_desc desc = {sizeof(desc), 1, length, bytes}; rsx_frontend_buffer ref;
    assert(rsx_frontend_buffer_create(device, &desc, &ref) == 0); return ref;
}
static void release_buffer(rsx_frontend_device *device, rsx_frontend_buffer ref) {
    assert(rsx_frontend_buffer_release(device, ref) == 0);
}
static rsx_frontend_pipeline pipeline(rsx_frontend_device *device, const char *directory) {
    NSString *root = [NSString stringWithUTF8String:directory];
    NSData *source = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"shader.metal"]];
    NSData *metadata = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"manifest.json"]];
    assert(source && metadata);
    NSDictionary *manifest = [NSJSONSerialization JSONObjectWithData:metadata options:0 error:NULL];
    assert([manifest[@"schema_version"] unsignedIntValue] == 1);
    NSArray *buffers = manifest[@"buffers"];
    rsx_frontend_binding_layout layouts[16] = {0}; assert(buffers.count <= 16);
    for (NSUInteger i = 0; i < buffers.count; ++i) {
        NSDictionary *entry = buffers[i]; uint32_t size_index = RSX_FRONTEND_NO_SIZE_INDEX;
        for (NSDictionary *sizes in manifest[@"metal_sizes_buffer"][@"entries"])
            if ([sizes[@"binding"] isEqual:entry[@"binding"]]) size_index = [sizes[@"offset_bytes"] unsignedIntValue] / 4;
        layouts[i] = (rsx_frontend_binding_layout){[entry[@"binding"] unsignedIntValue],
            [entry[@"access"] isEqual:@"read"] ? RSX_FRONTEND_READ : RSX_FRONTEND_READ_WRITE,
            [entry[@"minimum_binding_bytes"] unsignedLongLongValue],
            size_index == RSX_FRONTEND_NO_SIZE_INDEX ? 0 : [entry[@"layout"][@"stride_bytes"] unsignedIntValue], size_index,
            [entry[@"layout"][@"alignment_bytes"] unsignedIntValue],
            [entry[@"address_space"] isEqual:@"uniform"] ? RSX_FRONTEND_UNIFORM : RSX_FRONTEND_STORAGE};
    }
    NSString *entry = manifest[@"targets"][@"msl"][@"entry_point"];
    rsx_frontend_pipeline_desc desc = {0}; desc.struct_size = sizeof(desc); desc.abi_version = 1;
    desc.source_bytes = source.bytes; desc.source_length = source.length;
    desc.entry_point = entry.UTF8String; desc.entry_point_length = (uint32_t)strlen(entry.UTF8String);
    desc.bindings = layouts; desc.binding_count = (uint32_t)buffers.count; desc.shader_language = RSX_FRONTEND_MSL;
    for (unsigned i = 0; i < 3; ++i) desc.threads_per_group[i] = [manifest[@"workgroup_size"][i] unsignedIntValue];
    rsx_frontend_pipeline ref;
    assert(rsx_frontend_pipeline_create(device, &desc, &ref) == 0);
    rsx_frontend_pipeline unused;
    uint32_t alignment = layouts[0].alignment_bytes; layouts[0].alignment_bytes = 3;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_INVALID);
    layouts[0].alignment_bytes = alignment;
    layouts[0].minimum_bytes += layouts[0].runtime_array_stride;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_UNSUPPORTED);
    layouts[0].minimum_bytes -= layouts[0].runtime_array_stride;
    desc.shader_language = RSX_FRONTEND_HLSL;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_UNSUPPORTED && !unused.id);
    desc.shader_language = RSX_FRONTEND_SPIRV;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_UNSUPPORTED);
    desc.abi_version = 2;
    assert(rsx_frontend_pipeline_create(device, &desc, &unused) == RSX_FRONTEND_INVALID);
    return ref;
}
static rsx_frontend_frame frame(rsx_frontend_pipeline pipeline, const rsx_frontend_binding *bindings, uint32_t count) {
    return (rsx_frontend_frame){sizeof(rsx_frontend_frame), 1, pipeline, bindings, count, {1, 1, 1}};
}
static void drain(rsx_frontend_device *device, const rsx_frontend_ticket *tickets, uint32_t expected) {
    uint32_t seen = 0; double deadline = CACurrentMediaTime() + 5;
    while (seen != (1u << expected) - 1) {
        rsx_frontend_event events[2]; uint32_t count;
        assert(rsx_frontend_poll(device, events, 2, &count) == 0);
        for (uint32_t i = 0; i < count; ++i) {
            assert(events[i].succeeded && events[i].no_further_gpu_access);
            assert(events[i].native_status == MTLCommandBufferStatusCompleted);
            uint32_t index;
            for (index = 0; index < expected; ++index)
                if (events[i].ticket.device_generation == tickets[index].device_generation &&
                    events[i].ticket.submission_id == tickets[index].submission_id) break;
            assert(index < expected && !(seen & (1u << index))); seen |= 1u << index;
        }
        assert(CACurrentMediaTime() < deadline); if (!count) usleep(1000);
    }
    uint32_t count; rsx_frontend_event events[2];
    assert(rsx_frontend_poll(device, events, 2, &count) == 0 && !count);
}
static void expect_values(const float *values, const float *expected, unsigned count) {
    for (unsigned i = 0; i < count; ++i) assert(fabsf(values[i] - expected[i]) < 0.00001f);
}
static void transform_test(rsx_frontend_device *device, const char *directory) {
    rsx_frontend_pipeline p = pipeline(device, directory);
    const float input[12] = {1,2,3,1, 4,5,6,1, 7,8,9,1};
    const float sentinel[8] = {-99,-99,-99,-99,-99,-99,-99,-99};
    const float expected[8] = {12,26,42,1, 18,35,54,1};
    TransformBytes parameters = {{2,0,0,0, 0,3,0,0, 0,0,4,0, 10,20,30,1}, {0,0,0,0}};
    rsx_frontend_buffer src = buffer(device, input, sizeof(input));
    rsx_frontend_buffer dst = buffer(device, sentinel, sizeof(sentinel));
    rsx_frontend_buffer uniform = buffer(device, &parameters, sizeof(parameters));
    rsx_frontend_binding bindings[3] = {{0,0,src,0,sizeof(input)}, {1,0,dst,0,sizeof(sentinel)}, {2,0,uniform,0,sizeof(parameters)}};
    rsx_frontend_frame f = frame(p, bindings, 3); rsx_frontend_ticket ticket;
    unsigned char misaligned_bytes[96] = {0};
    rsx_frontend_buffer misaligned = buffer(device, misaligned_bytes, sizeof(misaligned_bytes));
    bindings[2].buffer = misaligned; bindings[2].offset = 4;
    assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_INVALID && !ticket.submission_id);
    bindings[2].buffer = uniform; bindings[2].offset = 0; release_buffer(device, misaligned);
    /* Zero count performs genuine compute completion without modifying output. */
    assert(rsx_frontend_submit(device, &f, &ticket) == 0); drain(device, &ticket, 1);
    float values[8]; assert(rsx_frontend_buffer_read(device, dst, 0, values, sizeof(values)) == 0);
    expect_values(values, sentinel, 8);
    release_buffer(device, uniform); parameters.counts[0] = UINT32_MAX;
    uniform = buffer(device, &parameters, sizeof(parameters)); bindings[2].buffer = uniform;
    /* Every uncommitted original ownership path blocks explicit compute. */
    s_eng_rec_count = 1; assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY && !ticket.submission_id); s_eng_rec_count = 0;
    s_eng_stage_used = 4; assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY); s_eng_stage_used = 0;
    s_eng_tail_encode = ^(id<MTLCommandBuffer> cb){ (void)cb; };
    assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY); s_eng_tail_encode = nil;
    s_rec_count = 1; assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY); s_rec_count = 0;
    s_vert_count = 1; assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY); s_vert_count = 0;
    /* A real prior shared-queue wait keeps both tickets nonterminal. */
    id<MTLSharedEvent> gate = [s_dev newSharedEvent]; assert(gate);
    id<MTLCommandBuffer> previous = [s_queue commandBuffer];
    [previous encodeWaitForEvent:gate value:1]; [previous commit];
    rsx_frontend_ticket pending[2];
    assert(rsx_frontend_submit(device, &f, &pending[0]) == 0);
    assert(rsx_frontend_buffer_read(device, dst, 0, values, sizeof(values)) == RSX_FRONTEND_BUSY);
    rsx_frontend_buffer dst2 = buffer(device, sentinel, sizeof(sentinel)); bindings[1].buffer = dst2;
    assert(rsx_frontend_submit(device, &f, &pending[1]) == 0);
    assert(pending[1].submission_id > pending[0].submission_id);
    assert(rsx_frontend_submit(device, &f, &ticket) == RSX_FRONTEND_BUSY && !ticket.submission_id);
    rsx_frontend_event events[2]; uint32_t count;
    assert(rsx_frontend_poll(device, events, 2, &count) == 0 && !count);
    /* Publish a different full version and release the old public references
     * while its GPU work is still blocked; retained submissions use old bytes. */
    const float changed[12] = {9,9,9,9, 9,9,9,9, 9,9,9,9};
    rsx_frontend_buffer_desc update = {sizeof(update), 1, sizeof(changed), changed};
    rsx_frontend_buffer newer;
    assert(rsx_frontend_buffer_update(device, src, &update, &newer) == 0);
    assert(newer.id == src.id && newer.version > src.version);
    release_buffer(device, src);
    assert(rsx_frontend_buffer_release(device, src) == RSX_FRONTEND_STALE);
    assert(rsx_frontend_pipeline_release(device, p) == 0);
    assert(rsx_frontend_pipeline_release(device, p) == RSX_FRONTEND_STALE);
    gate.signaledValue = 1; drain(device, pending, 2);
    assert(rsx_frontend_buffer_read(device, dst, 0, values, sizeof(values)) == 0); expect_values(values, expected, 8);
    assert(rsx_frontend_buffer_read(device, dst2, 0, values, sizeof(values)) == 0); expect_values(values, expected, 8);
    assert(rsx_frontend_buffer_read(device, newer, UINT64_MAX, values, 4) == RSX_FRONTEND_INVALID);
    rsx_frontend_buffer stale = newer; ++stale.device_generation;
    assert(rsx_frontend_buffer_read(device, stale, 0, values, 4) == RSX_FRONTEND_STALE);
    release_buffer(device, newer); release_buffer(device, dst); release_buffer(device, dst2); release_buffer(device, uniform);
    puts("PASS real Metal transform: zero/oversized counts, original-order gate, two-ticket pressure, delayed completion, immutable version retention");
}
static void compact_test(rsx_frontend_device *device, const char *directory) {
    rsx_frontend_pipeline p = pipeline(device, directory);
    const CandidateBytes candidates[4] = {{{1,2,3,1},1,{0}}, {{4,5,6,1},0,{0}}, {{7,8,9,1},7,{0}}, {{10,11,12,1},1,{0}}};
    const float sentinel[8] = {-99,-99,-99,-99,-99,-99,-99,-99};
    const float selected[8] = {1,2,3,1, 7,8,9,1};
    for (unsigned test = 0; test < 3; ++test) {
        uint32_t parameters[4] = {test == 0 ? 0 : test == 1 ? 4 : UINT32_MAX,0,0,0};
        uint32_t counters[4] = {99,99,99,99};
        rsx_frontend_buffer refs[4] = {buffer(device, candidates, sizeof(candidates)), buffer(device, sentinel, sizeof(sentinel)),
            buffer(device, counters, sizeof(counters)), buffer(device, parameters, sizeof(parameters))};
        rsx_frontend_binding bindings[4] = {{0,0,refs[0],0,sizeof(candidates)}, {1,0,refs[1],0,sizeof(sentinel)},
            {2,0,refs[2],0,sizeof(counters)}, {3,0,refs[3],0,sizeof(parameters)}};
        rsx_frontend_frame f = frame(p, bindings, 4); rsx_frontend_ticket ticket;
        assert(rsx_frontend_submit(device, &f, &ticket) == 0); drain(device, &ticket, 1);
        float output[8]; assert(rsx_frontend_buffer_read(device, refs[1], 0, output, sizeof(output)) == 0);
        expect_values(output, test ? selected : sentinel, 8);
        assert(rsx_frontend_buffer_read(device, refs[2], 0, counters, sizeof(counters)) == 0);
        assert(counters[0] == (test ? 2 : 0) && counters[1] == parameters[0]);
        assert(counters[2] == (test ? 4 : 0) && counters[3] == (test ? 1 : 0));
        for (unsigned i = 0; i < 4; ++i) release_buffer(device, refs[i]);
    }
    assert(rsx_frontend_pipeline_release(device, p) == 0);
    puts("PASS real Metal stable compaction: selected order, output truncation, zero and oversized batches with exact counters");
}
static void shutdown_test(rsx_frontend_device *device, const char *directory) {
    rsx_frontend_pipeline p = pipeline(device, directory);
    const float input[4] = {1,2,3,1};
    TransformBytes parameters = {{2,0,0,0, 0,3,0,0, 0,0,4,0, 10,20,30,1}, {1,0,0,0}};
    rsx_frontend_buffer refs[3] = {buffer(device, input, 16), buffer(device, NULL, 16), buffer(device, &parameters, 80)};
    rsx_frontend_binding bindings[3] = {{0,0,refs[0],0,16}, {1,0,refs[1],0,16}, {2,0,refs[2],0,80}};
    rsx_frontend_frame f = frame(p, bindings, 3); rsx_frontend_ticket ticket;
    FrontendMetal *inner = context(device->context);
    id<MTLBuffer> observed_output = find_buffer(inner, refs[1])->gpu;
    id<MTLSharedEvent> gate = [s_dev newSharedEvent];
    id<MTLCommandBuffer> prior = [s_queue commandBuffer]; [prior encodeWaitForEvent:gate value:1]; [prior commit];
    assert(rsx_frontend_submit(device, &f, &ticket) == 0);
    for (unsigned i = 0; i < 3; ++i) release_buffer(device, refs[i]);
    assert(inner->buffers.count == 0 && inner->versions.count == 0);
    assert(rsx_frontend_pipeline_release(device, p) == 0);
    dispatch_semaphore_t signaled = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        usleep(30000); gate.signaledValue = 1; dispatch_semaphore_signal(signaled);
    });
    double start = CACurrentMediaTime();
    rsx_frontend_close(s_frontend); s_frontend = NULL;
    assert(CACurrentMediaTime() - start >= 0.02);
    dispatch_semaphore_wait(signaled, dispatch_time(DISPATCH_TIME_NOW, 1000000000));
    const float expected[4] = {12,26,42,1}; expect_values(observed_output.contents, expected, 4);
    assert(prior.status == MTLCommandBufferStatusCompleted);
    /* Closing this borrowed frontend leaves the existing device/queue alive;
     * a new context rejects every old handle despite per-context ID reuse. */
    rsx_frontend_device *fresh = rsx_metal_backend_frontend(); assert(fresh);
    rsx_frontend_caps_desc caps = {sizeof(caps), 1}; assert(rsx_frontend_caps(fresh, &caps) == 0);
    assert(caps.device_generation != ticket.device_generation);
    float unused[4]; assert(rsx_frontend_buffer_read(fresh, refs[0], 0, unused, 16) == RSX_FRONTEND_STALE);
    rsx_frontend_close(s_frontend); s_frontend = NULL;
    puts("PASS shutdown drains real pending GPU use; released versions retire metadata; stale context rejected; shared owner remains alive");
}
int main(int argc, char **argv) {
    assert(argc == 3 && "provide generated transform and compaction directories");
    @autoreleasepool {
        assert(!rsx_metal_backend_frontend());
        /* Establish the same owner fields as the isolated existing-backend tests.
         * The frontend must borrow these exact objects, never create a device. */
        s_dev = MTLCreateSystemDefaultDevice(); assert(s_dev);
        s_queue = [s_dev newCommandQueue]; assert(s_queue);
        s_ready = 1;
        assert(!s_layer && !s_window && !NSApp);
        rsx_frontend_device *frontend = rsx_metal_backend_frontend();
        assert(frontend && "existing Metal owner needs an explicit frontend adapter");
        rsx_frontend_caps_desc caps = {sizeof(caps), RSX_FRONTEND_ABI_VERSION};
        assert(rsx_frontend_caps(frontend, &caps) == RSX_FRONTEND_OK);
        assert(caps.compute_supported && !caps.draws_supported && caps.max_pending == 2);
        printf("Metal device: %s, generation %llu\n", s_dev.name.UTF8String, (unsigned long long)caps.device_generation);
        transform_test(frontend, argv[1]); compact_test(frontend, argv[2]);
        FrontendMetal *inner = context(frontend->context);
        assert(inner->versions.count == 0 && "all unaddressable version metadata must be retired");
        assert(!s_layer && !s_window && !NSApp);
        shutdown_test(frontend, argv[1]);
        s_ready = 0; s_queue = nil; s_dev = nil;
        puts("PASS explicit frontend borrows existing Metal owner");
    }
}
