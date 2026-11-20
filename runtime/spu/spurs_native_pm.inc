/* spurs_native_pm.inc -- the native policy-module seam (spurs_native_pm.h).
 * Included once, by spurs_policy.c, so every program that links the policy
 * runner has it. Nothing here runs unless PS3RECOMP_SPURS_NATIVE_PM selects a
 * registered implementation; no implementation is registered by the runtime. */
#include "spurs_native_pm.h"

/* ---- SHA-256 (FIPS 180-4) -------------------------------------------------- */
static const uint32_t s_npm_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u };
#define NPM_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void spurs_npm_sha_block(uint32_t h[8], const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = NPM_ROR(w[i - 15], 7) ^ NPM_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = NPM_ROR(w[i - 2], 17) ^ NPM_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (NPM_ROR(e, 6) ^ NPM_ROR(e, 11) ^ NPM_ROR(e, 25)) + ((e & f) ^ (~e & g)) + s_npm_k[i] + w[i];
        uint32_t t2 = (NPM_ROR(a, 2) ^ NPM_ROR(a, 13) ^ NPM_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
#undef NPM_ROR
void spurs_native_pm_sha256(const uint8_t* data, size_t n, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) spurs_npm_sha_block(h, data + i);
    uint8_t tail[128] = {0};
    size_t r = n - i;
    if (r) memcpy(tail, data + i, r);
    tail[r] = 0x80;
    size_t blocks = r + 9 <= 64 ? 1 : 2;
    uint64_t bits = (uint64_t)n * 8u;
    for (int b = 0; b < 8; b++) tail[blocks * 64 - 1 - b] = (uint8_t)(bits >> (8 * b));
    for (size_t b = 0; b < blocks; b++) spurs_npm_sha_block(h, tail + 64 * b);
    for (int j = 0; j < 8; j++) {
        out[4 * j] = (uint8_t)(h[j] >> 24); out[4 * j + 1] = (uint8_t)(h[j] >> 16);
        out[4 * j + 2] = (uint8_t)(h[j] >> 8); out[4 * j + 3] = (uint8_t)h[j];
    }
}

/* ---- registry ---------------------------------------------------------------- */
enum { SPURS_NPM_MAX = 8 };
static const struct spurs_native_pm* s_npm[SPURS_NPM_MAX];
static unsigned s_npm_count;
static SRWLOCK s_npm_lock = SRWLOCK_INIT;
static unsigned long s_npm_consults;
static spurs_native_pm_payload_fn s_npm_payload;
static int s_npm_mode = -1;
static const struct spurs_native_pm* s_npm_shadow[16];
static int s_npm_self_shadow;
static char s_npm_name[64];

/* The window lies wholly below the policy image base and ends at the kernel
 * services, so no policy image byte and no kernel address is ever a slot. */
typedef char spurs_npm_window_below_image[(SPURS_NATIVE_PM_WINDOW_END <= 0xA00u &&
    SPURS_NATIVE_PM_WINDOW_END == SPURS_PM_EXIT_TO_KERNEL_LS &&
    SPURS_NATIVE_PM_WINDOW_LS + SPURS_NATIVE_PM_SLOTS * SPURS_NATIVE_PM_SLOT_BYTES == SPURS_NATIVE_PM_WINDOW_END)
    ? 1 : -1];

int spurs_native_pm_register(const struct spurs_native_pm* pm)
{
    if (!pm || !pm->run || !pm->name || !*pm->name) return -1;
    if (pm->services && pm->services->slot[SPURS_NATIVE_PM_SLOTS - 1]) return -1;  /* job-return slot */
    int rc = -1;
    AcquireSRWLockExclusive(&s_npm_lock);
    unsigned i = 0;
    for (; i < s_npm_count; i++) if (s_npm[i]->fingerprint == pm->fingerprint) break;
    if (i == s_npm_count && s_npm_count < SPURS_NPM_MAX) { s_npm[s_npm_count++] = pm; rc = 0; }
    ReleaseSRWLockExclusive(&s_npm_lock);
    return rc;
}
void spurs_native_pm_unregister_all(void)
{
    AcquireSRWLockExclusive(&s_npm_lock);
    s_npm_count = 0;
    ReleaseSRWLockExclusive(&s_npm_lock);
}

int spurs_native_pm_mode(void)
{
    int m = __atomic_load_n(&s_npm_mode, __ATOMIC_ACQUIRE);
    if (m >= 0) return m;
    AcquireSRWLockExclusive(&s_npm_lock);
    if (s_npm_mode < 0) {
        const char* e = getenv("PS3RECOMP_SPURS_NATIVE_PM");
        m = SPURS_NATIVE_PM_OFF;
        if (e && *e && strcmp(e, "0") != 0) {
            if (!strcmp(e, "1")) m = SPURS_NATIVE_PM_ON;
            else if (!strcmp(e, "check")) m = SPURS_NATIVE_PM_CHECK;
            else if (!strcmp(e, "shadow")) m = SPURS_NATIVE_PM_SHADOW;
            else if (strlen(e) < sizeof(s_npm_name)) { m = SPURS_NATIVE_PM_ON; strcpy(s_npm_name, e); }
            else fprintf(stderr, "[spurs-native-pm] PS3RECOMP_SPURS_NATIVE_PM=%s not understood: lifted path\n", e);
            const char* ss = getenv("PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW");
            s_npm_self_shadow = (m == SPURS_NATIVE_PM_ON || m == SPURS_NATIVE_PM_CHECK) && ss && *ss && strcmp(ss, "0");
            if (m != SPURS_NATIVE_PM_OFF)
                fprintf(stderr, "[spurs-native-pm] mode=%s%s%s\n",
                        m == SPURS_NATIVE_PM_CHECK ? "check" : m == SPURS_NATIVE_PM_SHADOW ? "shadow" : "on",
                        s_npm_name[0] ? " implementation=" : "", s_npm_name);
        }
        __atomic_store_n(&s_npm_mode, m, __ATOMIC_RELEASE);
    }
    m = s_npm_mode;
    ReleaseSRWLockExclusive(&s_npm_lock);
    return m;
}

unsigned long spurs_native_pm_consults(void) { return __atomic_load_n(&s_npm_consults, __ATOMIC_RELAXED); }

const struct spurs_native_pm* spurs_native_pm_select(uint32_t wid, uint64_t fingerprint,
                                                     const uint8_t* image, uint32_t size)
{
    return spurs_native_pm_select_instance(wid, fingerprint, image, size, 0, 0);
}

const struct spurs_native_pm* spurs_native_pm_select_instance(uint32_t wid, uint64_t fingerprint,
                                                              const uint8_t* image, uint32_t size,
                                                              uint32_t spurs_ea, uint32_t n_spus)
{
    __atomic_add_fetch(&s_npm_consults, 1ul, __ATOMIC_RELAXED);
    const int mode = spurs_native_pm_mode();
    if (mode != SPURS_NATIVE_PM_ON && mode != SPURS_NATIVE_PM_CHECK && mode != SPURS_NATIVE_PM_SHADOW)
        return NULL;
    spurs_native_pm_register_builtins();
    const struct spurs_native_pm* pm = NULL;
    AcquireSRWLockShared(&s_npm_lock);
    for (unsigned i = 0; i < s_npm_count; i++)
        if (s_npm[i]->fingerprint == fingerprint) { pm = s_npm[i]; break; }
    ReleaseSRWLockShared(&s_npm_lock);
    if (!pm) return NULL;
    if (s_npm_name[0] && strcmp(s_npm_name, pm->name) != 0) {
        fprintf(stderr, "[spurs-native-pm] wid=%u %s registered for fp=0x%016llX, not selected: lifted\n",
                wid, pm->name, (unsigned long long)fingerprint);
        return NULL;
    }
    uint8_t sha[32];
    spurs_native_pm_sha256(image, size, sha);
    if (memcmp(sha, pm->sha256, 32) != 0) {
        fprintf(stderr, "[spurs-native-pm] wid=%u %s: fp=0x%016llX matches but the image SHA-256 differs "
                "from the one it was written for: lifted\n", wid, pm->name, (unsigned long long)fingerprint);
        return NULL;
    }
    if (pm->accept && !pm->accept(spurs_ea, wid, n_spus)) {
        fprintf(stderr, "[spurs-native-pm] wid=%u instance=0x%X spus=%u: %s does not take this workload: lifted\n",
                wid, spurs_ea, n_spus, pm->name);
        return NULL;
    }
    if (mode == SPURS_NATIVE_PM_SHADOW) {
        if (!pm->shadow || wid >= 16) {
            fprintf(stderr, "[spurs-native-pm] wid=%u %s has no shadow comparison: lifted\n", wid, pm->name);
            return NULL;
        }
        __atomic_store_n(&s_npm_shadow[wid], pm, __ATOMIC_RELEASE);
        fprintf(stderr, "[spurs-native-pm] wid=%u runs lifted, shadowed by %s (fp=0x%016llX)\n", wid, pm->name,
                (unsigned long long)fingerprint);
        return NULL;
    }
    if (s_npm_self_shadow && pm->shadow && wid < 16) {
        __atomic_store_n(&s_npm_shadow[wid], pm, __ATOMIC_RELEASE);
        fprintf(stderr, "[spurs-native-pm] wid=%u: native runs also fed to the shadow comparison\n", wid);
    }
    fprintf(stderr, "[spurs-native-pm] wid=%u runs native %s (fp=0x%016llX size=%u)%s\n", wid, pm->name,
            (unsigned long long)fingerprint, size, mode == SPURS_NATIVE_PM_CHECK ? " with self-checks" : "");
    return pm;
}

int spurs_native_pm_self_shadow(void) { return spurs_native_pm_mode() != SPURS_NATIVE_PM_OFF && s_npm_self_shadow; }

const struct spurs_native_pm* spurs_native_pm_shadowed(uint32_t wid)
{
    return wid < 16 ? __atomic_load_n(&s_npm_shadow[wid], __ATOMIC_ACQUIRE) : NULL;
}

void spurs_native_pm_set_payload_hook(spurs_native_pm_payload_fn fn)
{ __atomic_store_n(&s_npm_payload, fn, __ATOMIC_RELEASE); }
spurs_native_pm_payload_fn spurs_native_pm_payload_hook(void)
{ return __atomic_load_n(&s_npm_payload, __ATOMIC_ACQUIRE); }

/* ---- running ------------------------------------------------------------------ */
extern void spu_indirect_branch(spu_context*);
extern void spu_drain_call(spu_context*, uint32_t);

/* The job a native run is in on this thread: where a branch to the job-return
 * slot from a frame deeper than the entry bracket lands (design 5: the job
 * ended; unwind to the entry bracket). NULL outside a job. */
#include <setjmp.h>
#include "spu_jmp.h"
#if defined(_WIN32)
#define SPURS_NPM_SETJMP(env) setjmp(env)
#define SPURS_NPM_LONGJMP(env, v) longjmp(env, v)
#else
#define SPURS_NPM_SETJMP(env) _setjmp(env)
#define SPURS_NPM_LONGJMP(env, v) _longjmp(env, v)
#endif
static SPU_THREAD_LOCAL jmp_buf* t_npm_job_env;

/* Called by the window intercept (spu_channels.c) for a branch to the
 * job-return slot that no drain bracket took as a host return: a job that
 * returned with `bi $r0` from a frame deeper than its entry. Ends the job. */
void spurs_native_pm_job_return_far(spu_context* c)
{
    jmp_buf* env = t_npm_job_env;
    if (!env) return;                       /* not in a job: the caller refuses it */
    extern void spu_interp_attr_unwind(void);
    spu_interp_attr_unwind();
    c->pc = SPURS_NATIVE_PM_JOB_RETURN_LS;
    SPURS_NPM_LONGJMP(*env, 1);
}

int spurs_native_pm_enter_job(spu_context* c, uint32_t entry, uint32_t stack,
                              uint32_t stack_bytes, uint32_t context_ls, uint32_t arg)
{
    spurs_native_pm_payload_fn hook = spurs_native_pm_payload_hook();
    const u128 link = spu_make_preferred_u32(SPURS_NATIVE_PM_JOB_RETURN_LS);
    c->gpr[0] = link;
    c->gpr[1] = spu_make_preferred_u32(stack & SPU_LS_MASK & ~15u);
    c->gpr[1]._u32[1] = stack_bytes;
    c->gpr[3]._u32[0] = c->gpr[3]._u32[1] = c->gpr[3]._u32[2] = c->gpr[3]._u32[3] = context_ls & SPU_LS_MASK;
    c->gpr[4] = spu_make_preferred_u32(arg);
    if (hook && hook(c, entry & SPU_LS_MASK)) return 0;
    const int32_t saved_image = (int32_t)c->image_id;
    const uint32_t depth0 = c->host_depth;
    void* const irq_frame0 = c->irq_frame;
    const uint32_t drain_ret0 = c->drain_ret_pc;
    jmp_buf env;
    jmp_buf* const outer = t_npm_job_env;
    t_npm_job_env = &env;
    if (SPURS_NPM_SETJMP(env) == 0) {
        spu_jmp_no_unwind(env);
        c->pc = entry & SPU_LS_MASK;
        c->host_depth++;
        spu_indirect_branch(c);
        spu_drain_call(c, SPURS_NATIVE_PM_JOB_RETURN_LS);
    } else {
        /* the job returned to the slot from a deeper frame: those frames are gone */
        g_spu_trampoline_fn = 0;
        c->irq_frame = irq_frame0;
        c->drain_ret_pc = drain_ret0;
        c->region_dispatch_valid = 0;
    }
    t_npm_job_env = outer;
    c->host_depth = depth0;
    spu_img_restore(c, saved_image);
    return (c->pc & SPU_LS_MASK) == SPURS_NATIVE_PM_JOB_RETURN_LS ? 0 : -1;
}

static SPU_THREAD_LOCAL const struct spurs_native_pm* t_npm_pm;
static SPU_THREAD_LOCAL const struct spurs_native_pm_run* t_npm_run;
static SPU_THREAD_LOCAL int t_npm_rc;
static void spurs_npm_thunk(spu_context* c)
{
    t_npm_rc = t_npm_pm->run(c, t_npm_run);
    if (t_npm_rc == 0) {
        /* The run ended as an exit to the kernel: the same state a lifted
         * exit-to-kernel leaves for the policy runner's bookkeeping. */
        extern SPU_THREAD_LOCAL volatile unsigned g_spurs_pm_exited;
        g_spurs_pm_exited = 1;
        c->pc = SPURS_PM_EXIT_TO_KERNEL_LS;
        c->status = SPU_STATUS_STOPPED_BY_STOP;
        if (c->pm_observer) c->pm_observer->kernel(c, SPURS_PM_KERNEL_EXIT);
    }
}
/* One native dispatch inside the same halt bracket a lifted run uses (job
 * code may halt). Returns what spu_run_with_halt returns. */
static int spurs_native_pm_invoke(const struct spurs_native_pm* pm, spu_context* c,
                                  const struct spurs_native_pm_run* run)
{
    t_npm_pm = pm; t_npm_run = run; t_npm_rc = -1;
    /* Open the hook sites' gate line (spu_context.h) before the table exists:
     * from here on the resolver tests for the service window in policy runs. */
    if (!__atomic_load_n(&g_spu_dispatch_gates.pm_extras, __ATOMIC_RELAXED))
        __atomic_store_n(&g_spu_dispatch_gates.pm_extras, 1, __ATOMIC_RELEASE);
    c->native_services = pm->services;
    const int halted = spu_run_with_halt(spurs_npm_thunk, c);
    c->native_services = NULL;
    t_npm_pm = NULL; t_npm_run = NULL;
    return halted;
}

/* The native WWS job manager (registered by spurs_native_pm_register_builtins). */
#include "wwsjm/wwsjm_run.inc"
