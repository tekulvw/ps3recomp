/*
 * ps3recomp - HLSL -> SPIR-V through glslang, for the Vulkan backend
 *
 * See rsx_shader_spirv.h for the binding contract. The front end is driven
 * exactly as rsx_shader_msl.cpp drives it (C++ API for setEntryPoint, Vulkan
 * rules, auto-mapped bindings and locations, then mapIO, legalized SPIR-V),
 * with three differences, all because the output goes to a Vulkan driver
 * rather than to spirv-cross:
 *
 *   - Vulkan 1.0 client, SPIR-V 1.0: the backend creates a 1.0 instance and
 *     promises to run on 1.0-level drivers. (The MSL path targets 1.3 because
 *     that is spirv-cross's floor, not a driver's.)
 *   - Texture and sampler registers are shifted apart (setShiftBinding),
 *     because Vulkan has one binding namespace per set.
 *   - The vertex stage flips Y (setInvertY): D3D clip space is +y up.
 *
 * Translations are cached. glslang takes 20-150 ms per shader, on the thread
 * that walks the FIFO, and the engine asks for the same HLSL again for every
 * pipeline that shares a program (Twisted Metal: 236 translations in its
 * first minute, 74 distinct). The cache key is the stage plus the whole HLSL
 * text, so a hit returns exactly the words that text translated to. With
 * PS3RECOMP_SPIRV_CACHE=<existing directory> the cache is also kept on disk,
 * one file per shader, and the next run starts warm. A file whose key,
 * glslang version or translator key differs from the request is a miss,
 * and is rewritten. The translator key is PS3RECOMP_SPIRV_TRANSLATOR_KEY,
 * which CMake sets to a digest of this file and rsx_shader_spirv.h, so a
 * rebuild of the same translator keeps the cache; a build without it (a
 * test compiling this file directly) uses its build time instead.
 *
 * Reading those files one at a time, as draws first ask for them, puts small
 * file reads on the FIFO thread (usually well under a millisecond each, but
 * one Windows load frame spent 348 ms on 63 of them). The Vulkan backend
 * therefore starts rsx_hlsl_to_spirv_cache_preload_start when its guest
 * programs come up: a worker reads every usable file into memory, taking the
 * lock only to insert each entry, so by the time the game draws they are
 * memory hits. PS3RECOMP_SPIRV_CACHE_PRELOAD=0 turns it off.
 */

#include "rsx_shader_spirv.h"
#include "rsx_build_counters.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PS3RECOMP_HAVE_SPIRV_TRANSLATION)

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void put_log(char* log, u32 log_size, const char* what, const char* detail)
{
    if (!log || log_size == 0) return;
    snprintf(log, log_size, "%s%s%s", what ? what : "",
             (what && detail && *detail) ? ": " : "", detail ? detail : "");
}

bool glslang_ready()
{
    static bool inited = false;
    if (!inited) {
        if (!glslang::InitializeProcess()) return false;
        inited = true;
    }
    return true;
}

int translate(const char* hlsl, int stage, std::vector<unsigned int>& spirv,
              char* log, u32 log_size)
{
    if (!glslang_ready()) {
        put_log(log, log_size, "glslang::InitializeProcess failed", nullptr);
        return -1;
    }

    const EShLanguage lang = (stage == RSX_SHADER_STAGE_VERTEX) ? EShLangVertex
                                                                : EShLangFragment;
    const EShMessages msgs =
        (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);

    glslang::TShader shader(lang);
    const char* strings[1] = { hlsl };
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceHlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    /* SPIR-V FragCoord.w is 1/w (FXC's SV_POSITION.w is w): tells the FP
     * decompiler's _rsx_inv_w helper which one it gets. */
    shader.setPreamble("#define RSX_FRAGCOORD_W_IS_RCP 1\n");
    shader.setEntryPoint("main");
    shader.setAutoMapBindings(true);
    shader.setAutoMapLocations(true);
    shader.setShiftBinding(glslang::EResTexture, RSX_SPIRV_TEXTURE_BINDING);
    shader.setShiftBinding(glslang::EResSampler, RSX_SPIRV_SAMPLER_BINDING);
    if (lang == EShLangVertex) shader.setInvertY(true);

    if (!shader.parse(GetDefaultResources(), 100, false, msgs)) {
        put_log(log, log_size, "HLSL parse", shader.getInfoLog());
        return -1;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        put_log(log, log_size, "HLSL link", program.getInfoLog());
        return -1;
    }
    if (!program.mapIO()) {
        put_log(log, log_size, "HLSL io map", program.getInfoLog());
        return -1;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions opts;
    opts.disableOptimizer = false;     /* run the HLSL legalization passes */
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &opts);
    if (spirv.empty()) {
        put_log(log, log_size, "SPIR-V generation", logger.getAllMessages().c_str());
        return -1;
    }
    return 0;
}

/* ---- translation cache ------------------------------------------------- */

const u32 kCacheFormat = 1;
const u32 kCacheMagic = 0x56505352u;   /* "RSPV" */
/* The translator key stands for translate()'s options and the binding
 * contract in rsx_shader_spirv.h: when either changes, the files an older
 * translator wrote become misses. CMake passes a digest of both files
 * ("k" and 22 hex digits); without it, this file's compile time stands in,
 * which also makes every rebuild start cold. */
#if defined(PS3RECOMP_SPIRV_TRANSLATOR_KEY)
#define RSX_SPV_KEY_STR2(x) #x
#define RSX_SPV_KEY_STR(x) RSX_SPV_KEY_STR2(x)
const char kTranslatorBuild[24] = RSX_SPV_KEY_STR(PS3RECOMP_SPIRV_TRANSLATOR_KEY);
static_assert(sizeof(RSX_SPV_KEY_STR(PS3RECOMP_SPIRV_TRANSLATOR_KEY)) == 24, "k and 22 hex digits");
#else
const char kTranslatorBuild[24] = __DATE__ " " __TIME__;
#endif

struct Entry {
    std::vector<unsigned int> words;
    bool preloaded = false;   /* put here by the preload, and not used yet */
};

struct Cache {
    std::mutex lock;
    std::unordered_map<std::string, Entry> map;
    std::string dir;
    bool dir_read = false;
    RsxSpirvCacheStats stats = {};
    /* The preload worker. It is joined by reset and preload_wait; at process
     * exit it may still be running, which is why the cache is never
     * destroyed (cache()). */
    std::thread preload;
    bool preload_started = false;
    std::atomic<bool> preload_stop{false};
};

/* Never destroyed: a preload still running at exit keeps a valid cache, and a
 * joinable std::thread is never destroyed. */
Cache& cache()
{
    static Cache* c = new Cache;
    return *c;
}

uint64_t now_ns()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

const std::string& cache_dir(Cache& c)
{
    if (!c.dir_read) {
        const char* e = getenv("PS3RECOMP_SPIRV_CACHE");
        c.dir = (e && *e) ? e : "";
        c.dir_read = true;
        if (!c.dir.empty()) rsx_build_set_flag(RSX_BUILD_FLAG_SPIRV_DISK_CACHE);
    }
    return c.dir;
}

/* The key is the stage, then the HLSL text. */
std::string cache_key(const char* hlsl, int stage)
{
    std::string key(1, (char)('0' + stage));
    key += hlsl;
    return key;
}

/* "<16 hex digits>.spv": FNV-1a 64 of the key. */
std::string cache_name(const std::string& key)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    for (unsigned char ch : key) { h ^= ch; h *= 0x100000001b3ull; }
    char name[40];
    snprintf(name, sizeof name, "%016llx.spv", h);
    return name;
}

std::string cache_path(const std::string& dir, const std::string& key)
{
    return dir + "/" + cache_name(key);
}

/* Header: magic, format, glslang major/minor/patch, translator build (six
 * words), key bytes, word count; then the key and the words. */
const int kHeaderWords = 13;

void cache_header(u32 hdr[kHeaderWords], const std::string& key, size_t nwords)
{
    const glslang::Version v = glslang::GetVersion();
    hdr[0] = kCacheMagic; hdr[1] = kCacheFormat;
    hdr[2] = (u32)v.major; hdr[3] = (u32)v.minor; hdr[4] = (u32)v.patch;
    memcpy(&hdr[5], kTranslatorBuild, sizeof kTranslatorBuild);
    hdr[11] = (u32)key.size(); hdr[12] = (u32)nwords;
}

bool cache_load(const std::string& path, const std::string& key,
                std::vector<unsigned int>& spirv)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u32 want[kHeaderWords], hdr[kHeaderWords];
    cache_header(want, key, 0);
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 &&
              memcmp(hdr, want, (kHeaderWords - 1) * sizeof(u32)) == 0 &&
              hdr[kHeaderWords - 1] != 0 && hdr[kHeaderWords - 1] <= (1u << 24);
    if (ok) {
        std::string stored(key.size(), '\0');
        ok = fread(&stored[0], 1, stored.size(), f) == stored.size() && stored == key;
    }
    if (ok) {
        spirv.resize(hdr[kHeaderWords - 1]);
        ok = fread(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    }
    fclose(f);
    if (!ok) spirv.clear();
    return ok;
}

/* Written under a temporary name and renamed, so a reader never sees half a file. */
void cache_store(const std::string& path, const std::string& key,
                 const std::vector<unsigned int>& spirv)
{
    char suffix[48];
    snprintf(suffix, sizeof suffix, ".%zx.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string tmp = path + suffix;
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 hdr[kHeaderWords];
    cache_header(hdr, key, spirv.size());
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(key.data(), 1, key.size(), f) == key.size() &&
              fwrite(spirv.data(), sizeof(u32), spirv.size(), f) == spirv.size();
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok) remove(path.c_str());      /* rename does not replace on Windows */
#endif
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) remove(tmp.c_str());
}

/* ---- preload ------------------------------------------------------------ */

/* One file's contents as an entry, or false. The same checks as cache_load,
 * made without a request to compare against: the header must be this
 * translator's, the sizes must add up to the file, the stage must be one
 * there is, and the file must be named for the key it holds (so the request
 * path would have read this same file). */
bool preload_parse(const std::string& name, const std::vector<unsigned char>& bytes,
                   std::string& key, std::vector<unsigned int>& words)
{
    const size_t hdr_bytes = kHeaderWords * sizeof(u32);
    if (bytes.size() < hdr_bytes) return false;
    u32 hdr[kHeaderWords], want[kHeaderWords];
    memcpy(hdr, bytes.data(), hdr_bytes);
    cache_header(want, std::string(), 0);
    if (memcmp(hdr, want, (kHeaderWords - 2) * sizeof(u32)) != 0) return false;
    const u32 key_bytes = hdr[kHeaderWords - 2], nwords = hdr[kHeaderWords - 1];
    if (key_bytes < 2 || nwords == 0 || nwords > (1u << 24)) return false;
    if (bytes.size() != hdr_bytes + (size_t)key_bytes + (size_t)nwords * sizeof(u32)) return false;
    key.assign((const char*)bytes.data() + hdr_bytes, key_bytes);
    if (key[0] != (char)('0' + RSX_SHADER_STAGE_VERTEX) &&
        key[0] != (char)('0' + RSX_SHADER_STAGE_FRAGMENT)) return false;
    if (key.find('\0') != std::string::npos) return false;   /* the HLSL is a C string */
    if (cache_name(key) != name) return false;
    words.resize(nwords);
    memcpy(words.data(), bytes.data() + hdr_bytes + key_bytes, (size_t)nwords * sizeof(u32));
    return true;
}

bool preload_is_name(const std::string& name)
{
    if (name.size() != 20 || name.compare(16, 4, ".spv") != 0) return false;
    for (size_t i = 0; i < 16; i++) {
        const char ch = name[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

bool read_file(const std::filesystem::path& path, std::vector<unsigned char>& out, size_t limit)
{
    FILE* f = fopen(path.string().c_str(), "rb");
    if (!f) return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    const long size = ok ? ftell(f) : -1;
    ok = ok && size >= 0 && (size_t)size <= limit && fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize((size_t)size);
        ok = size == 0 || fread(out.data(), 1, out.size(), f) == out.size();
    }
    fclose(f);
    return ok;
}

void preload_files(Cache* c, const std::string& dir);

/* An exception leaving a thread ends the process, so none does: a failure
 * here (a path that does not convert, no memory) only ends the preload, and
 * requests read the files as before. */
void preload_run(Cache* c, std::string dir)
{
    try {
        preload_files(c, dir);
    } catch (...) {
        fprintf(stderr, "[RSX spirv] cache preload: stopped by an error, files are read as draws need them\n");
        std::lock_guard<std::mutex> guard(c->lock);
        c->stats.preload_done = 1;
    }
}

void preload_files(Cache* c, const std::string& dir)
{
    const uint64_t t0 = now_ns();
    u32 used = 0, skipped = 0, files = 0;
    uint64_t bytes_used = 0;
    bool capped = false;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    std::vector<unsigned char> bytes;
    std::string key;
    std::vector<unsigned int> words;
    for (; !ec && it != end && !c->preload_stop.load(std::memory_order_relaxed); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!preload_is_name(name)) continue;    /* temporaries and anything else */
        if (files >= RSX_SPIRV_PRELOAD_MAX_FILES || bytes_used >= RSX_SPIRV_PRELOAD_MAX_BYTES) {
            capped = true;
            break;
        }
        files++;
        if (!read_file(it->path(), bytes, RSX_SPIRV_PRELOAD_MAX_BYTES - bytes_used) ||
            !preload_parse(name, bytes, key, words)) {
            skipped++;
            continue;
        }
        std::lock_guard<std::mutex> guard(c->lock);
        if (c->preload_stop.load(std::memory_order_relaxed)) break;
        Entry e;
        e.words = std::move(words);
        e.preloaded = true;
        if (c->map.emplace(std::move(key), std::move(e)).second) {
            used++;
            bytes_used += bytes.size();
            c->stats.preloaded = used;
            c->stats.preload_bytes = bytes_used;
        }
        key.clear();
        words.clear();
    }
    const uint64_t ns = now_ns() - t0;
    {
        std::lock_guard<std::mutex> guard(c->lock);
        c->stats.preload_skipped = skipped;
        c->stats.preload_ns = ns;
        c->stats.preload_done = 1;
    }
    fprintf(stderr, "[RSX spirv] cache preload: %u shaders (%llu KB) in %.1f ms, %u skipped%s\n",
            used, (unsigned long long)(bytes_used >> 10), (double)ns / 1e6, skipped,
            capped ? ", stopped at the size cap" : (ec ? ", directory listing failed" : ""));
}

} // namespace

extern "C" int rsx_hlsl_to_spirv_available(void) { return 1; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    if (out_words) *out_words = 0;
    if (!hlsl || !words || !out_words || max_words == 0) {
        put_log(log, log_size, "bad arguments", nullptr);
        return -1;
    }
    if (stage != RSX_SHADER_STAGE_VERTEX && stage != RSX_SHADER_STAGE_FRAGMENT) {
        put_log(log, log_size, "unknown shader stage", nullptr);
        return -1;
    }

    /* One lock around the whole lookup and translation: glslang's process
     * state is not documented as thread-safe either. */
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    const std::string key = cache_key(hlsl, stage);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
        c.stats.memory_hits++;
        if (it->second.preloaded) { it->second.preloaded = false; c.stats.preload_hits++; }
        rsx_build_note_shader_hit(0);
    } else {
        std::vector<unsigned int> spirv;
        const std::string& dir = cache_dir(c);
        const std::string path = dir.empty() ? std::string() : cache_path(dir, key);
        const uint64_t read0 = dir.empty() ? 0 : now_ns();
        const bool loaded = !dir.empty() && cache_load(path, key, spirv);
        if (!dir.empty()) c.stats.disk_read_ns += now_ns() - read0;
        if (loaded) {
            c.stats.disk_hits++;
            rsx_build_note_shader_hit(1);
        } else {
            const uint64_t t0 = rsx_build_now_ns();
            const int failed = translate(hlsl, stage, spirv, log, log_size);
            rsx_build_note(RSX_BUILD_TRANSLATION, rsx_build_now_ns() - t0, 0);
            if (failed) return -1;
            c.stats.translations++;
            if (!dir.empty()) cache_store(path, key, spirv);
        }
        Entry e;
        e.words = std::move(spirv);
        it = c.map.emplace(key, std::move(e)).first;
    }
    const std::vector<unsigned int>& spirv = it->second.words;
    if (spirv.size() > max_words) {
        char n[64];
        snprintf(n, sizeof n, "%zu words, room for %u", spirv.size(), max_words);
        put_log(log, log_size, "SPIR-V output too large", n);
        return -1;
    }
    memcpy(words, spirv.data(), spirv.size() * sizeof(u32));
    *out_words = (u32)spirv.size();
    return 0;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (out) *out = c.stats;
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void)
{
    Cache& c = cache();
    c.preload_stop.store(true, std::memory_order_relaxed);
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(c.lock);
        worker = std::move(c.preload);
    }
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> guard(c.lock);
    c.map.clear();
    c.dir_read = false;
    c.stats = {};
    c.preload_started = false;
    c.preload_stop.store(false, std::memory_order_relaxed);
}

extern "C" void rsx_hlsl_to_spirv_cache_preload_start(void)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> guard(c.lock);
    if (c.preload_started) return;
    c.preload_started = true;
    const char* off = getenv("PS3RECOMP_SPIRV_CACHE_PRELOAD");
    if (off && strcmp(off, "0") == 0) {
        fprintf(stderr, "[RSX spirv] cache preload: off (PS3RECOMP_SPIRV_CACHE_PRELOAD=0)\n");
        return;
    }
    const std::string& dir = cache_dir(c);
    if (dir.empty()) return;
    try {
        c.preload = std::thread(preload_run, &c, dir);
    } catch (...) {
        fprintf(stderr, "[RSX spirv] cache preload: no thread, files are read as draws need them\n");
    }
}

extern "C" void rsx_hlsl_to_spirv_cache_preload_wait(void)
{
    Cache& c = cache();
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(c.lock);
        worker = std::move(c.preload);
    }
    if (worker.joinable()) worker.join();
}

#else /* !PS3RECOMP_HAVE_SPIRV_TRANSLATION */

extern "C" int rsx_hlsl_to_spirv_available(void) { return 0; }

extern "C" int rsx_hlsl_to_spirv(const char* hlsl, int stage,
                                 u32* words, u32 max_words, u32* out_words,
                                 char* log, u32 log_size)
{
    (void)hlsl; (void)stage; (void)words; (void)max_words;
    if (out_words) *out_words = 0;
    if (log && log_size)
        snprintf(log, log_size, "HLSL -> SPIR-V translation not built (glslang not found)");
    return -1;
}

extern "C" void rsx_hlsl_to_spirv_cache_stats(RsxSpirvCacheStats* out)
{
    if (out) memset(out, 0, sizeof *out);
}

extern "C" void rsx_hlsl_to_spirv_cache_reset(void) {}
extern "C" void rsx_hlsl_to_spirv_cache_preload_start(void) {}
extern "C" void rsx_hlsl_to_spirv_cache_preload_wait(void) {}

#endif
