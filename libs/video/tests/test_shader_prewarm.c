/*
 * ps3recomp - SPIR-V cache warm-up test (rsx_shader_prewarm.h)
 *
 * With PS3RECOMP_SPIRV_CACHE set, a warmed program and state leave the file
 * a later draw of the same program in the same state reads:
 *   - its name is the cache name of the builders' text, which
 *     test_engine_shader_text shows is the draw engine's text;
 *   - a later process (rsx_hlsl_to_spirv_cache_reset) asking for that text
 *     is a disk hit, and warming it again reports it as on disk;
 *   - state on units the program does not sample names the same file;
 *   - rsx_hlsl_to_spirv_cache_file_current accepts the file, and calls one
 *     from another translator stale and a foreign file not a cache file.
 *
 * Synthetic programs only. Built as a CMake target (it links glslang).
 * POSIX only (mkdtemp). Needs no GPU.
 */
#include "../rsx_shader_prewarm.h"
#include "../rsx_shader_spirv.h"
#include "../rsx_fp_decompiler.h"
#include "../rsx_vp_decompiler.h"
#include "../rsx_test_programs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
static void check(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static char g_dir[256];
static char g_text[256 * 1024];

static void path_of(u64 name, char* out, size_t n)
{
    snprintf(out, n, "%s/%016llx.spv", g_dir, (unsigned long long)name);
}
static int exists(u64 name)
{
    char p[512]; path_of(name, p, sizeof p);
    return access(p, F_OK) == 0;
}

#define WORDS 1u << 20
static u32 g_words[WORDS];

int main(void)
{
    if (!rsx_hlsl_to_spirv_available()) { printf("[SKIP] no glslang\n"); return 0; }
    snprintf(g_dir, sizeof g_dir, "%s/prewarm.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 2; }
    setenv("PS3RECOMP_SPIRV_CACHE", g_dir, 1);
    rsx_hlsl_to_spirv_cache_reset();

    /* MOV o0, v0 ; MOV o7, v8 (end), and TEX r0, tc0 on unit 0. */
    u8 vp[32];
    rsx_test_vp_mov_out(vp, 0, RSX_TEST_VP_SWZ_IDENT, 0, 0);
    rsx_test_vp_mov_out(vp + 16, 8, RSX_TEST_VP_SWZ_IDENT, 7, 1);
    u8 fp[16];
    rsx_test_fp_tex_r0_tc0(fp, 0);
    rsx_fp_text_state st;
    memset(&st, 0, sizeof st);
    st.ctrl = 0x40;

    rsx_shader_prewarm_result r;
    int rc = rsx_shader_prewarm_vertex(vp, sizeof vp, 0, 0, &r);
    check("a vertex program is translated", rc == RSX_PREWARM_TRANSLATED && r.outcome == rc);
    rsx_vp_build_text(vp, sizeof vp, 0, 0, 0xFFFFu, g_text, sizeof g_text);
    check("into the file its builder text names",
          r.name == rsx_hlsl_to_spirv_cache_name(g_text, RSX_SHADER_STAGE_VERTEX) && exists(r.name));
    const u64 vs_name = r.name;

    rc = rsx_shader_prewarm_fragment(fp, sizeof fp, &st, &r);
    check("a fragment program is translated", rc == RSX_PREWARM_TRANSLATED);
    rsx_fp_build_text(fp, sizeof fp, &st, g_text, sizeof g_text, NULL);
    check("into the file its builder text names",
          r.name == rsx_hlsl_to_spirv_cache_name(g_text, RSX_SHADER_STAGE_FRAGMENT) && exists(r.name));
    const u64 fs_name = r.name;
    check("the name is FNV-1a 64 of the stage digit and the text",
          rsx_hlsl_to_spirv_cache_name("x", 1) == ((0xcbf29ce484222325ull ^ '1') * 0x100000001b3ull ^ 'x') * 0x100000001b3ull);

    rc = rsx_shader_prewarm_fragment(fp, sizeof fp, &st, &r);
    check("the same request again is served from memory", rc == RSX_PREWARM_IN_MEMORY && r.name == fs_name);

    /* A cube map and an sRGB texture on units the program does not sample. */
    rsx_fp_text_state stale = st;
    stale.cube_mask = 1u << 14;
    stale.gamma_masks[3] = 7;
    stale.shadow[11].enabled = 1;
    rc = rsx_shader_prewarm_fragment(fp, sizeof fp, &stale, &r);
    check("unsampled cube, gamma and shadow state name the same file", r.name == fs_name);
    /* Gamma on the sampled unit is a variant of its own. */
    rsx_fp_text_state gamma = st;
    gamma.gamma_masks[0] = 7;
    rc = rsx_shader_prewarm_fragment(fp, sizeof fp, &gamma, &r);
    check("gamma on the sampled unit is another file", rc == RSX_PREWARM_TRANSLATED && r.name != fs_name);

    /* A later process. */
    rsx_hlsl_to_spirv_cache_reset();
    RsxSpirvCacheStats s0, s1;
    rsx_hlsl_to_spirv_cache_stats(&s0);
    u32 n = 0;
    rsx_fp_build_text(fp, sizeof fp, &st, g_text, sizeof g_text, NULL);
    rc = rsx_hlsl_to_spirv(g_text, RSX_SHADER_STAGE_FRAGMENT, g_words, WORDS, &n, NULL, 0);
    rsx_hlsl_to_spirv_cache_stats(&s1);
    check("the next process's request for that text is a disk hit",
          rc == 0 && n > 0 && s1.disk_hits == s0.disk_hits + 1 && s1.translations == s0.translations);
    rc = rsx_shader_prewarm_vertex(vp, sizeof vp, 0, 0, &r);
    check("and warming the vertex program again finds it on disk", rc == RSX_PREWARM_ON_DISK && r.name == vs_name);

    /* The file check. */
    char p[512];
    path_of(fs_name, p, sizeof p);
    check("a file this translator wrote is current", rsx_hlsl_to_spirv_cache_file_current(p) == 1);
    {
        FILE* f = fopen(p, "r+b");
        u32 w[13] = { 0 };
        if (f && fread(w, sizeof w, 1, f) == 1) {
            w[2] ^= 1u;                           /* another glslang major version */
            fseek(f, 0, SEEK_SET);
            fwrite(w, sizeof w, 1, f);
        }
        if (f) fclose(f);
    }
    check("one from another glslang is stale", rsx_hlsl_to_spirv_cache_file_current(p) == 0);
    char foreign[512];
    snprintf(foreign, sizeof foreign, "%s/notes.txt", g_dir);
    FILE* f = fopen(foreign, "wb");
    if (f) { fputs("not a cache file, longer than a cache header is.....\n", f); fclose(f); }
    check("a foreign file is not a cache file", rsx_hlsl_to_spirv_cache_file_current(foreign) == -1);
    check("a missing file is not one either", rsx_hlsl_to_spirv_cache_file_current("/nonexistent/x.spv") == -1);

    /* A program that does not decompile. */
    u8 junk[16];
    memset(junk, 0, sizeof junk);
    rc = rsx_shader_prewarm_vertex(junk, sizeof junk, 0, 0, &r);
    check("an unterminated vertex program fails with a reason", rc == RSX_PREWARM_FAILED && r.log[0]);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0) printf("(could not remove %s)\n", g_dir);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
