#ifndef PS3_NATIVE_HOST_MEMORY_H
#define PS3_NATIVE_HOST_MEMORY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-only output structures: none of these pointers are guest EAs. Owner is
 * a stable native owner identity. Handles include actual registry generations. */
typedef struct sys_memory_native_allocation {
    uint32_t ea, size;
    uint64_t vm_epoch, generation, owner;
} sys_memory_native_allocation;
/* Requires an initialized VM/allocator and installed VM-transition guard.
 * Default allocation uses a real mapper reservation, preserving guest user
 * budget. It shares the actual mapper cursor and runtime-window exclusions.
 * The optional _user profile charges real user-pool quota instead.
 * Both profiles clear their actual owned mapped storage on success.
 * Failure leaves host_result unchanged. Failed map rollback is an explicit
 * release of this same owned handle; it never loses the owner's allocation. */
int32_t sys_memory_native_allocate(uint32_t bytes,uint32_t alignment,uint64_t owner,sys_memory_native_allocation* host_result);
int32_t sys_memory_native_allocate_user(uint32_t bytes,uint32_t alignment,uint64_t owner,sys_memory_native_allocation* host_result);
int32_t sys_memory_native_acquire(const sys_memory_native_allocation* allocation);
int32_t sys_memory_native_unpin(const sys_memory_native_allocation* allocation);
/* Mapping references are changed only while the caller holds a native pin. */
int32_t sys_memory_native_mapping_ref(const sys_memory_native_allocation* allocation,int delta);
/* VM/GCM shutdown must first quiesce users, unmap, then release every owner.
 * Refuses pins or live mappings. Successful release clears the HOST handle. */
int32_t sys_memory_native_release(sys_memory_native_allocation* allocation);
/* Published once by actual allocator initialization before native users.
 * Standalone video-only runtimes have no native allocation ownership to guard. */
typedef int (*sys_memory_native_live_fn)(uint64_t,uint64_t);
#ifdef _MSC_VER
__declspec(selectany) sys_memory_native_live_fn g_sys_memory_native_live=0;
#else
__attribute__((weak)) sys_memory_native_live_fn g_sys_memory_native_live=0;
#endif
/* Lock-free VM-transition guard. Does not acquire the allocator lock. */
int sys_memory_native_range_live(uint64_t ea,uint64_t bytes);
#ifdef __cplusplus
}
#endif
#endif
