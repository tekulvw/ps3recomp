# Draw-request observer

The existing pass capture records draws accepted by the backend. This separate
observer records each matching `BEGIN_END` group reaching the draw engine,
including groups rejected before vertex fetch, topology rebuilding or pipeline
creation. It identifies whether missing geometry reached this boundary.

It is disabled by default. Enable it with these environment variables:

| Variable | Meaning |
| --- | --- |
| `RSX_ENGINE_DRAW_OBSERVER` | Output JSONL file path. Its parent directory must exist. |
| `RSX_ENGINE_DRAW_OBSERVER_TRIGGER` | Optional file whose existence arms capture. The observer does not modify it. |
| `RSX_ENGINE_DRAW_OBSERVER_TARGET` | Optional color-A offset filter, parsed as a C integer (`0x...` for hex). |
| `RSX_ENGINE_DRAW_OBSERVER_MAX` | Maximum records. Default 256, hard maximum 4096. Zero disables capture. |

Waiting for the trigger and requests for other color-A offsets consume no
records. The output opens on the first matching armed request; an existing
output file is truncated at that point. Each engine lifetime starts a fresh
observer. After reaching its cap, it closes the file and performs no further
capture IO. The renderer owner thread owns the observer, just as it owns the
draw engine; it is not a concurrent external inspection interface.

Each line is one outcome. `emitted` means the backend draw callback ran;
`empty_packets`, `topology`, `fetch`, `index_topology`, `zero_geometry`,
`targets`, `pipeline`, `index_allocation` and `constant_allocation` identify
where the request returned. `not_ready` identifies an uninitialized engine.
An outcome is not a GPU completion or a proof that pixels passed clipping,
culling, depth/stencil, or fragment tests.

Records include the frame, primitive, batch counts, requested array vertices
and indices, fetched vertex/reference counts, fetch status, index-array state,
vertex program start, fragment program address/control, color offsets and
locations, target selector, clip size, and all sixteen vertex-array format and
offset register pairs. The target filter refers to color A; the recorded
selector and offsets allow readers to distinguish other attachment selections.
The observer reads only the draw accumulator and register file. It does not
resolve or read guest memory, compile a shader, query the backend, or alter a
draw's rejection logic. It does not dump programs, constants, textures or assets.

One `fwrite` emits the complete JSONL record, followed by a flush. A diagnostic
open/write/flush failure disables capture. Diagnostic IO preserves `errno` and
never changes the original draw accumulator or backend state. A failed IO may
leave a partial final line; discard that line when reading the receipt.

The original draw handler is delegated to by an outer observer wrapper. Stage
markers only assign observer state when capture is active. Removing the marker
lines and restoring the handler's name reproduces the original handler body
byte for byte.

Run the synthetic test without a graphics API:

```sh
cc -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I include -I libs/video \
  -o /tmp/test_draw_observer libs/video/tests/test_draw_observer.c \
  libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_texture_layout.c \
  libs/video/rsx_vp_decompiler.c libs/video/rsx_fp_decompiler.c
/tmp/test_draw_observer
```

The fixture uses synthetic requests and the existing stub backend. It compares
complete backend state, guest-memory read counts, guest bytes and `errno` with
capture off, with capture on, and with injected open and write failures. It
also checks arming, filtering, caps, exact rejection outcomes and empty groups.
