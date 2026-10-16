/* Optional pre-mutation lease. A rejected experimental transition must leave
 * memory intact; a successful begin is paired with end after the mutation. */
#ifndef PS3_VM_RELEASE_GUARD_H
#define PS3_VM_RELEASE_GUARD_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {int(*begin)(uint64_t,uint64_t);void(*end)(void);} ps3_vm_release_hooks;
#ifdef _MSC_VER
__declspec(selectany) ps3_vm_release_hooks ps3_vm_release_guard={0};
#else
__attribute__((weak)) ps3_vm_release_hooks ps3_vm_release_guard={0};
#endif
static inline int vm_release_begin(uint64_t ea,uint64_t size){return !ps3_vm_release_guard.begin||ps3_vm_release_guard.begin(ea,size);}
static inline void vm_release_end(void){if(ps3_vm_release_guard.end)ps3_vm_release_guard.end();}
#ifdef __cplusplus
}
#endif
#endif
