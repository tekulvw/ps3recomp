/*
 * ps3recomp - warm the SPIR-V translation cache without running a title
 *
 * The draw engine translates each guest program pair to SPIR-V the first
 * time a draw needs it, on the thread that walks the FIFO. With
 * PS3RECOMP_SPIRV_CACHE set the translations are kept on disk, so only a
 * title's first session pays. A port that can find its title's programs
 * without running it (in its own data files, say) can pay that cost earlier,
 * at install time, with these calls: each one makes the text the engine
 * would compile for one program in one described state, with the engine's
 * own builders (rsx_vp_build_text, rsx_fp_build_text), and translates it
 * through rsx_hlsl_to_spirv, which writes the file the engine will look up.
 *
 * A described state the engine never meets only costs a file; a program or
 * state it does meet finds its file. The calls are not thread-safe with each
 * other (glslang runs under one lock anyway); run several processes, each
 * with its share, for parallelism. Built with the Vulkan backend, like
 * rsx_shader_spirv.cpp.
 */
#ifndef PS3RECOMP_RSX_SHADER_PREWARM_H
#define PS3RECOMP_RSX_SHADER_PREWARM_H

#include "ps3emu/ps3types.h"
#include "rsx_fp_decompiler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rsx_shader_prewarm_outcome {
    RSX_PREWARM_FAILED = -1,      /* no text, or the translation failed */
    RSX_PREWARM_TRANSLATED = 0,   /* translated (and written, with a disk cache) */
    RSX_PREWARM_ON_DISK = 1,      /* the disk cache already held it */
    RSX_PREWARM_IN_MEMORY = 2,    /* this process translated or read it already */
} rsx_shader_prewarm_outcome;

typedef struct rsx_shader_prewarm_result {
    int outcome;                  /* rsx_shader_prewarm_outcome */
    u64 name;                     /* the cache file's name (rsx_hlsl_to_spirv_cache_name) */
    char log[256];                /* why, when it failed */
} rsx_shader_prewarm_result;

/* A vertex program: `ucode` is its words as the guest uploads them to the
 * transform program memory (little-endian), `branch_base` the slot it is
 * loaded at (0 for a program as stored, whose branch targets count from its
 * first instruction), `vtex_mask` the vertex-texture units a draw enables. */
int rsx_shader_prewarm_vertex(const u8* ucode, u32 max_bytes, u32 branch_base, u32 vtex_mask,
                              rsx_shader_prewarm_result* out);

/* A fragment program: `ucode` as the RSX reads it from memory (big-endian,
 * half-word swapped, inline constants in place), `st` the draw state, as
 * for rsx_fp_build_text. */
int rsx_shader_prewarm_fragment(const u8* ucode, u32 max_bytes, const rsx_fp_text_state* st,
                                rsx_shader_prewarm_result* out);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_SHADER_PREWARM_H */
