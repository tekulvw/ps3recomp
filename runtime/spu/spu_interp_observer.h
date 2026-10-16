/* Opt-in interpreter-entry observation, outside the instruction loop. */
#ifndef SPU_INTERP_OBSERVER_H
#define SPU_INTERP_OBSERVER_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Runs synchronously on the context's owning thread, once per interpreter
 * invocation after its normal PC/status initialization. start_lsa and stop_lsa
 * are the caller's original arguments; ctx->pc contains the masked start PC.
 *
 * The callback may copy bounded scalar metadata into application-owned storage.
 * It must not mutate the context or LS, retain/dereference ctx after returning,
 * invoke guest execution, install another callback recursively, or nonlocally
 * unwind. Invocation counts are not CPU shares. Calls already in progress when
 * an application arms its observer are not observed until another invocation.
 * No exit callback is promised: genuine guest services can unwind execution.
 */
typedef void (*spu_interp_entry_observer_fn)(const spu_context* ctx,
                                          uint32_t start_lsa,
                                          uint32_t stop_lsa);
/* Default NULL. Prefer installation before workers start. Publication is
 * atomic, but replacement/uninstallation does not wait for an in-flight
 * callback; its code and backing state must remain valid until workers quiesce.
 * The SDK retains only this function pointer, never a context pointer. */
void spu_interp_entry_observer_set(spu_interp_entry_observer_fn observer);
#ifdef __cplusplus
}
#endif
#endif
