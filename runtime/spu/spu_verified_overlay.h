/* Opt-in fixed-placement, readonly-text lifted overlay verification. */
#ifndef SPU_VERIFIED_OVERLAY_H
#define SPU_VERIFIED_OVERLAY_H
#include "spu_context.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct spu_verified_code_segment {
    uint32_t lsa;
    uint32_t size;
    const uint8_t* bytes;
} spu_verified_code_segment;

/* Startup only, before SPU threads start. Copies immutable expected bytes.
 * Up to16 images,8 nonoverlapping aligned executable segments/image,4MiB total.
 * Registers no functions and does not load or modify guest local store.
 * Return1 on success,0 on invalid/duplicate/allocation/budget failure.
 * Compiled functions must use the lifter's --verified-code-image guard.
 */
int spu_verified_overlay_register(int policy_image_id, int image_id,
    const spu_verified_code_segment* segments, unsigned count);

/* Generated function-entry gate. Does not mutate guest/context state.
 * Rejecting callers must defer to indirect dispatch through the normal drain.
 * Verifies transfer entries only; mid-function self-modification and concurrent
 * external LS writes during a function are outside this readonly-text contract.
 */
int spu_verified_code_enter(const spu_context* ctx, int image_id, uint32_t entry);

/* Opt-in generated direct-transfer gate. Verifies every instruction byte in
 * the complete generated function [entry,end) at each entry, without caching
 * trust. The interval must fit within one registered executable segment and
 * begin at an exact registered function. Other functions validate their own
 * spans when entered. Indirect lookup retains full-image admission/ambiguity
 * checks. Callers must pass the full interval represented by the lifted body;
 * this is not a whole-image identity assertion or mid-function SMC support. */
int spu_verified_code_enter_span(const spu_context* ctx, int image_id,
    uint32_t entry, uint32_t end);

#ifdef __cplusplus
}
#endif
#endif
