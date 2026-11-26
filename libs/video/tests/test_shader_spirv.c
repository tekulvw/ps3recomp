/*
 * ps3recomp - HLSL -> SPIR-V translation cache test
 *
 * rsx_hlsl_to_spirv keeps every translation it makes, in memory and, with
 * PS3RECOMP_SPIRV_CACHE=<dir>, on disk. This checks that:
 *   - the same stage and text is translated once, and the hit returns the
 *     same words;
 *   - the same text for another stage is its own entry;
 *   - a new process (simulated by rsx_hlsl_to_spirv_cache_reset) is served
 *     from the disk file, with the same words;
 *   - a damaged file is a miss, and is rewritten;
 *   - a failed translation is not cached;
 *   - a hit larger than the caller's buffer still fails as before;
 *   - the preload reads every usable file into memory, so the next requests
 *     are memory hits with no file read; it skips files of another
 *     translator, damaged or misnamed files and temporaries, leaves entries
 *     already in memory alone, gives the same words to a request racing it,
 *     and stays off with PS3RECOMP_SPIRV_CACHE_PRELOAD=0 or no disk cache.
 *
 * Built as a CMake target (test_shader_spirv) because it links glslang.
 * POSIX only (mkdtemp). Needs no GPU.
 */
#include "../rsx_shader_spirv.h"
#include "../rsx_build_counters.h"   /* the build counters see the same events */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

static void check(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) g_pass++; else g_fail++;
}

static const char* kVS =
    "struct VOut { float4 pos : SV_Position; float4 col : COLOR0; };\n"
    "VOut main(float4 p : ATTR0, float4 c : ATTR3) {\n"
    "    VOut o; o.pos = p; o.col = c; return o;\n"
    "}\n";

static const char* kPS =
    "float4 main(float4 pos : SV_Position, float4 col : COLOR0) : SV_Target0 {\n"
    "    return col.zyxw;\n"
    "}\n";

/* Valid as either stage: no inputs, one output. */
static const char* kBoth =
    "float4 main() : SV_Target0 { return float4(1, 0, 0, 1); }\n";

#define MAX_WORDS 4096u
static u32 g_a[MAX_WORDS], g_b[MAX_WORDS];

static int translate(const char* hlsl, int stage, u32* words, u32* n)
{
    char log[512] = "";
    int r = rsx_hlsl_to_spirv(hlsl, stage, words, MAX_WORDS, n, log, sizeof log);
    if (r) printf("       (%s)\n", log);
    return r;
}

static RsxSpirvCacheStats stats(void)
{
    RsxSpirvCacheStats s;
    rsx_hlsl_to_spirv_cache_stats(&s);
    return s;
}

/* The one .spv file in dir, or "" when there is not exactly one. */
static void only_file(const char* dir, char* out, size_t size)
{
    DIR* d = opendir(dir);
    int n = 0;
    out[0] = 0;
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".spv") == 0) {
            snprintf(out, size, "%s/%s", dir, e->d_name);
            n++;
        }
    }
    closedir(d);
    if (n != 1) out[0] = 0;
}

/* Every .spv file in dir, up to max, into names; returns how many. */
static int spv_files(const char* dir, char names[][1024], int max)
{
    DIR* d = opendir(dir);
    int n = 0;
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcmp(e->d_name + len - 4, ".spv") == 0)
            snprintf(names[n++], 1024, "%s/%s", dir, e->d_name);
    }
    closedir(d);
    return n;
}

static void remove_all(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    char path[1024];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

static long file_bytes(const char* path, unsigned char* buf, long max)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(buf, 1, (size_t)max, f);
    fclose(f);
    return n;
}

static int write_bytes(const char* path, const unsigned char* buf, long n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static unsigned char g_file[1 << 20];

static void preload_tests(void)
{
    u32 vs[MAX_WORDS], ps[MAX_WORDS], nv = 0, np = 0, n = 0;
    RsxSpirvCacheStats s;
    char dir[] = "/tmp/ps3recomp_spirv_preload_XXXXXX";
    if (!mkdtemp(dir)) { check("preload: mkdtemp", 0); return; }
    setenv("PS3RECOMP_SPIRV_CACHE", dir, 1);
    unsetenv("PS3RECOMP_SPIRV_CACHE_PRELOAD");
    rsx_hlsl_to_spirv_cache_reset();

    /* An earlier session: two shaders on disk. */
    check("preload: earlier session translates two shaders",
          translate(kVS, RSX_SHADER_STAGE_VERTEX, vs, &nv) == 0 &&
          translate(kPS, RSX_SHADER_STAGE_FRAGMENT, ps, &np) == 0);
    static char names[8][1024];
    check("... and writes two files", spv_files(dir, names, 8) == 2);

    /* Files the preload must not use, next to them. */
    long len = file_bytes(names[0], g_file, sizeof g_file);
    char path[1100];
    int made = len > 60;
    /* The same contents under a name its key does not give. */
    snprintf(path, sizeof path, "%s/0123456789abcdef.spv", dir);
    made = made && write_bytes(path, g_file, len) == 0;
    /* Another translator: one byte of the translator key differs. Named like
     * a cache file, so it is read and rejected rather than ignored. */
    g_file[5 * 4] ^= 0x5A;
    snprintf(path, sizeof path, "%s/fedcba9876543210.spv", dir);
    made = made && write_bytes(path, g_file, len) == 0;
    g_file[5 * 4] ^= 0x5A;
    /* Damaged: cut short. */
    snprintf(path, sizeof path, "%s/00000000000000aa.spv", dir);
    made = made && write_bytes(path, g_file, len - 7) == 0;
    /* A temporary and a stray file: not cache files at all. */
    snprintf(path, sizeof path, "%s/%s", dir, "1111111111111111.spv.abc.tmp");
    made = made && write_bytes(path, g_file, len) == 0;
    snprintf(path, sizeof path, "%s/%s", dir, "notes.txt");
    made = made && write_bytes(path, (const unsigned char*)"x", 1) == 0;
    check("preload: unusable files written", made);

    /* A new session with the preload. */
    rsx_hlsl_to_spirv_cache_reset();
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_preload_wait();
    s = stats();
    check("preload: both shaders in memory, the three unusable files skipped",
          s.preload_done == 1 && s.preloaded == 2 && s.preload_skipped == 3 && s.preload_bytes > 0);
    check("... before any request", s.memory_hits == 0 && s.disk_hits == 0 && s.translations == 0);
    check("preload: vertex request succeeds", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &n) == 0);
    check("... with the same words", n == nv && memcmp(g_a, vs, nv * 4u) == 0);
    check("preload: fragment request succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &n) == 0);
    check("... with the same words", n == np && memcmp(g_a, ps, np * 4u) == 0);
    s = stats();
    check("... both memory hits served by the preload, no file read, no glslang",
          s.memory_hits == 2 && s.preload_hits == 2 && s.disk_hits == 0 && s.translations == 0 &&
          s.disk_read_ns == 0);
    translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &n);
    s = stats();
    check("... a second use is a plain memory hit", s.memory_hits == 3 && s.preload_hits == 2);
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_preload_wait();
    s = stats();
    check("preload: starts once per session", s.preloaded == 2 && s.preload_skipped == 3);
    {
        int left = spv_files(dir, names, 8);
        check("preload: leaves every file in place", left == 5);
    }

    /* An entry a request already loaded is left alone. */
    rsx_hlsl_to_spirv_cache_reset();
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &n);
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_preload_wait();
    s = stats();
    check("preload: only the entry not yet in memory", s.disk_hits == 1 && s.preloaded == 1);

    /* A request racing the worker gets the same words either way. */
    rsx_hlsl_to_spirv_cache_reset();
    rsx_hlsl_to_spirv_cache_preload_start();
    int race_ok = translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &n) == 0 &&
                  n == np && memcmp(g_a, ps, np * 4u) == 0;
    rsx_hlsl_to_spirv_cache_preload_wait();
    s = stats();
    check("preload: a racing request gets the same words, without glslang",
          race_ok && s.translations == 0 && s.disk_hits + s.memory_hits == 1 &&
          s.preloaded + s.disk_hits == 2);

    /* A reset while the worker may still run stops it and starts clean. */
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_reset();
    s = stats();
    check("preload: reset stops it and clears the counters", s.preloaded == 0 && s.preload_done == 0);

    /* The off switch. */
    setenv("PS3RECOMP_SPIRV_CACHE_PRELOAD", "0", 1);
    rsx_hlsl_to_spirv_cache_reset();
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_preload_wait();
    translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &n);
    s = stats();
    check("preload: off with PS3RECOMP_SPIRV_CACHE_PRELOAD=0, the file is read on request",
          s.preloaded == 0 && s.preload_done == 0 && s.disk_hits == 1 && s.translations == 0);
    unsetenv("PS3RECOMP_SPIRV_CACHE_PRELOAD");

    /* No disk cache, no preload. */
    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();
    rsx_hlsl_to_spirv_cache_preload_start();
    rsx_hlsl_to_spirv_cache_preload_wait();
    s = stats();
    check("preload: nothing without PS3RECOMP_SPIRV_CACHE", s.preloaded == 0 && s.preload_done == 0);

    remove_all(dir);
    rmdir(dir);
}

int main(void)
{
    u32 na = 0, nb = 0;
    RsxSpirvCacheStats s;

    if (!rsx_hlsl_to_spirv_available()) {
        printf("[FAIL] built without glslang\n");
        return 1;
    }
    unsetenv("PS3RECOMP_SPIRV_CACHE");
    rsx_hlsl_to_spirv_cache_reset();
    rsx_build_stats_reset();

    /* Memory. */
    check("vertex program translates", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0 && na > 5);
    check("SPIR-V magic", g_a[0] == 0x07230203u);
    check("same request again succeeds", translate(kVS, RSX_SHADER_STAGE_VERTEX, g_b, &nb) == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    s = stats();
    check("... served from memory, glslang ran once", s.translations == 1 && s.memory_hits == 1);

    check("fragment program translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    check("same text as a vertex program", translate(kBoth, RSX_SHADER_STAGE_VERTEX, g_a, &na) == 0);
    check("same text as a fragment program", translate(kBoth, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... two entries, not a hit", s.translations == 4 && s.memory_hits == 1);
    check("... with other words", na != nb || memcmp(g_a, g_b, na * 4u) != 0);

    {
        u32 small[4], n = 123;
        int r = rsx_hlsl_to_spirv(kPS, RSX_SHADER_STAGE_FRAGMENT, small, 4, &n, NULL, 0);
        check("a hit larger than the buffer fails", r == -1 && n == 0);
    }

    check("broken HLSL fails", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    check("... and again (not cached)", translate("float4 main( {", RSX_SHADER_STAGE_FRAGMENT, g_a, &na) != 0);
    s = stats();
    check("... counted as no translation", s.translations == 4);
    {
        struct ps3recomp_rsx_build_stats b;
        ps3recomp_rsx_build_stats(&b);
        /* A failed translation still took glslang's time, so it counts as one; the
         * too-small buffer request was the second memory hit. */
        check("build counters: every glslang run, timed, and the memory hit",
              b.shader_translations == 6 && b.shader_translation_ns > 0 && b.shader_memory_hits == 2 &&
              b.shader_disk_hits == 0 && !(b.flags & RSX_BUILD_FLAG_SPIRV_DISK_CACHE));
    }

    /* Disk. */
    char dir[] = "/tmp/ps3recomp_spirv_cache_XXXXXX";
    if (!mkdtemp(dir)) { printf("[FAIL] mkdtemp\n"); return 1; }
    setenv("PS3RECOMP_SPIRV_CACHE", dir, 1);
    rsx_hlsl_to_spirv_cache_reset();

    check("disk: first run translates", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_a, &na) == 0);
    s = stats();
    check("... with glslang", s.translations == 1 && s.disk_hits == 0);
    char path[1024];
    only_file(dir, path, sizeof path);
    check("... and writes one file", path[0] != 0);

    rsx_hlsl_to_spirv_cache_reset();
    check("disk: next run succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... from the file, no glslang", s.disk_hits == 1 && s.translations == 0);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... then from memory", s.memory_hits == 1 && s.disk_hits == 1);
    {
        struct ps3recomp_rsx_build_stats b;
        ps3recomp_rsx_build_stats(&b);
        check("build counters: the disk hit, and the disk cache flag",
              b.shader_disk_hits == 1 && (b.flags & RSX_BUILD_FLAG_SPIRV_DISK_CACHE));
    }

    /* Damage the stored key: the file no longer matches the request. */
    if (path[0]) {
        FILE* f = fopen(path, "r+b");
        if (f) { fseek(f, 13 * 4 + 10, SEEK_SET); fputc('#', f); fclose(f); }
    }
    rsx_hlsl_to_spirv_cache_reset();
    check("disk: damaged file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
    s = stats();
    check("... as a miss, with glslang", s.disk_hits == 0 && s.translations == 1);
    check("... with the same words", na == nb && memcmp(g_a, g_b, na * 4u) == 0);
    rsx_hlsl_to_spirv_cache_reset();
    translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb);
    s = stats();
    check("... and the file was rewritten", s.disk_hits == 1 && s.translations == 0);

    /* A truncated file. */
    if (path[0] && truncate(path, 20) == 0) {
        rsx_hlsl_to_spirv_cache_reset();
        check("disk: truncated file still succeeds", translate(kPS, RSX_SHADER_STAGE_FRAGMENT, g_b, &nb) == 0);
        s = stats();
        check("... as a miss", s.disk_hits == 0 && s.translations == 1);
    } else {
        check("truncate", 0);
    }

    only_file(dir, path, sizeof path);
    if (path[0]) remove(path);
    rmdir(dir);

    preload_tests();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
