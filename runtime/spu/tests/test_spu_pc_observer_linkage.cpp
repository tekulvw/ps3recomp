/* Mixed C/C++ translation units must see the same gate and event counter. */
#include "../spu_pc_observer.h"
#include <cassert>
extern "C" void spu_pc_observer_linkage_check(spu_pc_observer_state* from_c)
{
    assert(from_c==&g_spu_pc_observer);
    assert(ReadAcquire(&from_c->events)==ReadAcquire(&g_spu_pc_observer.events));
    spu_pc_observe((spu_context*)1,0x30001,0);
}
