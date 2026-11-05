/* Optional guest write-watch hook (runtime/memory/vm_watch.h). A protection
 * change the runtime makes itself (vm_commit, vm_protect) takes the affected
 * pages out of the write watch first, so the watch never restores or keeps a
 * protection the runtime did not ask for. Inert until the watch is enabled. */
#ifndef PS3_VM_WATCH_HOOKS_H
#define PS3_VM_WATCH_HOOKS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    /* [addr, addr+size) is about to get protection `writable` (nonzero:
     * read-write) from the runtime. */
    void (*forget)(uint32_t addr, uint32_t size, int writable);
    /* The same for a host range (ignored outside guest memory): host-level
     * protection or commit calls (VirtualAlloc/VirtualProtect/VirtualFree and
     * their POSIX equivalents) on guest pages. */
    void (*forget_host)(const void* host, size_t size, int writable);
} ps3_vm_watch_hooks;
#ifdef _MSC_VER
__declspec(selectany) ps3_vm_watch_hooks ps3_vm_watch_hook = {0};
#else
__attribute__((weak)) ps3_vm_watch_hooks ps3_vm_watch_hook = {0};
#endif
static inline void vm_watch_forget(uint32_t addr, uint32_t size, int writable)
{
    if (ps3_vm_watch_hook.forget) ps3_vm_watch_hook.forget(addr, size, writable);
}
static inline void vm_watch_forget_host(const void* host, size_t size, int writable)
{
    if (ps3_vm_watch_hook.forget_host) ps3_vm_watch_hook.forget_host(host, size, writable);
}
#ifdef __cplusplus
}
#endif
#endif
