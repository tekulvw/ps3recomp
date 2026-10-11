/* Extended query/load fixtures do not create memory containers. Keep their
 * runtime dependency explicit; real container behavior is tested separately. */
#include "cellSysmodule.h"

int sys_memory_container_available(u32 container_id, u32* available)
{
    (void)container_id;
    (void)available;
    return 0;
}
