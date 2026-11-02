# Explicit frontend compute interface

`libs/video/rsx_frontend.h` provides an opt-in, versioned compute interface. The initial
implementation uses the existing Metal backend's device and command queue. It creates no
second renderer, window, device or queue and changes no original rendering defaults. Draws,
presentation and other APIs remain unsupported by this compute slice.

Obtain the borrowed context with `rsx_metal_backend_frontend()` after the original backend is
initialized. A null result means no usable context. It remains valid until backend shutdown;
do not retain/call the context pointer after shutdown. Serialize all frontend and original
backend calls on their existing owner. The API does not provide concurrent recording ownership.

Every record descriptor begins with `struct_size` and `abi_version`. Initialize them to the
exact C structure size and `RSX_FRONTEND_ABI_VERSION`. Reserved fields must be zero. Handles
are values qualified by the device generation; buffers also carry an ID and content version.
A newly acquired context after teardown rejects all earlier value handles.

## Buffers and pipelines

Buffer creation copies complete initial bytes or initializes zero bytes into a persistent shared
Metal buffer. A full-buffer update allocates a new immutable content version; it never modifies
or reads the previous version. Old public versions remain addressable until explicitly released.
Submissions independently retain each referenced version and pipeline through GPU terminal
completion, even if the public handle is released meanwhile. Version metadata is retired once
no public version remains addressable. Buffers and bound slice lengths must fit `u32`, meet
their schema minimum size, and respect the manifest alignment.

`rsx_frontend_pipeline_desc` selects MSL, HLSL or SPIR-V payload bytes explicitly. The initial
Metal implementation accepts MSL only and rejects the other targets as unsupported. Pipeline
creation performs actual runtime Metal library/function/pipeline compilation. Its binding schema
must come from the same generated shader manifest: slots, access, minimum bytes, alignment,
runtime-array stride and runtime-size index, plus declared threads per group.

Initial native support restricts runtime arrays to byte offset zero, with minimum binding size
equal to one complete stride. A struct header before a runtime array returns `UNSUPPORTED`.
The shader translator can describe such layouts; that translation does not establish native
support. Runtime sizes use dense indices. For Metal the adapter supplies their bound **byte
lengths** in a retained extra buffer at slot 30. User resource slots remain 0 through 15.

The caller must use metadata that matches the actual shader. Pipeline creation does not reflect
arbitrary MSL buffer member layouts or certify a caller-authored schema against arbitrary source.
The source-bound synthetic test consumes the translator's emitted manifest directly. A complete
integration must bind generated source/metadata provenance and map resource-store namespaces,
slot generations and content versions to these native handles.

## Submission, ordering and completion

`rsx_frontend_submit()` consumes descriptors synchronously and records an explicit compute
command buffer on the existing queue. It returns `BUSY` whenever any original engine records,
staging bytes, tail encoder, legacy records or legacy vertices remain uncommitted. The caller
must explicitly submit that original work first. Already committed original commands preserve
their order on the shared queue. This guard runs for **every** compute submission; the getter
alone establishes no ordering boundary. There is no implicit original flush, replay, fallback,
per-draw host fence or whole-frame readback.

At most two tickets remain pending, including terminal tickets awaiting poll. Read-only shared
buffer use can overlap; a pending writer blocks other use of that version. Writable aliasing
within a submission is rejected. Buffer readback is an explicit diagnostic API and returns
`BUSY` until all its uses are terminal and polled.

A negative submit result guarantees no compute commit. `OK` and `INDETERMINATE` return a
generation-qualified monotonic ticket. A commit exception is indeterminate only if the native
command buffer has reached committed status; not-enqueued/enqueued failures are removed from
pending. Consequently teardown never waits for an uncommitted indeterminate command.

Polling inspects actual native command-buffer terminal state. Zero events means pending, never
completion. Each terminal event is emitted once and states success/failure separately from
`no_further_gpu_access`. Both successful and failed terminal command buffers have ceased GPU
access; ordinary API/poll errors provide no such guarantee. Source publication and physical
presentation are distinct facts and no events are fabricated for them by this compute path.

Normal submit/poll never waits for GPU completion. Backend teardown explicitly waits for
already committed pending commands before releasing frontend state; callers must quiesce API
calls first. Device loss recovery, hardware fault injection, real draws/scenes/presentation,
cross-API execution and live game cutover remain pending.

## Source-bound synthetic verification

Generate both authored fixtures using the companion portable shader tool, retaining each
directory's `shader.metal` and `manifest.json`. Then run from this SDK source root:

```sh
python3 tools/test_frontend_metal.py --shader-root <generated-shader-root>
```

The source root contains `copy_transform/` and `compact/`. The fixture compiles against the
same SDK source, establishes the existing Metal owner fields like the SDK's isolated backend
tests, borrows the frontend, and asserts no layer, window or NSApplication was created. It
requires a real Metal device; a missing device is a failure, never software/null acceptance.
The subprocess has a 20-second run deadline. It verifies runtime shader compilation, exact
synthetic transform/compaction outputs, zero/oversized bounds, stable selected order, two-ticket
pressure, original-work ordering gates, manifest alignment, unsupported headed arrays, immutable
version retention across a real shared-event delay, exactly-once terminal polling, metadata
retirement, shutdown drain and stale context rejection. Diagnostic readback is used only after
terminal completion. These are synthetic Metal compute results, not complete scene, D3D12,
Vulkan, game visual or match-performance acceptance.
