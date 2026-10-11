# Guest-written SPURS task signal wake

The native WAIT_SIGNAL implementation previously waited on a host condition
variable that only `spu_taskset_signal_task` notified. Lifted PPU firmware can
perform the same operation by writing the real guest taskset SIGNALLED bitset
at offset 0x40 with `stwcx.`. That store went through SPU coherency, but coherency
only woke SPU channel waiters. WAIT_SIGNAL discovered the pending bit at its
one-second timeout instead.

The reproducer uses the real WAIT_SIGNAL, PPU `lwarx`/`stwcx` primitives, and
coherency dispatcher. Before this fix it fails because a genuine guest signal
does not resume the waiter within 250 ms. Afterward it passes without shortening
the one-second fallback or enabling a synthetic drain option.

## Synchronization

Scoped, caller-owned write watches register a 128-byte guest line with the
existing coherency dispatcher. Registration marks the line for coherent stores.
Both ordinary and MFC self-excluding notifications invoke matching watches,
even when there is no remaining SPU reservation. A watch callback can wake a
host waiter; it must not change guest state, re-enter coherency, or modify the
watch list. Add, callback, and removal are serialized by the lock-line lock, so
removal completes before the caller's stack object goes out of scope.

WAIT_SIGNAL registers the line, tests the guest bit, and consumes only that bit
under lock-line. When the bit is absent it acquires the signal CV mutex before
releasing lock-line and entering the atomic CV wait. On wake it releases the CV
mutex before acquiring lock-line again. Notifiers therefore always use the same
lock order: lock-line, then signal CV. A write between predicate check and sleep
cannot be lost. Host SendSignal also performs its bit update under lock-line;
plain read/modify/write under only the CV mutex would race lifted firmware CAS.
Changes notify SPU reservations and break PPU reservations on the modified word.

The existing store fast path samples whether a line is armed before locking.
A store already past an unarmed sample when the *first* watch is registered can
still miss that first notification. The existing timed predicate recheck remains
the fallback for this inherited first-publication race; this patch does not
change the global bitmap or guest-store publication model. Once the line is
armed, the handshake above covers check-to-sleep writes. A pending signal before
registration is consumed immediately.

## Validation

Run `python3 tools/test_spurs_task_signal.py`. The fixture exercises real guest
CAS, coherent stores, MFC PUT, host SendSignal, a signal latched before waiting,
zero and unrelated writes, notifications without a changed bit, two consumers
sharing a signal word, task 127, and repeated watch lifetimes. A test-side wrapper
holds the worker immediately before the real POSIX condition wait to put a
producer in the check-to-sleep window. Generic watch removal is checked for both
head and middle nodes, with writes after removal. Test processes have a bounded
deadline. Windows follows the same SRW-lock/condition-variable ordering but the
POSIX thread fixture is skipped there.

The branch is based on independent context-registry commit
`383d4413a73b66e9761101ac46f2569853231fb7`, directly above pinned base
`5244db92669b54831c6d3ebb7a96d58c4dbfa6df`, so its coherency traversal and lifetime
tests use the intended live-context registry. It carries no other integration
changes and no title addresses or title-specific workarounds.
