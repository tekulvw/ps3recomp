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

The collector uses fixed storage: at most64 sources,32 providers,64 targets and64
worker generations. Worker counters occupy224 KiB. No worker event allocates,
locks, writes a file, polls a trigger, or formats output. A private resolver
callback must obey the same bounded/no-I/O contract. Diagnostic instruction
coverage can be expensive; it is not a performance benchmark or a normal
rendering default.

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

Read counters only after sealing and reject any report with faults or lost
records. This module does not emit the public Rust `ObserverReceipt` schema.
That schema conservatively requires all inventoried routes; a source-bound
non-applicability record or a strict schema extension is required for a route
that does not exist. Do not convert absent instrumentation into enabled zero
counters. Actual private range/provider qualification, worker lifecycle hookup
and authenticated drain remain integration requirements before removal proof.

Focused commands (no game, device or window):

```
python tools/test_original_work_observer.py
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
clang-cl for the collector and MinGW/winpthreads for the actual runtime fixture.
