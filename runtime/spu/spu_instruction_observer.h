/* Optional bounded instruction witnesses, independent of entry tracing. */
#ifndef SPU_INSTRUCTION_OBSERVER_H
#define SPU_INSTRUCTION_OBSERVER_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif
#define SPU_INSTRUCTION_OBSERVER_CADENCE 4096u
/* Called synchronously on the owning worker immediately before an interpreted
 * instruction executes, after stop/lifted-rejoin checks. pc and opcode describe
 * that instruction, not a compiled caller checkpoint. ordinal is a per-host-
 * thread count of interpreted instructions while an observer is installed.
 * It is not a count of guest cycles or CPU time. Fixed cadence can alias loops.
 *
 * The callback must not modify context/LS, retain context pointers, call guest
 * code, or unwind. Copy only bounded metadata; preserve backing storage until
 * workers quiesce. The SDK preserves errno around the callback.
 */
typedef void (*spu_instruction_observer_fn)(const spu_context* ctx,
    uint32_t pc, uint32_t opcode, uint64_t ordinal);
/* Default NULL. Install BEFORE workers start. Each interpreter invocation
 * snapshots this pointer once; changing it does not affect an invocation already
 * running and uninstall is not a lifetime barrier. Arm/disarm bounded capture
 * in the application callback, without reinstalling this pointer. No context
 * pointer or per-context state is retained by the SDK. */
void spu_instruction_observer_set(spu_instruction_observer_fn observer);
#ifdef __cplusplus
}
#endif
#endif
