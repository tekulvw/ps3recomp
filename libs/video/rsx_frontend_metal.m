#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "rsx_frontend_internal.h"
#include <stdio.h>
#include <string.h>

@interface FrontendBuffer : NSObject {
@public id<MTLBuffer> gpu; uint64_t identifier, version; uint32_t readers, writers;
}
@end
@implementation FrontendBuffer @end
@interface FrontendPipeline : NSObject {
@public id<MTLComputePipelineState> gpu; MTLSize threads;
    rsx_frontend_binding_layout bindings[RSX_FRONTEND_MAX_BINDINGS];
    uint32_t binding_count, sizes_count;
}
@end
@implementation FrontendPipeline @end
@interface FrontendUse : NSObject {
@public FrontendBuffer *buffer; int write;
}
@end
@implementation FrontendUse @end
@interface FrontendSubmission : NSObject {
@public id<MTLCommandBuffer> command; FrontendPipeline *pipeline;
    id<MTLBuffer> sizes; NSArray<FrontendUse *> *uses; rsx_frontend_ticket ticket;
}
@end
@implementation FrontendSubmission @end
@interface FrontendMetal : NSObject {
@public id<MTLDevice> device; id<MTLCommandQueue> queue;
    NSMutableDictionary<NSString *, FrontendBuffer *> *buffers;
    NSMutableDictionary<NSNumber *, NSNumber *> *versions;
    NSMutableDictionary<NSNumber *, FrontendPipeline *> *pipelines;
    NSMutableArray<FrontendSubmission *> *pending;
    uint64_t generation, next_buffer, next_pipeline, next_submission;
    int (*ordering_ready)(void);
}
@end
@implementation FrontendMetal @end

static FrontendMetal *context(void *opaque) { return (__bridge FrontendMetal *)opaque; }
static NSString *buffer_key(uint64_t id, uint64_t version) {
    return [NSString stringWithFormat:@"%llu:%llu", (unsigned long long)id, (unsigned long long)version];
}
static FrontendBuffer *find_buffer(FrontendMetal *c, rsx_frontend_buffer ref) {
    return ref.device_generation == c->generation && ref.id && ref.version ?
        c->buffers[buffer_key(ref.id, ref.version)] : nil;
}
static FrontendPipeline *find_pipeline(FrontendMetal *c, rsx_frontend_pipeline ref) {
    return ref.device_generation == c->generation && ref.id ? c->pipelines[@(ref.id)] : nil;
}
static int metal_caps(void *opaque, rsx_frontend_caps_desc *out) {
    FrontendMetal *c = context(opaque);
    *out = (rsx_frontend_caps_desc){sizeof(*out), RSX_FRONTEND_ABI_VERSION, c->generation,
        1, 0, 2, RSX_FRONTEND_MAX_BINDINGS, 30, 0};
    return RSX_FRONTEND_OK;
}
static int metal_buffer_create(void *opaque, rsx_frontend_buffer previous,
    const rsx_frontend_buffer_desc *desc, rsx_frontend_buffer *out) {
    FrontendMetal *c = context(opaque);
    if (!desc->byte_length || desc->byte_length % 4) return RSX_FRONTEND_INVALID;
    if (desc->byte_length > UINT32_MAX || desc->byte_length > c->device.maxBufferLength)
        return RSX_FRONTEND_UNSUPPORTED;
    uint64_t buffer_id, version;
    if (previous.id) {
        if (!find_buffer(c, previous)) return RSX_FRONTEND_STALE;
        buffer_id = previous.id; version = [c->versions[@(buffer_id)] unsignedLongLongValue];
        if (version == UINT64_MAX) return RSX_FRONTEND_STALE;
        ++version;
    } else {
        if (c->next_buffer == UINT64_MAX) return RSX_FRONTEND_ALLOCATION_FAILED;
        buffer_id = ++c->next_buffer; version = 1;
    }
    id<MTLBuffer> gpu = [c->device newBufferWithLength:(NSUInteger)desc->byte_length options:MTLResourceStorageModeShared];
    if (!gpu || !gpu.contents) return RSX_FRONTEND_ALLOCATION_FAILED;
    if (desc->initial_bytes) memcpy(gpu.contents, desc->initial_bytes, (size_t)desc->byte_length);
    else memset(gpu.contents, 0, (size_t)desc->byte_length);
    FrontendBuffer *record = [FrontendBuffer new]; record->gpu = gpu;
    record->identifier = buffer_id; record->version = version;
    c->buffers[buffer_key(buffer_id, version)] = record; c->versions[@(buffer_id)] = @(version);
    *out = (rsx_frontend_buffer){c->generation, buffer_id, version};
    return RSX_FRONTEND_OK;
}
static int metal_buffer_release(void *opaque, rsx_frontend_buffer ref) {
    FrontendMetal *c = context(opaque);
    if (!find_buffer(c, ref)) return RSX_FRONTEND_STALE;
    [c->buffers removeObjectForKey:buffer_key(ref.id, ref.version)];
    int addressable = 0;
    for (FrontendBuffer *other in c->buffers.objectEnumerator)
        if (other->identifier == ref.id) { addressable = 1; break; }
    if (!addressable) [c->versions removeObjectForKey:@(ref.id)];
    return RSX_FRONTEND_OK;
}
static int metal_buffer_read(void *opaque, rsx_frontend_buffer ref, uint64_t offset, void *out, uint64_t length) {
    FrontendBuffer *record = find_buffer(context(opaque), ref);
    if (!record) return RSX_FRONTEND_STALE;
    if (offset > record->gpu.length || length > record->gpu.length - offset) return RSX_FRONTEND_INVALID;
    if (record->readers || record->writers) return RSX_FRONTEND_BUSY;
    memcpy(out, (const char *)record->gpu.contents + offset, (size_t)length);
    return RSX_FRONTEND_OK;
}
static int metal_pipeline_create(void *opaque, const rsx_frontend_pipeline_desc *desc, rsx_frontend_pipeline *out) {
    FrontendMetal *c = context(opaque);
    if (desc->shader_language != RSX_FRONTEND_MSL) return RSX_FRONTEND_UNSUPPORTED;
    if (!desc->source_bytes || !desc->source_length || desc->source_length > 1024 * 1024 ||
        !desc->entry_point || !desc->entry_point_length || desc->entry_point_length > 128 ||
        !desc->bindings || !desc->binding_count || desc->binding_count > RSX_FRONTEND_MAX_BINDINGS)
        return RSX_FRONTEND_INVALID;
    uint32_t slots = 0, indices = 0, sizes_count = 0, threads = 1;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t n = desc->threads_per_group[i];
        if (!n || n > 256 || threads > 256 / n) return RSX_FRONTEND_UNSUPPORTED;
        threads *= n;
    }
    for (uint32_t i = 0; i < desc->binding_count; ++i) {
        rsx_frontend_binding_layout b = desc->bindings[i];
        if (b.slot >= RSX_FRONTEND_MAX_BINDINGS || slots & (1u << b.slot) || !b.minimum_bytes ||
            b.minimum_bytes % 4 || b.minimum_bytes > UINT32_MAX || b.binding_kind > RSX_FRONTEND_UNIFORM ||
            (b.binding_kind == RSX_FRONTEND_UNIFORM && (b.access != RSX_FRONTEND_READ || b.runtime_array_stride)) ||
            b.alignment_bytes < 4 || (b.alignment_bytes & (b.alignment_bytes - 1)) || b.alignment_bytes > 256 ||
            (b.access != RSX_FRONTEND_READ && b.access != RSX_FRONTEND_READ_WRITE)) return RSX_FRONTEND_INVALID;
        slots |= 1u << b.slot;
        if (b.runtime_array_stride) {
            /* The translator can describe struct headers before arrays, but
             * this first native ABI route only supports byte-zero arrays. */
            if (b.minimum_bytes != b.runtime_array_stride) return RSX_FRONTEND_UNSUPPORTED;
            if (b.runtime_array_stride % 4 || b.minimum_bytes < b.runtime_array_stride ||
                b.runtime_size_index >= RSX_FRONTEND_MAX_BINDINGS || indices & (1u << b.runtime_size_index))
                return RSX_FRONTEND_INVALID;
            indices |= 1u << b.runtime_size_index; ++sizes_count;
        } else if (b.runtime_size_index != RSX_FRONTEND_NO_SIZE_INDEX) return RSX_FRONTEND_INVALID;
    }
    if (indices != (1u << sizes_count) - 1) return RSX_FRONTEND_INVALID;
    NSString *source = [[NSString alloc] initWithBytes:desc->source_bytes length:(NSUInteger)desc->source_length encoding:NSUTF8StringEncoding];
    NSString *entry = [[NSString alloc] initWithBytes:desc->entry_point length:desc->entry_point_length encoding:NSUTF8StringEncoding];
    if (!source || !entry) return RSX_FRONTEND_INVALID;
    MTLCompileOptions *options = [MTLCompileOptions new];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    } else
#endif
    { [options setValue:@NO forKey:@"fastMathEnabled"]; }
    options.languageVersion = MTLLanguageVersion2_0;
    NSError *error = nil;
    id<MTLLibrary> library = [c->device newLibraryWithSource:source options:options error:&error];
    id<MTLFunction> function = [library newFunctionWithName:entry];
    id<MTLComputePipelineState> gpu = function ? [c->device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!gpu) {
        fprintf(stderr, "[rsx frontend/metal] pipeline compilation failed: %s\n", error.localizedDescription.UTF8String ?: "entry point unavailable");
        return RSX_FRONTEND_UNSUPPORTED;
    }
    MTLSize limits = c->device.maxThreadsPerThreadgroup;
    if (threads > gpu.maxTotalThreadsPerThreadgroup || desc->threads_per_group[0] > limits.width ||
        desc->threads_per_group[1] > limits.height || desc->threads_per_group[2] > limits.depth)
        return RSX_FRONTEND_UNSUPPORTED;
    if (c->next_pipeline == UINT64_MAX) return RSX_FRONTEND_ALLOCATION_FAILED;
    FrontendPipeline *record = [FrontendPipeline new]; record->gpu = gpu;
    record->threads = MTLSizeMake(desc->threads_per_group[0], desc->threads_per_group[1], desc->threads_per_group[2]);
    record->binding_count = desc->binding_count; record->sizes_count = sizes_count;
    memcpy(record->bindings, desc->bindings, sizeof(*desc->bindings) * desc->binding_count);
    uint64_t id = ++c->next_pipeline; c->pipelines[@(id)] = record;
    *out = (rsx_frontend_pipeline){c->generation, id};
    return RSX_FRONTEND_OK;
}
static int metal_pipeline_release(void *opaque, rsx_frontend_pipeline ref) {
    FrontendMetal *c = context(opaque);
    if (!find_pipeline(c, ref)) return RSX_FRONTEND_STALE;
    [c->pipelines removeObjectForKey:@(ref.id)]; return RSX_FRONTEND_OK;
}
static void release_uses(FrontendSubmission *submission) {
    for (FrontendUse *use in submission->uses) {
        if (use->write) --use->buffer->writers; else --use->buffer->readers;
    }
}
static int metal_submit(void *opaque, const rsx_frontend_frame *frame, rsx_frontend_ticket *out) {
    FrontendMetal *c = context(opaque);
    if (c->pending.count >= 2 || !c->ordering_ready()) return RSX_FRONTEND_BUSY;
    FrontendPipeline *pipeline = find_pipeline(c, frame->pipeline);
    if (!pipeline) return RSX_FRONTEND_STALE;
    if (!frame->bindings || frame->binding_count != pipeline->binding_count) return RSX_FRONTEND_INVALID;
    for (unsigned i = 0; i < 3; ++i) if (!frame->groups[i] || frame->groups[i] > 65535) return RSX_FRONTEND_INVALID;
    if (c->next_submission == UINT64_MAX) return RSX_FRONTEND_DEVICE_FAILED;
    NSMutableArray<FrontendUse *> *uses = [NSMutableArray new];
    uint32_t slots = 0, lengths[RSX_FRONTEND_MAX_BINDINGS] = {0};
    for (uint32_t i = 0; i < frame->binding_count; ++i) {
        rsx_frontend_binding b = frame->bindings[i];
        if (b.slot >= RSX_FRONTEND_MAX_BINDINGS || b.reserved || slots & (1u << b.slot)) return RSX_FRONTEND_INVALID;
        slots |= 1u << b.slot;
        const rsx_frontend_binding_layout *schema = NULL;
        for (uint32_t j = 0; j < pipeline->binding_count; ++j) if (pipeline->bindings[j].slot == b.slot) schema = &pipeline->bindings[j];
        FrontendBuffer *buffer = find_buffer(c, b.buffer);
        if (!buffer) return RSX_FRONTEND_STALE;
        if (!schema || b.offset % schema->alignment_bytes || b.byte_length < schema->minimum_bytes || b.byte_length > UINT32_MAX ||
            b.offset > buffer->gpu.length || b.byte_length > buffer->gpu.length - b.offset ||
            (schema->runtime_array_stride && b.byte_length % schema->runtime_array_stride)) return RSX_FRONTEND_INVALID;
        int write = schema->access == RSX_FRONTEND_READ_WRITE;
        if (buffer->writers || (write && buffer->readers)) return RSX_FRONTEND_BUSY;
        for (FrontendUse *prior in uses) if (prior->buffer == buffer && (write || prior->write)) return RSX_FRONTEND_INVALID;
        FrontendUse *use = [FrontendUse new]; use->buffer = buffer; use->write = write; [uses addObject:use];
        if (schema->runtime_array_stride) lengths[schema->runtime_size_index] = (uint32_t)b.byte_length;
    }
    FrontendSubmission *submission = [FrontendSubmission new]; submission->pipeline = pipeline; submission->uses = uses;
    submission->command = [c->queue commandBuffer];
    if (!submission->command) return RSX_FRONTEND_DEVICE_FAILED;
    id<MTLComputeCommandEncoder> encoder = [submission->command computeCommandEncoder];
    if (!encoder) return RSX_FRONTEND_DEVICE_FAILED;
    [encoder setComputePipelineState:pipeline->gpu];
    for (uint32_t i = 0; i < frame->binding_count; ++i) {
        rsx_frontend_binding b = frame->bindings[i];
        [encoder setBuffer:((FrontendUse *)uses[i])->buffer->gpu offset:(NSUInteger)b.offset atIndex:b.slot];
    }
    if (pipeline->sizes_count) {
        submission->sizes = [c->device newBufferWithBytes:lengths length:pipeline->sizes_count * sizeof(uint32_t) options:MTLResourceStorageModeShared];
        if (!submission->sizes) { [encoder endEncoding]; return RSX_FRONTEND_ALLOCATION_FAILED; }
        [encoder setBuffer:submission->sizes offset:0 atIndex:30];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(frame->groups[0], frame->groups[1], frame->groups[2]) threadsPerThreadgroup:pipeline->threads];
    [encoder endEncoding];
    submission->ticket = (rsx_frontend_ticket){c->generation, ++c->next_submission};
    for (FrontendUse *use in uses) { if (use->write) ++use->buffer->writers; else ++use->buffer->readers; }
    [c->pending addObject:submission]; *out = submission->ticket;
    @try { [submission->command commit]; }
    @catch (NSException *error) {
        (void)error;
        if (submission->command.status >= MTLCommandBufferStatusCommitted) return RSX_FRONTEND_INDETERMINATE;
        release_uses(submission); [c->pending removeObjectIdenticalTo:submission]; memset(out, 0, sizeof(*out));
        return RSX_FRONTEND_DEVICE_FAILED;
    }
    return RSX_FRONTEND_OK;
}
static int metal_poll(void *opaque, rsx_frontend_event *events, uint32_t capacity, uint32_t *count) {
    FrontendMetal *c = context(opaque);
    for (NSUInteger i = 0; i < c->pending.count && *count < capacity;) {
        FrontendSubmission *submission = c->pending[i];
        MTLCommandBufferStatus status = submission->command.status;
        if (status != MTLCommandBufferStatusCompleted && status != MTLCommandBufferStatusError) { ++i; continue; }
        events[(*count)++] = (rsx_frontend_event){sizeof(*events), RSX_FRONTEND_ABI_VERSION, submission->ticket,
            status == MTLCommandBufferStatusCompleted && !submission->command.error, 1, (uint32_t)status, 0};
        release_uses(submission); [c->pending removeObjectAtIndex:i];
    }
    return RSX_FRONTEND_OK;
}
static void metal_close(void *opaque) {
    FrontendMetal *c = context(opaque);
    for (FrontendSubmission *submission in c->pending) [submission->command waitUntilCompleted];
    for (FrontendSubmission *submission in c->pending) release_uses(submission);
    [c->pending removeAllObjects];
    (void)CFBridgingRelease(opaque);
}
static const rsx_frontend_ops metal_ops = {metal_caps, metal_buffer_create, metal_buffer_release,
    metal_buffer_read, metal_pipeline_create, metal_pipeline_release, metal_submit, metal_poll, metal_close};
rsx_frontend_device *rsx_frontend_metal_create(id<MTLDevice> device, id<MTLCommandQueue> queue, int (*ordering_ready)(void)) {
    if (!device || !queue || queue.device != device || !ordering_ready) return NULL;
    FrontendMetal *c = [FrontendMetal new]; c->device = device; c->queue = queue; c->ordering_ready = ordering_ready;
    c->generation = rsx_frontend_next_generation(); if (!c->generation) return NULL;
    c->buffers = [NSMutableDictionary new]; c->versions = [NSMutableDictionary new];
    c->pipelines = [NSMutableDictionary new]; c->pending = [NSMutableArray new];
    void *opaque = (__bridge_retained void *)c;
    rsx_frontend_device *wrapper = rsx_frontend_wrap(&metal_ops, opaque);
    if (!wrapper) (void)CFBridgingRelease(opaque);
    return wrapper;
}
