/* Native SPURS policy modules: a generic, title-independent seam.
 *
 * A native policy module is host code that performs a workload's policy-module
 * runs in place of a lifted policy image. It is registered under the image's
 * runtime fingerprint (the key the lifted resolution uses) plus the SHA-256 of
 * the exact image bytes it was written for, and is used only when
 * PS3RECOMP_SPURS_NATIVE_PM selects it:
 *
 *   unset or 0   the lifted path (default). The registry is never consulted,
 *                no context gets a service table, nothing below runs.
 *   1            every workload whose policy image has a registered native
 *                implementation (fingerprint and SHA-256 both match) runs it.
 *   check        as 1, and the implementation is told to run its self-checks.
 *   shadow       the lifted path; a workload an implementation would take
 *                has its lifted runs' policy-run events (the recorder's
 *                format, spurs_pm_record.inc) handed to the implementation's
 *                shadow hook, which compares them with its own decisions.
 *   <name>       as 1, restricted to the implementation of that name.
 *
 * The choice is made once per workload id, when its policy image is first
 * resolved, and holds for the process: one context never alternates between
 * a lifted and a native run. There is no per-run fallback.
 *
 * Job code a native run enters calls back into it through the native-service
 * window: 16-byte LS slots in [SPURS_NATIVE_PM_WINDOW_LS, _END) below the two
 * kernel-service addresses. While a native run has installed its service
 * table (ctx->native_services), the resolver treats a branch to a slot as a
 * call of that slot's host function: arguments in r3..r6, result in r3, return
 * to the r0 link; the function never touches r80-r127. The last slot is the
 * job-return address a native run hands job code as its link. A branch into
 * the window with no table entry is unsupported and stops the process. */
#ifndef SPURS_NATIVE_PM_H
#define SPURS_NATIVE_PM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct spu_context;

#define SPURS_NATIVE_PM_WINDOW_LS      0x900u
#define SPURS_NATIVE_PM_WINDOW_END     0x9C0u  /* == SPURS_PM_EXIT_TO_KERNEL_LS */
#define SPURS_NATIVE_PM_SLOT_BYTES     16u
#define SPURS_NATIVE_PM_SLOTS          12u
#define SPURS_NATIVE_PM_JOB_RETURN_LS  (SPURS_NATIVE_PM_WINDOW_END - SPURS_NATIVE_PM_SLOT_BYTES)
#define SPURS_NATIVE_PM_SLOT_LS(i)     (SPURS_NATIVE_PM_WINDOW_LS + (uint32_t)(i) * SPURS_NATIVE_PM_SLOT_BYTES)
/* The process exit status of a loud refusal (an unsupported window branch). */
#define SPURS_NATIVE_PM_FATAL_EXIT     86

enum spurs_native_pm_mode {
    SPURS_NATIVE_PM_OFF = 0,
    SPURS_NATIVE_PM_ON = 1,
    SPURS_NATIVE_PM_CHECK = 2,
    SPURS_NATIVE_PM_SHADOW = 3
};

/* What a service returns to the window intercept. */
enum {
    SPURS_NATIVE_SERVICE_RETURN = 0,  /* return to the r0 link (like `bi $r0`)          */
    SPURS_NATIVE_SERVICE_STAY = 1     /* leave ctx->pc as the service set it            */
};
typedef int (*spurs_native_service_fn)(struct spu_context*);

/* The table a native run installs in ctx->native_services for its duration.
 * Slot i is LS address SPURS_NATIVE_PM_SLOT_LS(i); the job-return slot
 * (SPURS_NATIVE_PM_SLOTS - 1) is handled by the seam and must stay NULL. */
struct spurs_native_services {
    spurs_native_service_fn slot[SPURS_NATIVE_PM_SLOTS];
};

/* Arguments of one dispatch, as the kernel hands them to a policy module. */
struct spurs_native_pm_run {
    uint32_t spurs_ea, wid, spu_num;
    uint64_t wkl_data;          /* the workload's u64 argument                     */
    uint32_t poll_status;       /* the poll status at entry (r5 word 0)           */
    int image_id;               /* the lifted image id (for job code resolution)  */
    const uint8_t* pm_host;     /* the policy image bytes, as fingerprinted       */
    uint32_t pm_size;
    int fresh;                  /* the context was (re)initialised for this run   */
    int mode;                   /* enum spurs_native_pm_mode in force              */
};

struct spurs_native_pm {
    const char* name;
    uint64_t fingerprint;       /* spu_workload_fingerprint(image)                 */
    uint8_t sha256[32];         /* SHA-256 of the same image bytes                 */
    /* One dispatch on the slot's resident context. Returns 0 when the run ended
     * as an exit to the kernel; anything else is reported like a lifted run
     * that did not exit. May call spurs_kernel_select_workload(). */
    int (*run)(struct spu_context*, const struct spurs_native_pm_run*);
    /* Installed in ctx->native_services for the run; may be NULL. */
    const struct spurs_native_services* services;
    /* Optional: whether this implementation takes workload `wid` of the SPURS
     * instance at `spurs_ea` with `n_spus` SPUs (0 = unknown). Asked once, at
     * selection, after the identity gate; a refusal keeps the lifted path. */
    int (*accept)(uint32_t spurs_ea, uint32_t wid, uint32_t n_spus);
    /* Optional: one policy-run event of a lifted run of a shadowed workload,
     * as {kind, payload} in the recorder's file format (tools/spurs_pm_record.py),
     * on the worker thread running it. Used only in shadow mode. */
    void (*shadow)(struct spu_context*, uint32_t kind, const uint8_t* payload, uint32_t n);
};

/* Registration (any time before the workload is first resolved). Returns 0, or
 * -1 for a full registry, a duplicate fingerprint, or a malformed descriptor. */
int spurs_native_pm_register(const struct spurs_native_pm*);
void spurs_native_pm_unregister_all(void);   /* fixtures */

/* PS3RECOMP_SPURS_NATIVE_PM, parsed once: enum spurs_native_pm_mode. */
int spurs_native_pm_mode(void);

/* The native implementation for a policy image, or NULL (lifted). Consults the
 * registry only when the mode is ON or CHECK; prints one line per decision. */
const struct spurs_native_pm* spurs_native_pm_select(uint32_t wid, uint64_t fingerprint,
                                                     const uint8_t* image, uint32_t size);
/* The same, for workload `wid` of the instance at `spurs_ea` with `n_spus`
 * SPUs (passed to the implementation's accept hook). */
const struct spurs_native_pm* spurs_native_pm_select_instance(uint32_t wid, uint64_t fingerprint,
                                                              const uint8_t* image, uint32_t size,
                                                              uint32_t spurs_ea, uint32_t n_spus);
/* PS3RECOMP_SPURS_NATIVE_PM_SELF_SHADOW=1 with the switch on: native runs' events
 * also go to their implementation's shadow comparison (fixtures, diagnostics). */
int spurs_native_pm_self_shadow(void);
/* The implementation shadowing workload `wid` (shadow mode), or NULL. */
const struct spurs_native_pm* spurs_native_pm_shadowed(uint32_t wid);
/* Implementations built into the runtime (the WWS job manager) are registered
 * the first time a selection is made with the switch on; never while unset. */
void spurs_native_pm_register_builtins(void);
/* The native WWS job manager's descriptor (runtime/spu/wwsjm/wwsjm_run.inc). */
const struct spurs_native_pm* spurs_wwsjm_native(void);
/* Its counters, one [wwsjm] line on stderr (also printed at exit once registered). */
void spurs_wwsjm_print_stats(void);
/* The shadow comparison's summary, one [wwsjm-shadow] line on stderr. */
void spurs_wwsjm_shadow_print(void);
/* How many times the registry has been consulted (inertness checks). */
unsigned long spurs_native_pm_consults(void);

/* Payload hook: may run a job itself in place of its code. Called by
 * spurs_native_pm_enter_job with the job's entry; returns nonzero when it ran
 * the job (it then returned as job code returns). NULL by default. */
typedef int (*spurs_native_pm_payload_fn)(struct spu_context*, uint32_t entry);
void spurs_native_pm_set_payload_hook(spurs_native_pm_payload_fn);
spurs_native_pm_payload_fn spurs_native_pm_payload_hook(void);

/* Enter job code at `entry` from a native run: the payload hook first; else
 * r0 = the job-return slot, r1 = {stack top, stack bytes available, 0, 0}
 * (the SPU ABI's stack pointer and the word its stack check tests), r3 =
 * context (in all four words), r4 = arg (r2, r5..r79 are the caller's), and a
 * call bracket that drains until the job returns to that slot. Returns 0 when
 * the job returned there (or the hook ran it), -1 if it came back elsewhere. */
int spurs_native_pm_enter_job(struct spu_context*, uint32_t entry, uint32_t stack,
                              uint32_t stack_bytes, uint32_t context_ls, uint32_t arg);

/* SHA-256 (FIPS 180-4) of a byte range. */
void spurs_native_pm_sha256(const uint8_t* data, size_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
