/*
 * ps3recomp - RSX first-use build counters (docs/RSX_BUILD_STATS.md)
 *
 * Always on: the draw engine and the host backends count, with relaxed atomics, the one-off work
 * a draw can cause the first time it is seen -- building a pipeline for a new program and render
 * state, translating its shaders, creating the host pipeline object -- and the texture uploads a
 * draw's bindings cause. Each event reads the monotonic clock twice; nothing is counted per draw
 * or per FIFO method. An embedding application polls the counters, for example to say what a
 * slow frame spent its time on. They are cumulative for the whole process.
 *
 * What each field counts:
 *   programs           draw-engine pipelines built: the guest programs decompiled to HLSL for a
 *                      new program/state key and the backend's pipeline_create. program_ns leaves
 *                      out the translations and pipeline creations inside the build, which count
 *                      in their own fields: the *_ns fields are exclusive and add up.
 *   shader_translations  shaders actually translated: HLSL -> SPIR-V through glslang (Vulkan) or
 *                      HLSL -> MSL plus the Metal library compile (Metal), failed attempts
 *                      included. A shader served from the translator's own caches is not one.
 *   shader_memory_hits, shader_disk_hits  the SPIR-V translator's in-memory and on-disk
 *                      (PS3RECOMP_SPIRV_CACHE) cache hits. Metal counts a reused function as a
 *                      memory hit; it has no disk cache.
 *   pipelines          host pipeline objects created: vkCreateGraphicsPipelines (every call site)
 *                      or newRenderPipelineStateWithDescriptor for a guest program pipeline.
 *                      pipeline_worst_ns is the slowest single creation.
 *   texture_uploads    textures decoded and uploaded by the draw engine (new or changed contents);
 *                      texture_upload_bytes their guest bytes; slow_texture_uploads those that
 *                      took RSX_BUILD_SLOW_UPLOAD_NS or longer each.
 *   flags              RSX_BUILD_FLAG_*: which disk caches this process uses.
 *   pipeline_cache_loaded_bytes  bytes of VkPipelineCache data loaded at device creation
 *                      (PS3RECOMP_VK_PIPELINE_CACHE); 0 for an empty, refused or absent cache.
 *   pipeline_cache_saves, pipeline_cache_save_ns  times that cache was written to its file (every
 *                      32 new pipelines and at shutdown, on the thread that created the 32nd) and
 *                      the time it took, data read-back and file write included.
 *   backend            "vulkan" or "metal" once that backend's device is up, else "".
 * Version 2 appends, after backend, what the texture uploads were (docs/RSX_BUILD_STATS.md):
 *   texture_upload_new, texture_upload_new_bytes  uploads of a texture the cache did not hold: its
 *                      first use, or a use after it was evicted (texture_evictions counts those).
 *   texture_upload_changed, texture_upload_changed_bytes  uploads because a cached texture's guest
 *                      bytes changed; texture_upload_in_place of them went into the texture the
 *                      cache already had instead of a new one (the backend's texture_reusable).
 *   texture_upload_size_n/_bytes[4]  every upload by its guest size: up to 64 KiB, up to 1 MiB, up
 *                      to 4 MiB, larger.
 *   texture_upload_wait_ns  of texture_upload_ns, the time those uploads waited for the GPU to
 *                      free a recording slot (Vulkan batched recording; 0 elsewhere).
 *   gpu_slot_waits, gpu_slot_wait_ns  every such wait, on any work, uploads included.
 * Fields of a newer version are only ever appended; a caller built against version 1 reads the
 * same first 144 bytes.
 * The D3D12 backend is not instrumented: under it only the draw engine's programs and texture
 * uploads count, and backend stays "".
 */
#ifndef PS3EMU_RSX_BUILD_STATS_H
#define PS3EMU_RSX_BUILD_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3RECOMP_RSX_BUILD_STATS_VERSION 2
#define RSX_BUILD_SLOW_UPLOAD_NS 4000000ull         /* 4 ms */
#define RSX_BUILD_FLAG_SPIRV_DISK_CACHE    0x1u     /* PS3RECOMP_SPIRV_CACHE is set */
#define RSX_BUILD_FLAG_VK_PIPELINE_CACHE   0x2u     /* a VkPipelineCache is in use (PS3RECOMP_VK_PIPELINE_CACHE) */

/* Like struct stat and stat(), the struct tag and the getter share one name, so the type is
 * always written `struct ps3recomp_rsx_build_stats` (C and C++). */
struct ps3recomp_rsx_build_stats {
    uint32_t version;                 /* PS3RECOMP_RSX_BUILD_STATS_VERSION */
    uint32_t flags;                   /* RSX_BUILD_FLAG_* */
    uint64_t programs, program_ns;
    uint64_t shader_translations, shader_translation_ns;
    uint64_t shader_memory_hits, shader_disk_hits;
    uint64_t pipelines, pipeline_ns, pipeline_worst_ns;
    uint64_t texture_uploads, texture_upload_bytes, texture_upload_ns, slow_texture_uploads;
    uint64_t pipeline_cache_loaded_bytes;
    uint64_t pipeline_cache_saves, pipeline_cache_save_ns;
    char backend[8];
    /* version 2 */
    uint64_t texture_upload_new, texture_upload_new_bytes;
    uint64_t texture_upload_changed, texture_upload_changed_bytes, texture_upload_in_place;
    uint64_t texture_evictions;
    uint64_t texture_upload_size_n[4], texture_upload_size_bytes[4];
    uint64_t texture_upload_wait_ns;
    uint64_t gpu_slot_waits, gpu_slot_wait_ns;
};

/* Fills *out (always returns 0). Thread-safe, lock-free and cheap: a few relaxed loads. */
int ps3recomp_rsx_build_stats(struct ps3recomp_rsx_build_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_RSX_BUILD_STATS_H */
