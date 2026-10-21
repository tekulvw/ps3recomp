/* Default-off SPU interpreter time attribution.
 *
 * SPU_INTERP_ATTRIBUTION=<path> (read once) aggregates host wall time,
 * interpreted instruction count and entry count for every interpreted run,
 * keyed by the executing context's image ownership, the entry LSA and why the
 * dispatcher fell back to the interpreter. A background thread replaces <path>
 * atomically about every two seconds. Nothing is logged per event and no guest
 * state is changed. Report rows include a hash and the first 16 code bytes at
 * each entry, so the report is private run evidence. */
#ifndef SPU_INTERP_ATTRIBUTION_H
#define SPU_INTERP_ATTRIBUTION_H

#include <stdint.h>
#include "spu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

enum spu_interp_attr_reason {
    SPU_IATTR_DIRECT = 0,          /* interpreter entered without an indirect-dispatch miss */
    SPU_IATTR_UNLIFTED,            /* no image registered a function at this LSA */
    SPU_IATTR_OTHER_IMAGE,         /* a function exists, but for an image this context does not own */
    SPU_IATTR_VERIFIED_MISMATCH,   /* verified image owns the LSA, but live LS bytes differ */
    SPU_IATTR_VERIFIED_NO_ENTRY,   /* verified image owns the LSA, no lifted entry there */
    SPU_IATTR_VERIFIED_AMBIGUOUS,  /* several verified owners matched */
    SPU_IATTR_RESIDENT_NO_ENTRY,   /* bounded resident code owns the LSA, no lifted entry there */
    SPU_IATTR_COUNT
};

int spu_interp_attr_enabled(void);
/* `frame` is the caller's __builtin_frame_address(0). A begin from a deeper
 * frame while a run is open is a nested run and only adds instructions; a begin
 * from the same or a shallower frame means the open run was unwound (longjmp)
 * and closes it first. */
void spu_interp_attr_begin(const spu_context* ctx, uint32_t entry, int reason, const void* frame);
void spu_interp_attr_add_steps(uint64_t steps);
void spu_interp_attr_end(const void* frame);
/* Close any open run on this thread now: a halt longjmp is leaving it. */
void spu_interp_attr_unwind(void);
/* Write the report now; 0 on success. */
int spu_interp_attr_flush(void);
const char* spu_interp_attr_reason_name(int reason);

/* Implemented beside the verified-image registry (spu_channels.c). */
int spu_interp_attr_classify(const spu_context* ctx, uint32_t lsa);

#ifdef __cplusplus
}
#endif

#endif
