/*
 * ps3recomp - guest file-open observer (include/ps3emu/fs_host_observer.h, docs/fs-observer.md)
 *
 * C++ so that the open handlers in C (sys_fs.c, cellFs.c) and in C++ (ppu_fs.cpp, compiled into
 * the game) share one std::atomic callback.
 */
#include "../../include/ps3emu/fs_host_observer.h"

#include <atomic>

namespace {
std::atomic<ps3recomp_fs_observer_fn> s_observer{nullptr};
}

extern "C" ps3recomp_fs_observer_fn ps3recomp_fs_set_observer(ps3recomp_fs_observer_fn fn)
{
    return s_observer.exchange(fn, std::memory_order_acq_rel);
}

extern "C" void ps3recomp_fs_report_open(const char *path, int32_t fd, int32_t error)
{
    const ps3recomp_fs_observer_fn fn = s_observer.load(std::memory_order_acquire);
    if (!fn) return;
    ps3recomp_fs_event event;
    event.version = PS3RECOMP_FS_OBSERVER_VERSION;
    event.kind = PS3RECOMP_FS_OPEN;
    event.fd = error ? -1 : fd;
    event.error = error;
    event.path = path ? path : "";
    fn(&event);
}
