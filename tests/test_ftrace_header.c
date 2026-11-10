/* PS3_FTRACE header: base_ns names the clock reading span times are relative
 * to, so a trace aligns with samples taken on the same clock. Synthetic spans. */
#include "ps3emu/ps3_ftrace.h"
#include <assert.h>
int main(void)
{
    const uint64_t before = ps3_ft_now();
    assert(PS3_FT_ON());
    const uint64_t t0 = ps3_ft_now();
    ps3_ft_span(PS3_FT_SPU_BUSY, 1, 2, 3, t0, t0 + 1000000);
    const uint64_t after = ps3_ft_now();
    ps3_ft_dump();
    printf("before_ns=%llu after_ns=%llu span_t0_ns=%llu\n", (unsigned long long)before,
           (unsigned long long)after, (unsigned long long)t0);
    return 0;
}
