/* spu_jmp_no_unwind: a longjmp to a buffer it prepared lands on its setjmp with the
 * jump's value, from several C frames down, many times over; on 64-bit Windows the
 * buffer's Frame is cleared (no SEH unwind) unless SPU_LONGJMP_UNWIND is set. */
#include "spu_jmp.h"
#include <stdio.h>
#include <stdlib.h>

static jmp_buf s_env;
static volatile int s_depth_reached;

static void dive(int depth, int value)
{
    volatile char pad[64];
    pad[depth & 63] = (char)depth;
    if (depth == 0) {
        s_depth_reached = 1;
        longjmp(s_env, value);
    }
    dive(depth - 1, value + (int)pad[depth & 63] * 0);
}

int main(void)
{
    int fails = 0;
    volatile int landed = 0;
    for (volatile int i = 1; i <= 100000; i++) {
        s_depth_reached = 0;
        int r = setjmp(s_env);
        if (r == 0) {
            spu_jmp_no_unwind(s_env);
#if defined(_WIN64) && defined(_MSC_VER)
            const unsigned long long frame = ((_JUMP_BUFFER*)s_env)->Frame;
            if (spu_jmp_unwind_requested() ? frame == 0 : frame != 0) {
                fprintf(stderr, "Frame %llx with SPU_LONGJMP_UNWIND=%d\n", frame, spu_jmp_unwind_requested());
                fails++;
                break;
            }
#endif
            dive(8, i);
            fprintf(stderr, "dive returned without jumping\n");
            fails++;
            break;
        }
        if (r != i || !s_depth_reached) { fprintf(stderr, "landed with %d, expected %d\n", r, i); fails++; break; }
        landed++;
    }
    if (landed != 100000) fails++;
    printf("test_spu_jmp_no_unwind: %s (%d jumps, unwinding %s)\n", fails ? "FAIL" : "ok", (int)landed,
#if defined(_WIN64) && defined(_MSC_VER)
           spu_jmp_unwind_requested() ? "on" : "off"
#else
           "n/a"
#endif
    );
    return fails ? 1 : 0;
}
