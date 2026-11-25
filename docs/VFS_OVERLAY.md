# File overlay over the VFS root (`PS3_VFS_OVERLAY`)

A port sometimes needs the game to see a few of its files changed, for example one settings
archive rewritten for a different server, while every other file stays the original. Without
runtime help the only way is a second view of the whole game tree (copies, or links that archive
tools still count at full size). `PS3_VFS_OVERLAY` lets a port keep just the changed files.

## Behaviour

- **Off by default.** With `PS3_VFS_OVERLAY` unset or empty, path translation is unchanged.
- **Where it applies.** It applies to host paths the runtime's path translators (`sys_fs`, `cellFs`
  and the PPU `cellFs` HLE) resolve under `PS3_VFS_ROOT`, the directory that holds `PS3_GAME`. It
  needs `PS3_VFS_ROOT`; without it, the runtime says so once on stderr and uses no overlay. Paths
  the translators send elsewhere (`PS3_HDD0_ROOT`, `PS3_HDD1_ROOT`, `PS3_DEV_FLASH`,
  `PS3_USRDIR_BASE`) never use the overlay.
- **Layout.** The overlay directory has the root's layout: `<overlay>/PS3_GAME/USRDIR/a/b.bin`
  replaces `<root>/PS3_GAME/USRDIR/a/b.bin`.
- **Only ordinary files.** A path is redirected when the overlay has an ordinary file (or a link to
  one) at the same relative path. Directories in the overlay are never used, so opening, listing
  or checking a directory always uses the root's own directory, and a listing shows the root's
  entries.
- **Whole-file replacement.** A redirected path is the overlay's file for every operation: open,
  read, write, stat, fstat, the per-entry size in a directory listing, rename and delete. The
  root's file is never read or changed through that path. Files that are not in the overlay,
  including new files the game creates, behave as without an overlay.
- **Matching.** Separators may be `/` or `\`, repeated separators and `.` components are ignored,
  and on Windows the root prefix is compared without regard to letter case (the rest of the path
  follows the file system's own rules, since it is looked up there). A path with a `..`
  component is never redirected, so a path cannot leave the overlay directory.
- **Looked up on use.** The overlay's file is looked up each time a path is translated; nothing is
  cached, so a port may create the overlay before boot and needs no restart rule beyond that.
- **Visible.** The first use prints the overlay and root on stderr, and the first redirects name
  the files they redirect. A missing overlay directory or a redirected path too long for the
  caller's buffer is reported and that path keeps the root's file.

## Where it lives

`runtime/syscalls/sys_fs.c`: the three translators already end in the shared
`ps3_vfs_ps3game_fallback`, which applies the disc-layout fallback and then the overlay. The PPU
`cellFsGetDirectoryEntries` passes each entry's path through the same function before reading its
size, so a listing agrees with `stat`.

## Tests

`python3 tools/test_vfs_overlay.py` builds the real `cellFs`, `sys_fs` and PPU file layer with a
synthetic game tree and overlay (no game data) and checks, in separate processes: no change
without the switch; the overridden file through all three translators, PPU `cellFsOpen`/`Read`,
`cellFsStat`, `cellFsFstat` and a directory listing's entry size; every other file, directories
and new files from the root; writes reaching the overlay's copy and never the root's file; `..`,
sibling-prefix and directory-in-overlay cases; and the warnings for a missing root or overlay.
