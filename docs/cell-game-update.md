# Game update check (cellGameUpdate)

A title can ask the platform whether a newer patch of itself is published. The
calls live in the cellNetCtl library. This runtime has no patch server, so it
always answers "no update available" and finishes cleanly. Every call prints a
`[cellGameUpdate] ...` line, so the check is visible in a log.

## Contract (public API)

- `cellGameUpdateInit()` prepares the facility. A second Init without a Term
  between fails with `ALREADY_INITIALIZED` (0x8002cc02).
- `cellGameUpdateTerm()` releases it. Before Init it fails with
  `NOT_INITIALIZED` (0x8002cc01).
- `cellGameUpdateCheckStartAsync(param, cb, userdata)` starts a check that
  normally shows a progress dialog. `param` describes the request (its first
  word is the structure size). `cellGameUpdateCheckStartWithoutDialogAsync(cb,
  userdata)` starts the same check with no dialog.
- `cellGameUpdateCheckFinishAsync(cb, userdata)` closes the check after its
  result has been seen.
- `cellGameUpdateCheckAbort()` asks a running check to stop.
- The callback is `void cb(s32 status, s32 error_code, void *userdata)`
  (r3, r4, r5). It runs later, from the title's own
  `cellSysutilCheckCallback`, never from inside the call that started it.
- Result statuses: 0 no update, 1 update found, 2 maintenance, 3 error,
  4 cancelled, 5 finished, 6 aborted, 7 system update needed.
- Other errors: `INVALID_ADDR` (0x8002cc03) for a missing parameter block or
  callback, `BUSY` (0x8002cc07) while an earlier answer is still waiting.

## Behaviour in this runtime

1. Init and Term keep an initialised flag. Term also drops any answer that has
   not yet been delivered, so nothing calls into the title after it has shut
   the facility down.
2. Start (either form) records one pending answer: status 0 (no update),
   error 0. Finish records one pending answer: status 5 (finished), error 0.
   Each call returns `CELL_OK` at once; the answer is not delivered inside it.
3. A Start while an earlier Start answer is undelivered returns `BUSY`; so
   does a Finish while an earlier Finish answer is undelivered. A Finish may
   be issued before the Start answer arrives; it is then delivered after it.
4. Abort returns `CELL_OK`. If a Start answer is still pending, it becomes
   status 6 (aborted), which is still delivered, so a title waiting for its
   callback is not left waiting. With nothing pending, Abort changes nothing.
5. On each `cellSysutilCheckCallback`, the answers pending when the poll
   begins are taken off the record and each delivered once, Start before
   Finish, to the recorded callback with the recorded userdata. The record is
   cleared before the guest code runs, so a nested poll cannot deliver it twice,
   and a step the callback starts (for example Finish) is delivered on a later
   poll. With no guest caller installed the answers wait for a poll that has
   one.
6. The `*Ex` forms (a result structure instead of status/error) are not
   provided; an import of one stays unresolved and halts visibly.
