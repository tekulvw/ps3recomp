#ifndef SPU_NATIVE_SERVICE_H
#define SPU_NATIVE_SERVICE_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
/* A bounded host landing scope around one genuine guest service. Guest
 * restarts resume inside this scope, so an enclosing plain native workunit
 * state remains live; completed service effects are never replayed. No Rust
 * frame/destructor may be active across this call. Returns0 only at the exact
 * return PC, original SP and caller image with RUNNING status,1 for a genuine
 * halt (caller must propagate using spu_halt),-1 for invalid/mismatched return.
 * entry_pc==return_pc performs an interrupt checkpoint only, preserving LR.
 * Admission requires exclusive context execution and no active IRQ frame. */
int spu_native_service_call(spu_context *,uint32_t entry_pc,uint32_t return_pc);
/* Same protected service scope, but caller has already installed the exact
 * source link vector in its actual link register (r0/r4/r6/r78/etc). No GPR is
 * modified before dispatch; real return PC/SP/image checks remain identical. */
int spu_native_service_call_prepared(spu_context *,uint32_t entry_pc,uint32_t return_pc);
/* Plain-C owner lifetime scope, without changing architectural registers.
 * Returns 0 on normal callback return, 1 on halt, 2 on dispatch restart, -1
 * for invalid input. Caller MUST release its ownership state then propagate
 * nonlocal outcomes with spu_halt/spu_restart_dispatch; never replay body.
 * No C++/Rust destructors may be active across the callback. */
int spu_native_owner_call(spu_context *, void (*body)(spu_context *));
#ifdef __cplusplus
}
#endif
#endif
