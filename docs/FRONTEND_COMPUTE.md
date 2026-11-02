# Explicit frontend compute interface

`libs/video/rsx_frontend.h` provides an opt-in, versioned compute interface. The initial
implementations use the existing Metal or Vulkan backend's device and command queue. They create no
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

Buffer creation copies complete initial bytes or initializes zero bytes into a persistent native
buffer. A full-buffer CPU update allocates a new version; it never modifies or reads the previous
version. Explicit GPU `READ_WRITE` bindings can mutate a native version, so its post-dispatch bytes
must not be mistaken for the CPU seed. Old public versions remain addressable until released.
Submissions independently retain each referenced version and pipeline through GPU terminal
completion, even if the public handle is released meanwhile. Version metadata is retired once
no public version remains addressable. Buffers and bound slice lengths must fit `u32`, meet
their schema minimum size, and respect the manifest alignment.

`rsx_frontend_pipeline_desc` selects MSL, HLSL or SPIR-V payload bytes explicitly. The initial
Metal implementation accepts MSL only and rejects the other targets as unsupported. Pipeline
creation performs actual runtime Metal library/function/pipeline compilation. Its binding schema
must come from the same generated shader manifest: slots, uniform/storage kind, access, minimum bytes, alignment,
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
pressure, original-work ordering gates, manifest alignment, unsupported headed arrays, retention
of prior CPU uploads across a real shared-event delay, exactly-once terminal polling, metadata
retirement, shutdown drain and stale context rejection. Diagnostic readback is used only after
terminal completion. These are synthetic Metal compute results, not complete scene, D3D12,
Vulkan, game visual or match-performance acceptance.

## Vulkan route and Linux verification

Set `PS3RECOMP_VK_FRONTEND=1` before the existing Vulkan backend initializes. This explicitly
requests Vulkan1.1 for the tool's SPIR-V1.3; ordinary initialization continues to request
Vulkan1.0 when the option is absent. Query `rsx_vulkan_backend_frontend_available()` before
borrowing `rsx_vulkan_backend_frontend()`. Both require the existing selected graphics queue
to support compute. No parallel device/queue or mandatory present/readback is created.

The adapter accepts SPIR-V only, checks its compute entry and literal local size, and uses
explicit uniform/storage descriptor kinds. Uniforms must be read-only. Offsets obey both
manifest and device descriptor alignment/range limits. Initial support requires host-visible
coherent memory and runtime arrays at byte zero; unavailable capabilities remain visible.
Each of two ticket slots owns persistent command/descriptor pools and a fence, reused only
after terminal polling. Submit returns BUSY while original commands are recording or present
recording ownership cannot be acquired immediately. Queue operations use the original mutex.

Normal submit/poll performs no device/queue idle wait, fence wait, frame readback or original
present call. Poll uses native fence status. Native errors retain uncertain tickets and block
new admission. Genuine valid terminal receipts are delivered first even if another fence
fails; the independent fault is reported later, never alongside a consumed once-only receipt.
Out-of-memory submit errors are definite rejection under the
[Vulkan queue-submit guarantee](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html);
other unknown/device-lost submit results retain an indeterminate ticket. Device loss does not
become a successful terminal. Explicit teardown drains the shared device before destruction.

From Linux with a C compiler, Python3, SDL2/Vulkan development headers and loader in pkg-config:

```sh
python3 tools/test_frontend_vulkan.py --contract-only
python3 tools/test_frontend_vulkan.py --shader-root <generated-shader-root> --build-only
python3 tools/test_frontend_vulkan.py --shader-root <generated-shader-root> --require-validation \
  --require-hardware --gpu-index <index> --vendor-id <PCI-vendor> --device-id <PCI-device> \
  --report build/frontend-vulkan-hardware.json
```

Hardware mode matches vendor/device IDs and rejects CPU, virtual and unspecified devices.
The no-window fixture uses one existing-owner device/queue. A host-signaled timeline semaphore
delays queue progress while two compute tickets retain released old CPU versions. Timeline
support is enabled only on the fixture device; normal backend requirements remain unchanged.
Required validation mode fails if the Khronos layer cannot initialize, and every VUID in the
captured output fails the runner before it writes a success receipt. It checks
exact transform/compaction outputs, zero/oversized bounds, ordering gates, alignment/layout
rejection, pressure, pending-read protection, new publication and exactly-once terminal polling.
The CPU-only driver-fault test injects fence responses without creating a GPU device. Receipts
bind SDK/shader source hashes and distinguish diagnostic-only from explicit hardware runs.

The dedicated Linux workflow builds with GCC/Clang and runs lavapipe diagnostics under validation
using pinned authored shader sources. A green software result is never hardware acceptance.
Full scenes, original-surface interop, presentation, fault injection, D3D12 and full M2 remain
separate obligations.

## Dedicated CI and shader provenance

The explicit native fixture runners require generated shader directories and platform tools;
generic `tools/test_*.py` discovery excludes those two runners. Dedicated workflows supply the
inputs and invoke strict checks rather than treating a missing argument or device as a pass.
The Metal workflow compiles the native fixture by default with `--build-only`, explicitly leaving
GPU execution pending. An explicitly requested manual hardware mode runs the actual fixture and
fails if no real device is available. The Vulkan workflow runs its CPU driver-fault regression
and software diagnostics with required Khronos validation; every VUID fails the native runner.

Both workflows pin the authored shader tool and WGSL to public source checkpoint
[`33f0ad6f26dc2d951a6b937c28a1b1383ae7ddd4`](https://github.com/tekulvw/mag-recomp/tree/33f0ad6f26dc2d951a6b937c28a1b1383ae7ddd4/crates/mag-render-shaders),
which locks Naga 30.0.1 and Rust 1.99.0. MSL 2.0, HLSL `cs_6_0`, SPIR-V 1.3/Vulkan 1.1 and binding/layout
metadata are explicit. Generated shader binaries/manifests belong to one source-bound set;
native test receipts retain their hashes independently of translated compiler metadata.
The workflows have read-only repository permissions and no release publication jobs.

The old host-set Vulkan event gate was invalid while a submitted command referenced it.
The supported fixture uses a timeline semaphore with its feature/extension enabled only on
the synthetic owner device. This does not raise normal Vulkan backend requirements. Native
hardware results must come from the corrected gate under required validation, not from the
older event fixture's output. No shader translation, software result or compile-only job
establishes real-match performance or full frontend acceptance.
