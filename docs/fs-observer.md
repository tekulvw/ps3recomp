# Guest file-open observer

`include/ps3emu/fs_host_observer.h` lets an embedding application see which files the guest opens,
without logging them. It exists so an application can follow what a title is loading (for example
the level archive of a map, which marks a loading screen and names the map) next to its own frame
timing.

## Behaviour

- **Off by default.** No callback is installed at start. Each open handler then does one atomic load
  and opens the file exactly as before.
- `ps3recomp_fs_set_observer(fn)` installs a callback, `ps3recomp_fs_set_observer(NULL)` removes it;
  each returns the previous one.
- Every guest open is reported **after it returns**, on the guest thread that made it, as one
  `PS3RECOMP_FS_OPEN` event:

  | Open handler | File |
  | --- | --- |
  | `cellFsOpen`, `cellFsSdataOpen` | `runtime/ppu/ppu_fs.cpp` (the HLE handlers a lifted title calls) |
  | `sys_fs_open` | `runtime/syscalls/sys_fs.c` (the lv2 syscall) |
  | `cellFsOpen` | `libs/filesystem/cellFs.c` |

- Each event carries the guest path exactly as the title passed it (before any host translation;
  valid only during the call), the guest fd the open returned, and the error: 0 on success, else the
  `CELL_FS` code the guest got, with `fd` -1. A `cellFsSdataOpen` of a container that cannot be
  decrypted answers success without a handle, and is reported with error 0 and fd -1.
- The callback may run on several guest threads at once and the guest thread waits for it, so it
  must be quick and thread-safe. It must not open guest files itself.
- `ps3recomp_fs_report_open()` is the handlers' side of the hook (`runtime/syscalls/fs_host_observer.cpp`);
  an application does not call it.

## Tests

`runtime/ppu/tests/test_fs_aio.cpp` (`tools/test_ppu_fs_aio.py`) installs a recording callback and
checks that nothing is reported without it, that a failed open is reported with its path and
`CELL_FS_ENOENT`, that a successful one is reported with its path and guest fd, and that removing the
callback returns it.
