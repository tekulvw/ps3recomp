# Native SPURS policy modules: the seam

A generic, title-independent way to run a workload's SPURS policy module as host
code instead of the lifted policy image. The runtime registers **no**
implementation; with `PS3RECOMP_SPURS_NATIVE_PM` unset nothing here runs and
behaviour is byte-identical to a build without the seam
(`tools/test_spurs_native_pm.py`).

API: `runtime/spu/spurs_native_pm.h`. Implementation:
`runtime/spu/spurs_native_pm.inc` (included by `spurs_policy.c`), the resolver's
window intercept in `spu_channels.c`, selection in `libs/spurs/cellSpurs.c`.

## Switch

`PS3RECOMP_SPURS_NATIVE_PM`, read once when the scheduler starts:

| Value | Effect |
|---|---|
| unset, `0` | Lifted path (default). The registry is never consulted; no context gets a service table. |
| `1` | A workload whose policy image has a registered implementation, with matching fingerprint **and** SHA-256, runs natively. |
| `check` | As `1`; the implementation is told (`run->mode`) to run its self-checks. |
| `shadow` | Lifted path. Reserved for an implementation's shadow comparison; none is wired yet. |
| `<name>` | As `1`, restricted to the implementation registered under that name. |

The decision is made once per workload id, when its policy image is first
resolved (`spurs_resolve_pm`), and holds for the process: a resident context
never alternates between lifted and native runs, and there is no per-run
fallback. Each decision prints one `[spurs-native-pm]` line. An image whose
fingerprint matches but whose SHA-256 differs from the one the implementation
was written for stays lifted, with a line saying so.

## Registering

```c
static const struct spurs_native_services services = { { svc0, svc1 /* ... */ } };
static const struct spurs_native_pm impl = {
    "name", /* fingerprint */ 0x..., /* sha256 */ { ... }, run, &services };
spurs_native_pm_register(&impl);   /* before the workload is first resolved */
```

`run(ctx, args)` performs one dispatch on the slot's resident context (the same
context, LS persistence and worker thread a lifted run uses) and returns 0 when
the run ended as an exit to the kernel. The seam then leaves exactly the state a
lifted exit-to-kernel leaves, so contention, the idle back-off, the FTRACE span
and the recorder see a normal run end. A non-zero return is reported like a
lifted run that did not exit (`-2`). The run may poll the kernel with
`spurs_kernel_select_workload(ctx)` (the same HLE the lifted intercept uses).

## Entering job code and the service window

`spurs_native_pm_enter_job(ctx, entry, stack, context, arg)` consults the payload
hook (`spurs_native_pm_set_payload_hook`, NULL by default), else sets r0 = the
job-return slot, r1, r3, r4 and runs the job in a call bracket that drains until
the job returns there. It returns 0 when the job returned (by host return or by a
computed jump to its link).

Job code calls services through the **native-service window**: twelve 16-byte
slots at LS `[0x900, 0x9C0)`, below the two kernel-service addresses and the
policy image base. A native run hands job code slot addresses (for example in
the context it passes in r3). While its table is installed (`ctx->native_services`,
set by the seam for the run and cleared after), the resolver treats a branch to
slot `i` as a call of `slot[i]`: arguments in r3..r6, result in r3, return to the
r0 link like `bi $r0`. A service never touches r80-r127. The last slot is the job
return address and is handled by the seam. A branch to an empty slot, the middle
of a slot, or the job-return slot outside its drain stops the process (exit 86)
with a message: unsupported behaviour is never skipped.

The window test costs nothing outside policy runs and one load and compare in
the resolver's policy-mode block otherwise; `spu_dispatch_mt.c`'s fast path sends
the whole `[0x900, 0x9E0)` range (window and kernel services, none of it lifted
code) to the full resolver with one compare.

## Not yet covered

- Shadow comparison (`shadow`): an implementation-side hook is a later phase.
- A job that restarts dispatch (a one-way guest stack reset) unwinds to the
  policy runner's halt bracket and abandons the native run's host frames; a
  native implementation must treat that as unsupported until handled.

## Tests

`tools/test_spurs_native_pm.py` (POSIX; `tests/test_spurs_native_pm.c`):

- **Inert when unset.** Three cases run the workload through the real scheduler: a native implementation registered, none registered, and the switch on with nothing registered. All are lifted. The registry is consulted only in the last case, and no context gets a table. Guest memory and bookkeeping are identical across the three, and so are their policy-run recordings, event for event.
- **Identity gate.** Fingerprint and SHA-256 must both match, and `shadow` and a non-matching name stay lifted.
- **Native runs.** They do the same guest work with the same bookkeeping as lifted runs.
- **Window contract.** The result comes back in r3, control returns to r0, and r80-r127 are left as they were.
- **Job returns and hooks.** Jobs return by host return and by computed jump. The payload hook can claim an entry, and a run that does not exit returns -2.
- **Recordings and failure.** Recordings of native runs are complete, and a branch to an empty slot stops the process with exit 86.

`--mutants`: registry consulted while unset, a service clobbering r80, the window
not returning to r0, the SHA gate skipped, the table left installed after a run.
