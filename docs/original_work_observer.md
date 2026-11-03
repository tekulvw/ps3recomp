# Independent original execution witnesses

This diagnostic collector observes execution of declared original code ranges.
It is separate from replacement admission and outcome reporting. It neither
suppresses guest work nor establishes replacement correctness. Its synthetic
tests are not evidence that any title's original rendering work has been removed.

Configure the SDK with `-DPS3RECOMP_ORIGINAL_OBSERVER=ON`. Regenerate each observed
PPU/SPU provider with `--observe-original-work`; the independent leaf emitter has
the matching `observe_original_work=True` option. Observed generated sources
refuse compilation without the runtime compile flag. Normal generator output is
unchanged, and the interpreter/microstep macros compile away without that flag.
The focused runtime fixture additionally checks the normal interpreter object
has no undefined reference to `ps3_original_step`.

The SDK compile definition is private to the runtime target. Set it explicitly
on observed generated sources and the `ppu_loader.cpp` main scaffold, together
with the diagnostics include directory. Do not propagate diagnostic definitions
to every unrelated generated PPU translation unit: doing so needlessly changes
their compile commands and rebuilds the whole title.

The collector bounds source tables to64 sources,32 providers and64 targets.
Up to1024 EXPECTED/ACTIVE workers hold dense counter slots concurrently.
Immutable lifetime identities are retained separately: up to1,048,576 records
(32 MiB), with an8 MiB append-only hash index. Retirement archives only nonzero
counter cells into a16 MiB budget, then releases the dense slot without recycling
its lifetime identity. Active counters occupy about3.5 MiB; all storage together
is bounded to about59.52 MiB, with cold static pages touched as needed.

Sealed per-worker queries remain exact after slot reuse. Retired TLS retains its
old lifetime identity and cannot write into another worker's counter lease.
History or concurrent-slot exhaustion sets MISSING_WORKER. Archive exhaustion
sets OVERFLOW and pins the remaining dense evidence instead of discarding it.
These finite budgets require headroom checks for the declared live workload;
they are not an unlimited-duration guarantee.

No execution witness allocates, takes a lock, writes a file, polls a trigger or
formats output. Its private resolver must obey the same bounded/no-I/O contract.
Enrollment and retirement maintain the bounded history and sparse archive.
Diagnostic instruction coverage can be expensive; it is not a performance
benchmark or a normal rendering default.

Before starting workers, the private controller constructs a source census and
calls `ps3_original_arm`. The configuration includes an opaque manifest SHA256,
code-source SHA256/generation, provider-source SHA256, eligible route masks and
target PC intervals. The SDK copies these tables and never accepts a replacement
configuration. These hashes are supplied identity data, not cryptographic
authentication by this generic runtime. The private controller must bind them to
actual source/image/build receipts fixed before interception; candidate output
must not determine the target map.

The resolver receives the actual context, PC, execution route and compiled
provider call-site name. It must independently identify executing code and its
generation/provider. An image number alone is insufficient for overlapping or
mutable code. An unknown image, stale generation or unqualified provider must
return failure, including when it cannot be established whether the code is
targeted. Non-target code also needs a qualified source or independent exclusion
evidence. Unresolved code invalidates the capture rather than contributing an
eligible zero.

The host enrolls each expected native worker generation before that worker can
execute guest code. Each worker calls `ps3_original_worker_register` on itself.
New workers created during observation require explicit prior enrollment too;
there is no silent automatic enrollment. The same ID/generation cannot register
twice. These identities belong to actual host workers, not guest jobs or recycled
context addresses. Guest job/source generations remain separately qualified by
the resolver. Optional transaction begin/end scopes only annotate outside-scope
counts; work outside a replacement transaction always remains observed.

Generated original instructions are witnessed after normal entry admission, so
an early replacement return ahead of the original body does not count as original
execution. Direct calls, local labels, tail chains, region middle entries and
verified leaf bodies are covered by their generated instruction hooks. The full
interpreter observes after rejoin/stop checks, immediately before actual stepping;
a target reached later in one invocation is visible. The microstepper observes
only recognized instructions it actually executes, leaving unknown instructions
to the full interpreter. A collapsed no-op halt loop witnesses only its reachable
first traversal; a skipped PC receives no fabricated witness. Unqualified exits
from that shortcut invalidate coverage.

Every witness inside a target range increments that target/route counter. A body
with many executed instructions may produce many witnesses; these are not exact
distinct function-entry counts, CPU shares or FPS. The separately aggregated
outside-transaction route is additional evidence rather than a substitute for
specific route coverage. Unknown routes, missing workers, counter overflow,
explicit lost records, late arming and work after retirement set sticky faults.
Faults cannot be reset to obtain a clean receipt.

`ps3_original_begin_drain` closes enrollment but keeps recording. Each worker must
retire only after its actual last possible guest execution, including delayed
work through the drain boundary. `ps3_original_seal` refuses while any registered
worker is active; an enrolled worker that never registered sets a missing-worker
fault. The host must also supply real join/quiescence evidence and ensure no new
or unregistered worker can enter afterward. A sealed snapshot alone does not
prove that orchestration, and a later observed entry remains an integrity fault.

Read final counters only after sealing and reject any report with faults or lost
records. This module does not emit the public Rust `ObserverReceipt` schema.
That schema conservatively requires all inventoried routes; a source-bound
non-applicability record or a strict schema extension is required for a route
that does not exist. Do not convert absent instrumentation into enabled zero
counters. Actual private range/provider qualification, worker lifecycle hookup
and authenticated drain remain integration requirements before removal proof.

The native POSIX factory adapter enrolls before actual `pthread_create`,
registers before the original function, and retires through a pthread cleanup
handler after normal return or `pthread_exit`. It preserves arguments, result,
attributes and errno. A genuine creation failure cancels only the never-started
expected generation; it cannot retire an active worker. At most the fixed worker
capacity of small setup records can be allocated on the creation path, and each
is freed at entry or creation failure. Instruction observations still allocate
nothing. Normal builds macro-expand directly to the original pthread factory.

The actual POSIX PPU factory, SPU group/async factories and POSIX Win32-compatible
`CreateThread` implementation use that adapter. This includes persistent SPURS
workers and native guest-callback threads created through the shim. Main guest
execution has separate fixed host enrollment of ID1/generation1 before
`ppu_run`; its real body entry registers, and return/unwind retires it. Unexpected
worker routes still invalidate rather than auto-enroll at an instruction.

Observed Windows runtime/scaffold sources route the actual `CreateThread` and
CRT `_beginthreadex` calls through the native Windows adapter. It enrolls before
creation and registers on the new worker before its original body. Security,
stack reservation, suspended creation, thread IDs, return handles, arguments and
exit codes pass through the real APIs. Normal return and explicitly wrapped
`ExitThread`/`_endthreadex` retire after the last original body execution. Forced
termination, unhandled exceptions and unwrapped early exits do not establish
retirement; retain incomplete capture state. A genuine creation failure cancels
only the never-started enrollment. Observer/setup allocation failure preserves
the original native factory call and invalidates proof instead of suppressing
the guest thread. Normal builds retain direct Windows/CRT calls with no wrapper.
The Windows fixture checks both factories, genuine suspended/resume behavior,
normal return and explicit exit with two observed witnesses through drain.
Cross-linking that fixture alone is not evidence of Windows execution or a
client's complete thread lifecycle.

`ps3_original_live_status` and `ps3_original_live_total` are diagnostic-only APIs.
Their report deliberately has no drained-through field. Per-worker single-writer
counters use relaxed atomic loads/stores. Each accepted witness also updates a
checked aggregate using atomic compare-and-exchange; live-total queries therefore
do not scan lifetime history. Aggregate overflow remains a sticky fault and
returns saturated evidence with an error. Reading while active is safe but does
not form a synchronized frame snapshot, prove zero original execution or
establish worker quiescence.
Persistent workers may remain active when a bounded client test ends; retain
that incomplete state instead of manufacturing a sealed removal receipt. The
adapter adds no new guest stop protocol.

Focused commands (no game, device or window):

```
python tools/test_original_work_observer.py
python tools/test_original_work_threads.py
python tools/test_original_work_routes.py
python tools/test_ppu_original_work.py
python tools/test_original_work_routes.py --negative-interpreter-control
python tools/test_original_work_routes.py --negative-halt-control
```

The first fixture exercises actual native threads, immutable configuration,
generations, late work, faults and concurrent arm/register/drain. The SPU fixture
compiles and executes actual authored generated functions and real runtime
interpreter/microstep/region/leaf code. The PPU fixture executes a target inside a
generated function and compares its guest result with normal generation. Negative
controls alter only isolated temporary source copies: omitting the real
interpreter witness or fabricating a skipped halt-PC witness must fail after the
exact guest result has already been checked. `--compile-only` is an explicitly
separate cross-build mode and never claims execution. The dedicated CI workflow
runs the execution checks on all three host operating systems; Windows uses
clang-cl for the collector/native Windows lifecycle and MinGW/winpthreads for
the actual SPU runtime fixture. Actual Windows lifecycle acceptance remains
pending until that dedicated job executes successfully.
