/*
 * ps3recomp - HLSL -> SPIR-V for the Vulkan backend
 *
 * The RSX decompilers emit HLSL. The Metal backend lowers it HLSL -> SPIR-V ->
 * MSL (rsx_shader_msl.cpp); Vulkan consumes SPIR-V directly, so this stops
 * one step earlier and needs glslang only, not spirv-cross. Without glslang
 * the module compiles to a stub that reports itself unavailable, and the
 * Vulkan backend stays on its fixed-function path.
 *
 * Binding contract, which the Vulkan backend relies on (all in set 0). Vulkan
 * has one binding namespace per set where MSL has three, so the register
 * classes are moved apart:
 *   register(bN)  ->  binding N                          (VPConst 0, PSConstants 1)
 *   register(tN)  ->  binding RSX_SPIRV_TEXTURE_BINDING + N
 *   register(sN)  ->  binding RSX_SPIRV_SAMPLER_BINDING + N
 * The fragment decompiler's `Texture2D rsx_tex[16] : register(t0)` is one
 * binding holding an array of 16, and `SamplerState rsx_samp[16]` likewise.
 * Vertex-texture units (t16..t19) land on TEXTURE_BINDING + 16..19.
 *
 * Locations are numbered in declaration order (TProgram::mapIO). The vertex
 * decompiler always declares ATTR0..ATTR15 in order, so vertex input
 * location n is RSX attribute n; both decompilers declare the varyings in the
 * same order, so the stages link.
 *
 * The vertex stage's position is flipped in Y: the decompiled HLSL writes
 * D3D clip space (+y up), and Vulkan's framebuffer has +y down. Depth needs
 * nothing -- the viewport epilogue already maps GL clip z to [0, 1].
 */

#ifndef PS3RECOMP_RSX_SHADER_SPIRV_H
#define PS3RECOMP_RSX_SHADER_SPIRV_H

#include "ps3emu/ps3types.h"
#include "rsx_shader_msl.h"   /* RSX_SHADER_STAGE_VERTEX / _FRAGMENT */

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_SPIRV_VPCONST_BINDING  0u
#define RSX_SPIRV_PSCONST_BINDING  1u
#define RSX_SPIRV_TEXTURE_BINDING  16u
#define RSX_SPIRV_SAMPLER_BINDING  48u

/* 1 when built with glslang, 0 when the stub is in. */
int rsx_hlsl_to_spirv_available(void);

/* Translate one HLSL shader whose entry point is `main` into SPIR-V 1.0 for a
 * Vulkan 1.0 client.
 *   hlsl      : NUL-terminated HLSL, as the decompilers emit it.
 *   stage     : RSX_SHADER_STAGE_VERTEX or RSX_SHADER_STAGE_FRAGMENT.
 *   words     : receives the SPIR-V, max_words 32-bit words at most.
 *   out_words : receives the word count.
 *   log       : receives a diagnostic on failure (may be NULL).
 * Returns 0 on success, -1 on failure (bad input, a front-end error, an
 * output buffer too small, or the stub). */
int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                      u32* words, u32 max_words, u32* out_words,
                      char* log, u32 log_size);

/* The translation cache (rsx_shader_spirv.cpp). A request is answered from
 * memory, else from PS3RECOMP_SPIRV_CACHE's directory, else by glslang. */
typedef struct RsxSpirvCacheStats {
    u32 memory_hits;
    u32 disk_hits;
    u32 translations;   /* glslang runs that succeeded */
    /* The preload (rsx_hlsl_to_spirv_cache_preload_start): */
    u32 preloaded;      /* entries it put in memory */
    u32 preload_hits;   /* entries it put there that a request then used */
    u32 preload_skipped;/* .spv files it could not use: another translator or
                           glslang, damaged, or not named for their key */
    u32 preload_done;   /* 1 once it has finished (or was stopped or capped) */
    u64 preload_bytes;  /* bytes of the files it used */
    u64 preload_ns;     /* its wall time, listing the directory included */
    u64 disk_read_ns;   /* time requests spent reading files themselves */
} RsxSpirvCacheStats;

void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out);

/* The name a translation of `hlsl` for `stage` has in the disk cache: the
 * file is "<16 lowercase hex digits>.spv" of this value (FNV-1a 64 of the
 * stage digit and the text). Available without glslang. */
u64 rsx_hlsl_to_spirv_cache_name(const char* hlsl, int stage);

/* Whether the file at `path` is one this translator would read: 1 when its
 * header (magic, format, glslang version, translator key) is this build's and
 * its sizes add up, 0 when it is a cache file another translator wrote (a
 * guaranteed miss, safe to delete), -1 when it cannot be read or is not a
 * cache file at all. Without glslang, always -1. */
int rsx_hlsl_to_spirv_cache_file_current(const char* path);

/* Empties the memory cache, zeroes the counters and reads
 * PS3RECOMP_SPIRV_CACHE again on the next request. A preload still running
 * is stopped and waited for first. Disk files stay. */
void rsx_hlsl_to_spirv_cache_reset(void);

/* Reads every usable file in PS3RECOMP_SPIRV_CACHE's directory into the
 * memory cache, on a worker thread, so the first request for a shader an
 * earlier session translated is a memory hit instead of a file read on the
 * caller's thread. Returns at once; a request for a file the worker has not
 * reached yet reads it as before. A file is used only when everything a
 * request would check matches (magic, format, glslang version, translator
 * key, sizes) and its name is the one its key gives; others are skipped and
 * left in place. At most RSX_SPIRV_PRELOAD_MAX_FILES files and
 * RSX_SPIRV_PRELOAD_MAX_BYTES bytes. Does nothing without the disk cache,
 * with PS3RECOMP_SPIRV_CACHE_PRELOAD=0, without glslang, or when a preload
 * has already started since the last reset. Logs one line when done. */
#define RSX_SPIRV_PRELOAD_MAX_FILES 8192u
#define RSX_SPIRV_PRELOAD_MAX_BYTES (256u << 20)
void rsx_hlsl_to_spirv_cache_preload_start(void);

/* Waits for a started preload to finish (tests). */
void rsx_hlsl_to_spirv_cache_preload_wait(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SHADER_SPIRV_H */
