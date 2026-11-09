/*
 * spu_jmp.h -- non-unwinding longjmp targets for the SPU runtime on 64-bit Windows.
 *
 * On 64-bit Windows, longjmp unwinds every host frame between it and its setjmp
 * (RtlUnwindEx: a function-table lookup and a virtual unwind per frame). The SPU
 * runtime longjmps on every halt back to spu_run_with_halt, on every restart and
 * service halt back to the service landing pads, and on every iret back to the
 * drain loop that took the interrupt. In MAG's Training these jumps cost about
 * 5 ms of CPU per frame in RtlUnwindEx/RtlVirtualUnwind2/RtlLookupFunctionEntry.
 *
 * spu_jmp_no_unwind() zeroes the jmp_buf's Frame right after its setjmp returned 0,
 * which makes a later longjmp to that buffer restore the registers directly. The
 * jumps only cross C frames of this runtime and of lifted code, which have no
 * destructors or __finally blocks to run. SPU_LONGJMP_UNWIND=1 keeps the unwinding
 * jump. Other platforms: no change.
 *
 * Adapted from upstream ps3recomp 4609b3f0 ("spu: halt and iret longjmps without
 * unwinding on 64-bit Windows"), which applied it to the halt and iret sites.
 */
#ifndef SPU_JMP_H
#define SPU_JMP_H

#include <setjmp.h>
#include <stdlib.h>

static inline int spu_jmp_unwind_requested(void)
{
    static int s_unwind = -1;
    if (s_unwind < 0) s_unwind = getenv("SPU_LONGJMP_UNWIND") ? 1 : 0;
    return s_unwind;
}

/* Call right after a setjmp on `env` returned 0, in the frame that called setjmp. */
static inline void spu_jmp_no_unwind(void* env)
{
#if defined(_WIN64) && defined(_MSC_VER)
    if (!spu_jmp_unwind_requested()) ((_JUMP_BUFFER*)env)->Frame = 0;
#else
    (void)env;
#endif
}

#endif /* SPU_JMP_H */
