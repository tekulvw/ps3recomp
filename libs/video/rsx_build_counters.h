#ifndef PS3RECOMP_RSX_BUILD_COUNTERS_H
#define PS3RECOMP_RSX_BUILD_COUNTERS_H
/* The counting side of include/ps3emu/rsx_build_stats.h (docs/RSX_BUILD_STATS.md).
 *
 * The counters live in rsx_fifo_busy.c: every program that links the draw engine or a host
 * backend also links that file, so no build or test needs another source. Each call is a few
 * relaxed atomic adds; callers time an event with rsx_build_now_ns() before and after it. */
#include <stdint.h>
/* By path (RSX_BUILD_FLAG_*, RSX_BUILD_SLOW_UPLOAD_NS): some test builds have no -I include. */
#include "../../include/ps3emu/rsx_build_stats.h"
#ifdef __cplusplus
extern "C" {
#endif

enum rsx_build_event {
    RSX_BUILD_PROGRAM = 0,       /* a draw-engine pipeline built (decompile + backend create) */
    RSX_BUILD_TRANSLATION,       /* one shader translated (glslang, or MSL + Metal library) */
    RSX_BUILD_PIPELINE,          /* one host pipeline object created */
    RSX_BUILD_TEXTURE_UPLOAD,    /* one texture decoded and uploaded; bytes = guest bytes */
    RSX_BUILD_EVENT_COUNT
};

/* The monotonic clock the counters use, in nanoseconds. */
uint64_t rsx_build_now_ns(void);
/* One event that took `ns` (and moved `bytes`, texture uploads only). */
void rsx_build_note(int event, uint64_t ns, uint64_t bytes);
/* The fields are exclusive, so they add up: a program's own time leaves out the translations and
 * pipeline creations inside it (Metal creates its pipeline state there; Vulkan creates pipelines
 * later, at the first draw of each topology). This is the calling thread's translation and
 * pipeline time so far; a program notes its wall time minus what this grew by during it. */
uint64_t rsx_build_inner_ns(void);
/* What a RSX_BUILD_TEXTURE_UPLOAD was, noted next to it: a texture the cache did not hold
 * (changed == 0) or one whose guest bytes changed (changed != 0, in_place != 0 when it went
 * into the texture the cache already had). */
void rsx_build_note_texture_upload_kind(int changed, int in_place, uint64_t bytes);
/* The texture cache evicted an entry to make room. */
void rsx_build_note_texture_eviction(void);
/* The calling thread waited `ns` for the GPU to free a recording slot. */
void rsx_build_note_gpu_wait(uint64_t ns);
/* This thread's GPU waits so far; a texture upload notes what this grew by during it. */
uint64_t rsx_build_gpu_wait_ns(void);
void rsx_build_note_texture_upload_wait(uint64_t ns);
/* A shader the translator served from its cache: disk != 0 for the on-disk cache. */
void rsx_build_note_shader_hit(int disk);
/* The VkPipelineCache written to its file once, which took `ns`. */
void rsx_build_note_pipeline_cache_save(uint64_t ns);
/* Process facts: a RSX_BUILD_FLAG_* turned on, the backend's name (at most 7 characters, the
 * first call wins) and the VkPipelineCache bytes loaded at device creation. */
void rsx_build_set_flag(uint32_t flag);
void rsx_build_set_backend(const char* name);
void rsx_build_set_pipeline_cache_loaded(uint64_t bytes);
/* Tests only: every counter back to zero. */
void rsx_build_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif
