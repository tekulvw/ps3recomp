# Native macOS single-window text input

Retail MAG opens `cellOskDialogLoadAsync` directly, with no `cellOskDialogInit` call. The old stub rejected that request with 0x8002B301 and immediately unregistered the game's keyboard callback. The native path accepts valid guest parameters, opens an AppKit sheet on the existing game window, and waits for the user to press OK or Cancel.

The game receives LOADED (0x502), FINISHED (0x503), and UNLOADED (0x504) through its usual `cellSysutilCheckCallback` calls. The UI thread holds copied host strings and a generation token. It publishes to a locked mailbox; only the guest poll queues system events. Confirmed text is UTF-16 big-endian in guest memory. Password panels use a secure field; no prompt, input, or password is logged. Input limits and result capacity are bounded, including the terminator, and truncation avoids splitting surrogate pairs. The PS3 result's `numCharsResultString` specifies maximum content units; its buffer needs space for that content plus a terminating unit. The capacity field is left unchanged, matching RPCS3's single-window handling.

Abort completes with the abort result and invalidates an unshown/open request. Late native notifications cannot replace a newer result. An unavailable/closed native host reports an error rather than returning invented text. NonmacOS hosts retain the prior legacy stub. Native continuous/separate-window modes and PS3 language-panel rendering are outside this change; normal MAG keyboard calls use the supported single-window path.

`tools/test_native_osk.py` exercises real guest memory and real sysutil dispatch with a simulated host boundary, including a background-thread completion, duplicate load, no completion before user action, stale tokens, Unicode truncation, cancel, abort, invalid parameters, and guest-buffer canaries. On macOS it also runs the real AppKit sheet: Unicode OK, secure-field Cancel, and pre-open Abort, checking the deferred guest callbacks.

Reference semantics: [RPCS3 cellOskDialog.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellOskDialog.cpp). Original MAG retail callback OPD 0x01ADEC60 points to 0x00122E28, which recognizes the same three lifecycle statuses; its FINISHED path requests UnloadAsync, and UNLOADED invokes the game's supplied callback.

## Opt-in native confirmation for unattended tests

Set `OSK_NATIVE_CONTROL_FILE` to a fresh absolute path for each process before
launching it. The path is cached once; unset or empty means no control-file IO
and the normal UI behavior. Non-native/headless hosts cannot use this control.
Only a native dialog that has actually opened emits this readiness line:

```text
[cellOskDialog] native control ready token=1
```

Read the current token from that process's log, then publish one command using:

```sh
python3 tools/native_osk_confirm.py /absolute/per-run/osk-control 1
```

The helper atomically replaces the file with mode 0600 and the ASCII command
`confirm <token>\n`. The token must be a positive uint64. The host polls this
mailbox in its normal OSK/sysutil progress path, reads at most 64 bytes, and
consumes regular-file commands whether they are valid, malformed, or stale.
Publish only after the readiness line, use one writer, and let the command be
consumed before publishing again. Commands present before UI readiness are
discarded; they cannot confirm a later dialog. Tokens identify a generation
within one process, so use a new path for each run rather than carrying a file
or token across process launches.

A matching open generation can queue one confirmation. AppKit rechecks the
session, native window, and lifecycle on its main queue and performs the
alert's actual OK button action. The existing editable field, result, sheet
completion handler, and deferred guest event delivery remain responsible for
completion. Abort or parent-window closure can invalidate a queued action.
The control does not edit text, invent a guest result, log field contents, or
simulate guest authentication. The helper's published receipt acknowledges
only writing the command; observe the ordinary FINISHED/UNLOADED events for
completion.

`python3 tools/test_native_osk.py` covers opt-out file preservation, malformed
and oversized commands, binary payload rejection, open races, token mismatch,
one-shot handling, abort, and stale later generations. On macOS it additionally
confirms a real NSAlert through the command file and checks its Unicode result,
queued abort, and subsequent-sheet protection. This real UI fixture needs a
macOS graphical session and host Metal access. `--mailbox-only` runs just the
portable host-boundary tests; `python3 tools/test_native_osk_control.py` checks
the atomic command helper and invalid-token rejection.
