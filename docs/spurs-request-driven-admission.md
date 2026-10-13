# SPURS1 workload admission

Registering a runnable workload, assigning a nonzero priority, or configuring a
minimum contention does not itself request policy execution. Admission requires a
nonzero ready count, the workload's signal, or a pending flag assigned to that
workload receiver. This eligibility rule is also used by RPCS3's
[SPURS1 workload selection](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpursSpu.cpp#L295-L337).

The native coordinator previously ran policies without a request on recurring
idle-backoff passes. These probes incremented the workload's current contention,
so `cellSpursGetWorkloadInfo` could transiently report an otherwise idle workload
as busy. The coordinator now skips unrequested policies and rechecks readiness,
signal and receiver flag while holding the shared lock line immediately before
consuming a request. A withdrawn request cannot pass that consumption check.
Already admitted work still runs to its genuine policy return; completion and
contention release are unchanged. This change does not revise ready-count
consumption semantics or implement SPURS2 admission.

The scheduler fixture now explicitly requests execution for its manufactured
consumer/producer, capacity, incarnation and lifecycle scenarios. Its continuously
runnable policy requests its next visit before returning. Their existing
concurrency, maximum-contention, BUSY immutability, teardown, resident-context and
fairness assertions remain. The priority fixture also verifies that enabling a
lane without a request leaves it idle, then submits an explicit request.

A new regression proves that registration plus WakeUp leaves a policy unentered
with returned contention zero. Ready-count, signal and receiver-flag requests each
admit the real manufactured policy, report contention one while it is held, and
return to contention zero on its genuine return. The workload deliberately stays
registered and runnable after that return; no unrequested re-entry occurs.

Run `python3 tools/test_spurs_scheduler.py` and
`python3 tools/test_spurs_workload_flag.py` to exercise the scheduler and receiver
flag ABI/consumption/NID checks with manufactured data only.
