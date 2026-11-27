/*
 * ps3recomp - guest file-open observer (docs/fs-observer.md)
 *
 * Off until an embedding application installs a callback. Once installed, every guest file open
 * (cellFsOpen and cellFsSdataOpen in runtime/ppu/ppu_fs.cpp, the lv2 sys_fs_open in
 * runtime/syscalls/sys_fs.c, and cellFsOpen in libs/filesystem/cellFs.c) reports one event after
 * the call returns, on the guest thread that made it: the guest path as the title passed it, the
 * guest fd it got, or the error. Nothing is logged or stored here; the application keeps what it
 * needs (for example which level a title is loading) and must return quickly, because the guest
 * thread waits for it.
 *
 * The callback may run on several guest threads at once. Installing or removing it is atomic; a
 * call already in flight may still finish with the previous callback.
 */
#ifndef PS3EMU_FS_HOST_OBSERVER_H
#define PS3EMU_FS_HOST_OBSERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3RECOMP_FS_OBSERVER_VERSION 1

enum ps3recomp_fs_event_kind {
    PS3RECOMP_FS_OPEN = 1 /* a file open: succeeded (fd >= 0) or failed (error) */
};

struct ps3recomp_fs_event {
    uint32_t    version; /* PS3RECOMP_FS_OBSERVER_VERSION */
    uint32_t    kind;    /* enum ps3recomp_fs_event_kind */
    int32_t     fd;      /* the guest fd the open returned; -1 when it failed */
    int32_t     error;   /* 0 on success, else the CELL_FS error code the guest got */
    const char *path;    /* the guest path as passed (NUL-terminated); valid only during the call */
};

typedef void (*ps3recomp_fs_observer_fn)(const struct ps3recomp_fs_event *event);

/* Installs `fn` (NULL removes it) and returns the previous callback. */
ps3recomp_fs_observer_fn ps3recomp_fs_set_observer(ps3recomp_fs_observer_fn fn);

/* For the runtime's open handlers: reports one open to the installed callback, if any. Without
 * one it is a single atomic load. */
void ps3recomp_fs_report_open(const char *path, int32_t fd, int32_t error);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_FS_HOST_OBSERVER_H */
