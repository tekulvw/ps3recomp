#ifndef PS3_ORIGINAL_WORK_OBSERVER_H
#define PS3_ORIGINAL_WORK_OBSERVER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PS3_ORIGINAL_MAX_SOURCES 64u
#define PS3_ORIGINAL_MAX_PROVIDERS 32u
#define PS3_ORIGINAL_MAX_TARGETS 64u
/* Concurrent EXPECTED/ACTIVE workers, including pinned evidence on archive
 * exhaustion. Only counter storage is reused; lifetime identities never are.
 * Storage budgets are independent: 32 MiB compact history, 8 MiB hash index,
 * 16 MiB nonzero archive and about 3.5 MiB active dense counters. Exhaustion
 * remains a sticky fault; real-match acceptance must verify headroom. */
#define PS3_ORIGINAL_MAX_WORKERS 1024u
#define PS3_ORIGINAL_MAX_LIFETIMES 1048576u
#define PS3_ORIGINAL_MAX_ARCHIVED_COUNTS 1048576u
#define PS3_ORIGINAL_ROUTES 7u
enum ps3_original_route {
  PS3_ORIGINAL_PPU,
  PS3_ORIGINAL_LIFTED,
  PS3_ORIGINAL_INTERPRETER,
  PS3_ORIGINAL_MICROSTEP,
  PS3_ORIGINAL_REGION,
  PS3_ORIGINAL_LEAF,
  PS3_ORIGINAL_OUTSIDE
};
enum ps3_original_fault {
  PS3_ORIGINAL_BAD_SOURCE = 1u,
  PS3_ORIGINAL_BAD_ROUTE = 2u,
  PS3_ORIGINAL_MISSING_WORKER = 4u,
  PS3_ORIGINAL_OVERFLOW = 8u,
  PS3_ORIGINAL_LOSS = 16u,
  PS3_ORIGINAL_LATE_ARM = 32u,
  PS3_ORIGINAL_LATE_WORK = 64u,
  PS3_ORIGINAL_BAD_SCOPE = 128u
};
typedef struct ps3_original_identity {
  uint64_t source, generation, provider;
} ps3_original_identity;
typedef struct ps3_original_source {
  uint64_t id, generation, provider_mask;
  uint8_t code_sha256[32];
} ps3_original_source;
typedef struct ps3_original_provider {
  uint64_t id;
  uint32_t route_mask, reserved;
  uint8_t source_sha256[32];
} ps3_original_provider;
typedef struct ps3_original_target {
  uint64_t id, source, low, high;
  uint32_t route_mask, reserved;
} ps3_original_target;
/* The private source census supplies this independent resolver. Success means
 * actual executing code identity/generation and actual generated/runtime
 * provider are qualified by the immutable manifest. An unresolved image MUST
 * fail, including when it cannot be determined whether that code is targeted.
 * No context/LS mutation, guest execution, allocation, I/O, reentry or nonlocal
 * unwind is legal.
 * provider_name is a compiled call-site identity, never a candidate outcome.
 */
typedef int (*ps3_original_resolver)(const void *context, uint64_t pc,
                                     unsigned route, const char *provider_name,
                                     ps3_original_identity *out);
typedef struct ps3_original_config {
  uint32_t struct_size, abi_version, source_count, provider_count, target_count,
      reserved;
  uint64_t covered_from, counter_limit;
  uint8_t manifest_sha256[32];
  const ps3_original_source *sources;
  const ps3_original_provider *providers;
  const ps3_original_target *targets;
  ps3_original_resolver resolve;
} ps3_original_config;
typedef struct ps3_original_report {
  uint32_t struct_size, abi_version, faults, workers;
  uint64_t covered_from, drained_through, lost_records;
  uint8_t manifest_sha256[32];
} ps3_original_report;
typedef struct ps3_original_live_report {
  uint32_t struct_size, abi_version, faults, workers;
  uint64_t covered_from, lost_records;
  uint8_t manifest_sha256[32];
} ps3_original_live_report;
/* One immutable capture per process. Arm before ANY observed worker starts.
 * Enroll each expected worker generation before creating/starting that worker;
 * subsequent workers need explicit enrollment too. No ID/generation reuse.
 * Register/retire run on that worker. Retire only after its actual last guest
 * execution; join/drain orchestration remains the host's responsibility.
 * begin_drain does not stop recording: all late work until worker retirement
 * remains counted. seal succeeds only after every enrolled worker retired;
 * missing workers/unknown routes/lost records can never yield eligible zeros.
 * Negative status: -1 invalid/ineligible, -2 not compiled, -4 not drained.
 */
int ps3_original_arm(const ps3_original_config *);
int ps3_original_enroll(uint64_t worker, uint64_t generation);
int ps3_original_worker_register(uint64_t worker, uint64_t generation);
int ps3_original_worker_retire(void);
int ps3_original_transaction_begin(uint64_t transaction);
int ps3_original_transaction_end(uint64_t transaction);
void ps3_original_step(const void *, uint64_t pc, unsigned route,
                       const char *provider_name);
void ps3_original_lost(uint64_t count);
int ps3_original_begin_drain(uint64_t boundary);
int ps3_original_seal(ps3_original_report *);
/* Each count is an original body-execution witness at a declared PC, not an
 * estimate of CPU share or exact distinct invocation count. A body with many
 * observed instructions can contribute many witnesses. Zero is meaningful
 * only with complete private route/source enrollment, no faults, and actual
 * host join/drain evidence. This API does not manufacture ObserverReceipts. */
int ps3_original_count(uint64_t worker, uint64_t generation, uint64_t target,
                       unsigned route, uint64_t *out);
/* Explicitly diagnostic snapshots, with NO drained-through/final receipt.
 * Single-writer relaxed atomic counters are readable while workers execute.
 * Neither API establishes absence, complete worker enrollment or quiescence. */
int ps3_original_live_status(ps3_original_live_report *);
int ps3_original_live_total(uint64_t target, unsigned route, uint64_t *out);
/* Native factory integration: high-bit IDs are reserved for these helpers.
 * prepare=1 means enrolled; failure never authorizes suppressing a real thread.
 * cancel acknowledges a genuine native creation failure, never live work.
 * The main guest worker uses explicit host enrollment of ID1/generation1. */
int ps3_original_worker_prepare(uint64_t *worker);
int ps3_original_worker_cancel(uint64_t worker);
#if defined(PS3RECOMP_ORIGINAL_OBSERVER) && PS3RECOMP_ORIGINAL_OBSERVER
#define PS3_ORIGINAL_STEP(context, pc, route)                                  \
  ps3_original_step((context), (pc), (route), __FILE__)
#else
/* Compiles away: default builds have no instruction-level branch/counter. */
#define PS3_ORIGINAL_STEP(context, pc, route) ((void)0)
#endif
#ifdef __cplusplus
}
#endif
#endif
