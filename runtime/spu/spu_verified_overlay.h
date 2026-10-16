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


/* Experimental independent raw-ISA blocks. The generator must use no guest
 * calls, services/IRQ control, native return classification or outside-code
 * facts. Every control transfer publishes its architectural PC to the drain.
 * Every byte of [start,end) is checked at lookup AND entry, without cached trust.
 * Execution requires the actual innermost native call/drain witness and excludes
 * any span containing that caller's continuation PC.
 * Legacy translations retain their whole-image admission. Existing readonly-
 * running-body limitations remain; this does not support concurrent/mid-block SMC.
 * Startup only: the verified image and exact legacy entries must already exist.
 * The immutable verified-image byte storage remains owned by the runtime.
 */
typedef struct spu_verified_leaf_unit {
    uint32_t start, end;
    void (*func)(spu_context*);
} spu_verified_leaf_unit;
int spu_verified_leaf_register(int image_id,const spu_verified_leaf_unit*,unsigned count);
int spu_verified_leaf_enter(const spu_context*,int image_id,uint32_t start,uint32_t end);
/* Use only inside the generated native guest-call bracket after host_depth++.
 * The explicit instruction+4 return PC owns the caller's continuation. With
 * leaf mode off this is exactly the original indirect dispatcher call. The
 * original service/IRQ/ownership intercepts still run in both modes. */
void spu_verified_leaf_indirect_call(spu_context*,uint32_t return_pc);

#ifdef __cplusplus
}
#endif
#endif
