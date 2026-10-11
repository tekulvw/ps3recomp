# SPURS workload flag receiver and native policy delivery

This change is stacked on `b5ee097a049614a335100568811cf3c2aba886de`
(`fix/spurs-workload-flag`), which supplies the correctly initialized guest flag
object and getter. It implements NID `A73BF47E`, `_cellSpursWorkloadFlagReceiver`,
and the receiver-specific flag path through the existing SPURS1 native policy
scheduler. It does not supply job results or render completion.

## Firmware contract

The reference is [RPCS3's receiver API](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.cpp#L2866):
null pointer, 128-byte alignment, workload-number limit, enabled bit, then exception
state are checked. Registering when any owner already exists returns policy-module
`BUSY`; unregistering a different owner returns `PERM`. Every nonzero `is_set`
means register. Success resets the BE32 scalar at `spurs+0x6C` to `FFFFFFFF`
and changes the receiver byte at `+0x77`; failure preserves both.

The implementation adds widened host-backing bounds checks. The check includes
`exception` at `+0xD6C`. SPURS2 mode is rejected with `NOSYS` and an explicit
unsupported diagnostic after ordinary argument/state validation, since the native
policy scheduler only covers sixteen workloads. No receiver is installed on that
unsupported path.

Receiver ownership and scalar reset share the SPU/PPU lock-line lock. The receiver
call uses existing `spu_coh_reserve(NULL, ea)` to mark the shared line coherent
without manufacturing an SPU context. Stores invalidate SPU reservations and the
PPU reservations covering the modified words. A pair of competing setters cannot
both succeed. Removing the owning workload clears ownership and resets the flag;
removing another workload preserves the pending notification. Removal also clears
the guest enabled/state fields so a removed workload cannot register again.

## Notification delivery

[RPCS3's kernel selection](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp#L293)
recognizes a zero scalar only for the registered receiver, reports poll-status
`FLAG=4`, and resets the scalar upon selection. Its return value packs workload ID
and status into the preferred doubleword.

The native poller now observes a pending flag as a kick, bypassing idle backoff
without consuming the notification. Actual policy entry atomically consumes the
flag and supplies `r5`'s preferred word with status 4. A running policy's
select-workload call returns the current workload in `r3` word 0 and the status in
word 1, consuming only that workload's flag. Later polls do not repeat a consumed
notification. Another workload's entry or poll leaves it pending.

The runtime retains its existing non-preemptive policy model: this change does
not implement hardware priority arbitration, SPURS2 scheduling, taskset flag
receive services, or correct the separate ready-count/signal delivery paths.
Those limitations are not evidence of graphics completion. The flag's writer
must still perform a real guest-memory write; no notification is synthesized by
registration. Existing policy-lookup failures and unimplemented guest code remain
visible.

## Regression

`python3 tools/test_spurs_flag_receiver.py` compiles the real initializer/API,
coherence layer, `spu_run_policy_module`, and `spu_indirect_branch`. A tiny native
policy callback asserts the real entry and poll ABI, then takes the real
exit-to-kernel path. Only background kernel-thread creation is suppressed so the
fixture can dispatch deterministically. No game assets are used.

The fixture checks guest layout, all ordinary API errors, ownership conflicts,
concurrent setters, supported-versus-unsupported workload mode, memory bounds,
coherence reservation loss, nondestructive pending observation, inactive/exception
states, unrelated workload isolation, entry notification, packed polling, and
exactly-once consumption. It also runs the real HLE generator and verifies the
new NID registration. The initial failing run caught missing registration; the
API-only intermediate run then caught the old poll return selecting workload zero
for a workload-three caller.
