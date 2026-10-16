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
#ifdef __cplusplus
}
#endif
#endif
