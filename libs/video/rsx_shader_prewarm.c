/*
 * ps3recomp - warm the SPIR-V translation cache without running a title
 * (rsx_shader_prewarm.h).
 */
#include "rsx_shader_prewarm.h"
#include "rsx_shader_spirv.h"
#include "rsx_vp_decompiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The draw engine's buffer sizes (rsx_draw_engine.c's s_vs_hlsl/s_ps_hlsl);
 * room for any SPIR-V the engine's backend would accept. */
#define PREWARM_HLSL_BYTES (256u * 1024u)
#define PREWARM_SPV_WORDS  (1u << 20)

static int prewarm_text(const char* hlsl, int stage, rsx_shader_prewarm_result* out)
{
    out->name = rsx_hlsl_to_spirv_cache_name(hlsl, stage);
    u32* words = (u32*)malloc(PREWARM_SPV_WORDS * sizeof(u32));
    if (!words) {
        snprintf(out->log, sizeof out->log, "out of memory");
        return out->outcome = RSX_PREWARM_FAILED;
    }
    RsxSpirvCacheStats before, after;
    rsx_hlsl_to_spirv_cache_stats(&before);
    u32 n = 0;
    const int rc = rsx_hlsl_to_spirv(hlsl, stage, words, PREWARM_SPV_WORDS, &n,
                                     out->log, sizeof out->log);
    rsx_hlsl_to_spirv_cache_stats(&after);
    free(words);
    if (rc || !n) return out->outcome = RSX_PREWARM_FAILED;
    out->outcome = after.translations != before.translations ? RSX_PREWARM_TRANSLATED
                 : after.disk_hits != before.disk_hits     ? RSX_PREWARM_ON_DISK
                                                           : RSX_PREWARM_IN_MEMORY;
    return out->outcome;
}

int rsx_shader_prewarm_vertex(const u8* ucode, u32 max_bytes, u32 branch_base, u32 vtex_mask,
                              rsx_shader_prewarm_result* out)
{
    rsx_shader_prewarm_result scratch;
    if (!out) out = &scratch;
    memset(out, 0, sizeof *out);
    char* hlsl = (char*)malloc(PREWARM_HLSL_BYTES);
    if (!hlsl) {
        snprintf(out->log, sizeof out->log, "out of memory");
        return out->outcome = RSX_PREWARM_FAILED;
    }
    int rc;
    /* The engine hands the decompiler the program up to its end bit. */
    if (ucode) max_bytes = rsx_vp_program_size_instrs(ucode, max_bytes) * 16u;
    /* The engine passes its vertex layout, which is the program's own input
     * analysis (or all sixteen when that is uncertain); 0xFFFF lets the
     * builder make the same choice from the program. */
    if (!ucode || rsx_vp_build_text(ucode, max_bytes, branch_base, vtex_mask, 0xFFFFu,
                                    hlsl, PREWARM_HLSL_BYTES) <= 0) {
        snprintf(out->log, sizeof out->log, "the vertex program did not decompile");
        rc = out->outcome = RSX_PREWARM_FAILED;
    } else {
        rc = prewarm_text(hlsl, RSX_SHADER_STAGE_VERTEX, out);
    }
    free(hlsl);
    return rc;
}

int rsx_shader_prewarm_fragment(const u8* ucode, u32 max_bytes, const rsx_fp_text_state* st,
                                rsx_shader_prewarm_result* out)
{
    rsx_shader_prewarm_result scratch;
    if (!out) out = &scratch;
    memset(out, 0, sizeof *out);
    char* hlsl = (char*)malloc(PREWARM_HLSL_BYTES);
    if (!hlsl) {
        snprintf(out->log, sizeof out->log, "out of memory");
        return out->outcome = RSX_PREWARM_FAILED;
    }
    int rc;
    if (!ucode || !st || rsx_fp_build_text(ucode, max_bytes, st, hlsl, PREWARM_HLSL_BYTES, NULL) <= 0) {
        snprintf(out->log, sizeof out->log, "the fragment program did not decompile");
        rc = out->outcome = RSX_PREWARM_FAILED;
    } else {
        rc = prewarm_text(hlsl, RSX_SHADER_STAGE_FRAGMENT, out);
    }
    free(hlsl);
    return rc;
}
